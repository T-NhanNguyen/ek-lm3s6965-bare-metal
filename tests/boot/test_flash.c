#include "lm3s6965/boot_flash.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_store.h"

typedef struct
{
    uint32_t address, value;
    unsigned kind;
} event_t;

typedef struct
{
    uint8_t bytes[0x40000];
    uint32_t fma, fmd, fmc, ris, usecrl, completion;
    unsigned delay, remaining, calls, fail, after, corrupt, sticky, commands;
    unsigned reenter, count;
    unsigned command_call, command_delay;
    uint32_t command_completion;
    boot_flash_t *flash;
    boot_flash_erase_plan_t *plan;
    event_t events[4096];
} mock_t;

static mock_t m;
static boot_flash_t f;

static int event(mock_t *mock, unsigned kind, uint32_t address, uint32_t value)
{
    assert(mock->count < 4096u);
    mock->events[mock->count++] = (event_t){address, value, kind};
    ++mock->calls;
    if (mock->reenter != 0u)
    {
        mock->reenter = 0u;
        assert(boot_flash_erase(mock->flash, BOOT_METADATA_PAGE0) == BOOT_FLASH_BUSY);
        assert(boot_flash_recover(mock->flash) == BOOT_FLASH_BUSY);
        if (mock->plan != NULL)
            assert(boot_flash_erase_step(mock->flash, mock->plan) == BOOT_FLASH_BUSY);
    }
    return mock->calls == mock->fail;
}

static void complete(mock_t *mock)
{
    unsigned i;
    if ((mock->completion & 1u) == 0u)
    {
        if (mock->fmc == 2u) memset(mock->bytes + mock->fma, 255, 1024);
        else for (i = 0; i < 4; ++i)
            mock->bytes[mock->fma + i] &= (uint8_t)(mock->fmd >> (8u * i));
    }
    mock->fmc = 0;
    mock->ris |= mock->completion;
}

static int read_register(void *context, uint32_t address, uint32_t *value)
{
    mock_t *mock = context;
    int fail = event(mock, 0u, address, 0u);
    if (fail && mock->after == 0u) return -1;
    switch (address)
    {
        case BOOT_FLASH_FMC:
            if (mock->fmc != 0u && mock->remaining != 0u && --mock->remaining == 0u)
                complete(mock);
            *value = mock->fmc;
            break;
        case BOOT_FLASH_FCRIS: *value = mock->ris; break;
        case BOOT_FLASH_USECRL: *value = mock->usecrl; break;
        default: assert(0); break;
    }
    return fail ? -1 : 0;
}

static int write_register(void *context, uint32_t address, uint32_t value)
{
    mock_t *mock = context;
    int fail = event(mock, 1u, address, value);
    if (fail && mock->after == 0u) return -1;
    switch (address)
    {
        case BOOT_FLASH_FCMISC:
            assert(value == 3u);
            if (mock->sticky == 0u) mock->ris &= ~value;
            break;
        case BOOT_FLASH_FMD: mock->fmd = value; break;
        case BOOT_FLASH_FMA:
            assert(value < 0x40000u);
            mock->fma = value;
            break;
        case BOOT_FLASH_FMC:
            assert(mock->fmc == 0u);
            assert(value == UINT32_C(0xa4420001) || value == UINT32_C(0xa4420002));
            assert(mock->ris == 0u);
            mock->fmc = value & 3u;
            if (mock->calls == mock->command_call)
            {
                mock->delay = mock->command_delay;
                mock->completion = mock->command_completion;
            }
            mock->remaining = mock->delay;
            ++mock->commands;
            break;
        default: assert(0); break;
    }
    return fail ? -1 : 0;
}

static int read_flash(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    mock_t *mock = context;
    int fail = event(mock, 2u, address, (uint32_t)size);
    if (fail && mock->after == 0u) return -1;
    assert(mock->fmc == 0u);
    assert(address <= sizeof mock->bytes && size <= sizeof mock->bytes - address);
    memcpy(bytes, mock->bytes + address, size);
    if (mock->calls == mock->corrupt) bytes[size - 1u] ^= 1u;
    return fail ? -1 : 0;
}

static void setup(void)
{
    boot_flash_io_t io = { &m, read_register, write_register, read_flash };
    memset(&m, 0, sizeof m);
    memset(m.bytes, 255, sizeof m.bytes);
    m.usecrl = 7;
    m.delay = 1;
    m.completion = 2;
    m.flash = &f;
    assert(boot_flash_init(&f, &io, 8, 3) == BOOT_FLASH_OK);
}

static void primitive(void)
{
    uint8_t bytes[8];
    unsigned n, after, total;
    event_t trace[32];
    setup();
    m.ris = 3; m.delay = 3; m.reenter = 1;
    assert(boot_flash_program(&f, BOOT_METADATA_PAGE0, UINT32_C(0x12345678)) == BOOT_FLASH_OK);
    assert(m.bytes[BOOT_METADATA_PAGE0] == 0x78);
    assert(m.commands == 1u);
    memcpy(trace, m.events, m.count * sizeof trace[0]);
    total = m.calls;
    assert(trace[3].address == BOOT_FLASH_FCMISC && trace[3].value == 3u);
    assert(trace[5].address == BOOT_FLASH_FMD);
    assert(trace[6].address == BOOT_FLASH_FMA);
    assert(trace[7].address == BOOT_FLASH_FMC && trace[7].value == UINT32_C(0xa4420001));
    assert(boot_flash_program(&f, BOOT_METADATA_PAGE0, UINT32_MAX) == BOOT_FLASH_BITS);
    assert(m.commands == 1u);
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_OK);
    assert(boot_flash_read(&f, BOOT_METADATA_PAGE0, bytes, sizeof bytes) == BOOT_FLASH_OK);
    for (n = 0; n < sizeof bytes; ++n) assert(bytes[n] == 255u);
    /* STE: every callback boundary, before and after possible effect. */
    for (after = 0; after < 2; ++after)
        for (n = 1; n <= total; ++n)
        {
            unsigned commands;
            setup(); m.delay = 3; m.fail = n; m.after = after;
            assert(boot_flash_program(&f, BOOT_METADATA_PAGE0, 0) == BOOT_FLASH_IO);
            commands = m.commands;
            m.fail = 0;
            if (f.blocked != 0u)
            {
                assert(boot_flash_erase(&f, BOOT_METADATA_PAGE1) == BOOT_FLASH_BLOCKED);
                assert(boot_flash_read(&f, BOOT_METADATA_PAGE0, bytes, 4) == BOOT_FLASH_BLOCKED);
                assert(m.commands == commands);
                assert(boot_flash_recover(&f) == BOOT_FLASH_OK);
            }
        }
    setup();
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_OK);
    total = m.calls;
    for (after = 0; after < 2; ++after)
        for (n = 1; n <= total; ++n)
        {
            setup(); m.fail = n; m.after = after;
            assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_IO);
        }
    setup(); m.delay = 4;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_TIMEOUT);
    assert(m.fmc == 2u);
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE1) == BOOT_FLASH_BLOCKED);
    assert(m.commands == 1u);
    assert(boot_flash_recover(&f) == BOOT_FLASH_OK);
    setup(); m.delay = 0;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_TIMEOUT);
    assert(boot_flash_recover(&f) == BOOT_FLASH_TIMEOUT);
    assert(m.commands == 1u);
    setup(); m.fmc = 8;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_TIMEOUT);
    assert(m.commands == 0u);
    setup(); m.ris = 3; m.sticky = 1;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_STATUS);
    assert(m.commands == 0u);
    setup(); m.completion = 1;
    assert(boot_flash_program(&f, BOOT_METADATA_PAGE0, 0) == BOOT_FLASH_PROTECTED);
    setup(); m.completion = 3;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_PROTECTED);
    setup(); m.completion = 0;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_STATUS);
    setup();
    assert(boot_flash_program(&f, BOOT_METADATA_PAGE0, 0) == BOOT_FLASH_OK);
    total = m.calls;
    setup(); m.corrupt = total;
    assert(boot_flash_program(&f, BOOT_METADATA_PAGE0, 0) == BOOT_FLASH_VERIFY);
    setup();
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_OK);
    total = m.calls;
    setup(); m.corrupt = total;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_VERIFY);
    setup(); m.usecrl = 49;
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_CLOCK);
    assert(m.commands == 0u);
    m.usecrl = UINT32_C(0xabcd0007);
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_OK);
    setup();
    assert(boot_flash_program(&f, BOOT_METADATA_PAGE0 + 1u, 0) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0 + 4u) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase(&f, 0) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_program(&f, BOOT_SLOT_B_END, 0) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_program(&f, UINT32_MAX - 3u, 0) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_read(&f, BOOT_METADATA_PAGE0, bytes, SIZE_MAX) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_read(&f, BOOT_SLOT_B_END - 4u, bytes, 8) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_read(&f, 0, bytes, 4) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_read(&f, BOOT_METADATA_PAGE0, NULL, 4) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_read(&f, BOOT_METADATA_PAGE0, bytes, 0) == BOOT_FLASH_ARGUMENT);
    assert(m.calls == 0u);
    assert(boot_flash_erase(NULL, BOOT_METADATA_PAGE0) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_recover(NULL) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_init(NULL, &f.io, 8, 3) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_init(&f, NULL, 8, 3) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_init(&f, &f.io, 0, 3) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_init(&f, &f.io, 51, 3) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_init(&f, &f.io, 8, 0) == BOOT_FLASH_ARGUMENT);
    {
        boot_flash_io_t io = f.io;
        io.read_register = NULL;
        assert(boot_flash_init(&f, &io, 8, 3) == BOOT_FLASH_ARGUMENT);
        io = f.io; io.write_register = NULL;
        assert(boot_flash_init(&f, &io, 8, 3) == BOOT_FLASH_ARGUMENT);
        io = f.io; io.read_flash = NULL;
        assert(boot_flash_init(&f, &io, 8, 3) == BOOT_FLASH_ARGUMENT);
    }
    for (n = 1; n <= 2; ++n)
    {
        setup(); m.fail = n;
        assert(boot_flash_read(&f, BOOT_METADATA_PAGE0, bytes, 4) == BOOT_FLASH_IO);
        assert(f.blocked == (n == 1u ? 1u : 0u));
    }
    setup(); m.fmc = 4;
    assert(boot_flash_read(&f, BOOT_METADATA_PAGE0, bytes, 4) == BOOT_FLASH_TIMEOUT);
    assert(m.calls == 1u && f.blocked != 0u);
    setup();
    assert(boot_flash_program(&f, BOOT_SLOT_B_END - 4u, UINT32_C(0x01234567)) == BOOT_FLASH_OK);
    assert(boot_flash_erase(&f, BOOT_SLOT_B_END - BOOT_ERASE_BYTES) == BOOT_FLASH_OK);
    for (n = 1; n <= 3; ++n)
    {
        setup(); m.ris = n;
        assert(boot_flash_erase(&f, BOOT_METADATA_PAGE0) == BOOT_FLASH_OK);
        assert(m.commands == 1u);
    }
    for (n = 1; n <= 3; ++n)
    {
        setup(); m.fail = n;
        assert(boot_flash_recover(&f) == BOOT_FLASH_IO);
        assert(f.blocked != 0u);
    }
    setup(); m.ris = 3; m.sticky = 1;
    assert(boot_flash_recover(&f) == BOOT_FLASH_STATUS);
}

static boot_metadata_t fixture(void)
{
    boot_metadata_t record = {0};
    record.sequence = 10;
    record.current_slot = BOOT_SLOT_A;
    record.candidate_slot = record.previous_slot = record.intent_slot = BOOT_SLOT_NONE;
    record.confirmed_mask = BOOT_CONFIRMED_A;
    record.images[0].payload_length = BOOT_PAYLOAD_MIN;
    record.images[0].payload_hash[0] = 42;
    return record;
}

static void store_setup(unsigned source, boot_store_snapshot_t *expected)
{
    boot_metadata_t record = fixture();
    boot_store_adapter_t adapter;
    setup();
    assert(boot_metadata_encode(&record, BOOT_COMMIT_VALID,
        m.bytes + BOOT_METADATA_PAGE0 + source * BOOT_ERASE_BYTES,
        BOOT_METADATA_BYTES) == BOOT_OK);
    memset(m.bytes + BOOT_METADATA_PAGE0 + (1u - source) * BOOT_ERASE_BYTES, 0,
           BOOT_ERASE_BYTES);
    adapter = boot_flash_store_adapter(&f);
    assert(boot_store_load(&adapter, expected) == BOOT_STORE_OK);
    m.calls = m.count = 0;
}

static void store(void)
{
    unsigned source, total, commit, commit_command, commit_readback, n, after, fault;
    uint8_t old[BOOT_ERASE_BYTES], wanted[BOOT_METADATA_BYTES], loaded_bytes[BOOT_METADATA_BYTES], bytes[4];
    boot_store_snapshot_t expected, out, sentinel, loaded;
    boot_store_adapter_t adapter;
    boot_metadata_t desired;
    for (source = 0; source < 2; ++source)
    {
        uint32_t old_address = BOOT_METADATA_PAGE0 + source * BOOT_ERASE_BYTES;
        uint32_t destination = BOOT_METADATA_PAGE0 + (1u - source) * BOOT_ERASE_BYTES;
        store_setup(source, &expected);
        memcpy(old, m.bytes + old_address, sizeof old);
        desired = expected.record; ++desired.sequence; desired.images[0].release = 123;
        assert(boot_metadata_encode(&desired, BOOT_COMMIT_VALID, wanted, sizeof wanted) == BOOT_OK);
        adapter = boot_flash_store_adapter(&f);
        assert(boot_store_replace(&adapter, &expected, &desired, &out) == BOOT_STORE_OK);
        total = m.calls; commit = commit_command = commit_readback = 0;
        for (n = 0; n < m.count; ++n)
        {
            if (m.events[n].kind == 1u && m.events[n].address == BOOT_FLASH_FMA)
            {
                assert(m.events[n].value >= destination && m.events[n].value < destination + BOOT_ERASE_BYTES);
                if (m.events[n].value == destination + 164u)
                {
                    commit = n + 1u - 6u;
                    assert(m.events[n+1u].address == BOOT_FLASH_FMC);
                    commit_command = n + 2u;
                    commit_readback = commit_command + 3u;
                    assert(m.events[commit_readback - 1u].kind == 2u);
                    assert(m.events[commit_readback - 1u].address == destination + 164u);
                    assert(m.events[commit_readback - 1u].value == BOOT_PROGRAM_BYTES);
                    /* Commit is the final controller command, after all 41 staging words. */
                    assert(m.commands == 43u);
                }
            }
        }
        assert(commit != 0u && out.page == 1u - source);
        assert(memcmp(old, m.bytes + old_address, sizeof old) == 0);
        assert(memcmp(wanted, m.bytes + destination, sizeof wanted) == 0);
        memset(&sentinel, 0xa5, sizeof sentinel);
        for (after = 0; after < 2; ++after)
            for (n = 1; n <= total; ++n)
            {
                boot_store_result_t result;
                store_setup(source, &expected);
                adapter = boot_flash_store_adapter(&f);
                m.fail = n; m.after = after; out = sentinel;
                result = boot_store_replace(&adapter, &expected, &desired, &out);
                assert(result == (n >= commit ? BOOT_STORE_UNCERTAIN : BOOT_STORE_IO));
                assert(memcmp(&out, &sentinel, sizeof out) == 0);
                assert(memcmp(old, m.bytes + old_address, sizeof old) == 0);
                m.fail = 0;
                assert(boot_flash_recover(&f) == BOOT_FLASH_OK);
                assert(boot_store_load(&adapter, &loaded) == BOOT_STORE_OK);
                assert(loaded.record.sequence == expected.record.sequence || loaded.record.sequence == desired.sequence);
                if (loaded.record.sequence == desired.sequence)
                    assert(memcmp(wanted, m.bytes + destination, sizeof wanted) == 0);
            }
        /* Commit-word controller failures compose to UNCERTAIN, not store IO.
         * Exercise late/stuck timeout, ARIS, missing PRIS, and word readback
         * corruption separately; earlier staging commands remain successful. */
        for (fault = 0; fault < 5; ++fault)
        {
            unsigned calls;
            store_setup(source, &expected);
            adapter = boot_flash_store_adapter(&f);
            m.command_call = commit_command;
            m.command_delay = fault == 0u ? 4u : (fault == 1u ? 0u : 1u);
            m.command_completion = fault == 2u ? 1u : (fault == 3u ? 0u : 2u);
            if (fault == 4u) m.corrupt = commit_readback;
            out = sentinel;
            assert(boot_store_replace(&adapter, &expected, &desired, &out) == BOOT_STORE_UNCERTAIN);
            assert(memcmp(&out, &sentinel, sizeof out) == 0);
            assert(memcmp(old, m.bytes + old_address, sizeof old) == 0);
            assert(m.commands == 43u && f.blocked != 0u);
            assert(m.fmc == (fault < 2u ? 1u : 0u));
            if (fault < 3u)
                for (n = 0; n < BOOT_PROGRAM_BYTES; ++n)
                    assert(m.bytes[destination + BOOT_METADATA_BYTES - BOOT_PROGRAM_BYTES + n] == 255u);
            else
                assert(memcmp(wanted, m.bytes + destination, sizeof wanted) == 0);

            /* Quarantine prevents reads or further writes until explicit recovery. */
            calls = m.calls;
            loaded = sentinel;
            assert(boot_store_load(&adapter, &loaded) == BOOT_STORE_IO);
            assert(memcmp(&loaded, &sentinel, sizeof loaded) == 0);
            assert(boot_flash_read(&f, old_address, bytes, sizeof bytes) == BOOT_FLASH_BLOCKED);
            assert(boot_flash_program(&f, destination, 0) == BOOT_FLASH_BLOCKED);
            assert(boot_flash_erase(&f, old_address) == BOOT_FLASH_BLOCKED);
            assert(m.calls == calls && m.commands == 43u);
            if (fault == 1u)
            {
                assert(boot_flash_recover(&f) == BOOT_FLASH_TIMEOUT);
                assert(f.blocked != 0u && m.commands == 43u);
                /* Allow the outstanding command to finish on the next poll. */
                m.remaining = 1u;
            }
            assert(boot_flash_recover(&f) == BOOT_FLASH_OK);
            assert(f.blocked == 0u && m.fmc == 0u && m.ris == 0u);
            assert(m.commands == 43u);
            assert(memcmp(old, m.bytes + old_address, sizeof old) == 0);
            assert(boot_store_load(&adapter, &loaded) == BOOT_STORE_OK);
            assert(loaded.page == (fault == 2u ? source : 1u - source));
            assert(loaded.record.sequence == (fault == 2u ? expected.record.sequence : desired.sequence));
            assert(boot_metadata_encode(&loaded.record, BOOT_COMMIT_VALID,
                loaded_bytes, sizeof loaded_bytes) == BOOT_OK);
            assert(memcmp(loaded_bytes, fault == 2u ? old : wanted, sizeof loaded_bytes) == 0);
        }
        /* Staging/full-record readback corruption must stop, with commit invocation
         * onward remaining UNCERTAIN even when the physical command completed. */
        store_setup(source, &expected);
        adapter = boot_flash_store_adapter(&f);
        m.corrupt = commit - 1u;
        out = sentinel;
        assert(boot_store_replace(&adapter, &expected, &desired, &out) == BOOT_STORE_VERIFY);
        assert(memcmp(&out, &sentinel, sizeof out) == 0);
        assert(memcmp(old, m.bytes + old_address, sizeof old) == 0);
        assert(f.blocked == 0u);
        assert(boot_store_load(&adapter, &loaded) == BOOT_STORE_OK);
        assert(loaded.page == source && loaded.record.sequence == expected.record.sequence);
        store_setup(source, &expected);
        adapter = boot_flash_store_adapter(&f);
        m.corrupt = total - 4u; /* full committed readback (before final load) */
        out = sentinel;
        assert(boot_store_replace(&adapter, &expected, &desired, &out) == BOOT_STORE_UNCERTAIN);
        assert(memcmp(&out, &sentinel, sizeof out) == 0);
        assert(memcmp(old, m.bytes + old_address, sizeof old) == 0);
        assert(memcmp(wanted, m.bytes + destination, sizeof wanted) == 0);
        assert(f.blocked == 0u); /* Store-level corruption does not quarantine the controller. */
        assert(boot_store_load(&adapter, &loaded) == BOOT_STORE_OK);
        assert(loaded.page == 1u - source && loaded.record.sequence == desired.sequence);
        assert(boot_metadata_encode(&loaded.record, BOOT_COMMIT_VALID,
            loaded_bytes, sizeof loaded_bytes) == BOOT_OK);
        assert(memcmp(loaded_bytes, wanted, sizeof wanted) == 0);
    }
    setup(); adapter = boot_flash_store_adapter(&f);
    assert(adapter.read(adapter.context, BOOT_SLOT_A_BASE, bytes, sizeof bytes) != 0);
    assert(adapter.read(adapter.context, BOOT_SLOT_A_BASE - 2u, bytes, sizeof bytes) != 0);
    assert(adapter.erase_page(adapter.context, BOOT_SLOT_A_BASE) != 0);
    assert(adapter.program_word(adapter.context, BOOT_SLOT_A_BASE, 0) != 0);
    assert(adapter.program_word(adapter.context, UINT32_MAX - 3u, 0) != 0);
    assert(m.calls == 0u);
}

static void inactive(void)
{
    boot_flash_erase_plan_t plan;
    boot_store_snapshot_t snapshot = {0};
    unsigned destination, i, n, total, after, page;
    uint8_t metadata[2u * BOOT_ERASE_BYTES];
    for (destination = 0; destination < 2; ++destination)
    {
        uint32_t base = destination == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
        uint32_t manifest = destination == 0u ? BOOT_SLOT_A_MANIFEST : BOOT_SLOT_B_MANIFEST;
        uint32_t active = destination == 0u ? BOOT_SLOT_B_BASE : BOOT_SLOT_A_BASE;
        snapshot.record = fixture();
        if (destination == 0u)
        {
            snapshot.record.images[1] = snapshot.record.images[0];
            memset(&snapshot.record.images[0], 0, sizeof snapshot.record.images[0]);
            snapshot.record.current_slot = BOOT_SLOT_B;
            snapshot.record.confirmed_mask = BOOT_CONFIRMED_B;
        }
        setup();
        memset(m.bytes + base, 0, BOOT_SLOT_BYTES);
        memset(m.bytes + active, 0x5a, BOOT_SLOT_BYTES);
        memcpy(metadata, m.bytes + BOOT_METADATA_PAGE0, sizeof metadata);
        assert(boot_flash_erase_begin(&plan, &snapshot, 1u - destination, destination) == BOOT_FLASH_OK);
        m.reenter = 1; m.plan = &plan;
        for (i = 0; i < BOOT_SLOT_BYTES / BOOT_ERASE_BYTES; ++i)
        {
            unsigned first = m.count;
            assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_OK);
            assert(plan.page == i + 1u);
            for (n = first; n < m.count; ++n)
                if (m.events[n].kind == 1u && m.events[n].address == BOOT_FLASH_FMA)
                    assert(m.events[n].value == (i == 0u ? manifest : base + (i - 1u) * BOOT_ERASE_BYTES));
        }
        assert(m.commands == 110u);
        total = m.calls / 110u;
        assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_DONE);
        assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_DONE);
        assert(m.commands == 110u);
        for (i = 0; i < BOOT_SLOT_BYTES; ++i)
        {
            assert(m.bytes[base + i] == 255u);
            assert(m.bytes[active + i] == 0x5a);
        }
        assert(memcmp(metadata, m.bytes + BOOT_METADATA_PAGE0, sizeof metadata) == 0);
        /* Fail every register/read boundary of the manifest and first payload
         * page, before/after effects. No failure may advance or issue next page. */
        for (page = 0; page < 2; ++page)
            for (after = 0; after < 2; ++after)
                for (n = 1; n <= total; ++n)
                {
                    unsigned commands;
                    setup();
                    memset(m.bytes + base, 0, BOOT_SLOT_BYTES);
                    assert(boot_flash_erase_begin(&plan, &snapshot, 1u - destination, destination) == BOOT_FLASH_OK);
                    if (page != 0u) assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_OK);
                    m.fail = m.calls + n; m.after = after;
                    assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_IO);
                    assert(plan.page == page && plan.failed != 0u);
                    commands = m.commands;
                    assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_BLOCKED);
                    assert(m.commands == commands);
                    if (page == 0u) assert(m.bytes[base] == 0u);
                }
        setup();
        assert(boot_flash_erase_begin(&plan, &snapshot, 1u - destination, destination) == BOOT_FLASH_OK);
        m.corrupt = total;
        assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_VERIFY);
        assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_BLOCKED);
        setup();
        assert(boot_flash_erase_begin(&plan, &snapshot, 1u - destination, destination) == BOOT_FLASH_OK);
        m.delay = 0;
        assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_TIMEOUT);
        assert(boot_flash_erase_step(&f, &plan) == BOOT_FLASH_BLOCKED);
        assert(m.commands == 1u);
    }
    snapshot.record = fixture();
    assert(boot_flash_erase_begin(NULL, &snapshot, 0, 1) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase_begin(&plan, NULL, 0, 1) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase_begin(&plan, &snapshot, 0, 0) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase_begin(&plan, &snapshot, 1, 0) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase_begin(&plan, &snapshot, 0, 2) == BOOT_FLASH_ARGUMENT);
    snapshot.page = 2;
    assert(boot_flash_erase_begin(&plan, &snapshot, 0, 1) == BOOT_FLASH_ARGUMENT);
    snapshot.page = 0;
    snapshot.record.images[1] = snapshot.record.images[0];
    assert(boot_flash_erase_begin(&plan, &snapshot, 0, 1) == BOOT_FLASH_ARGUMENT);
    snapshot.record.candidate_slot = 1;
    snapshot.record.images[1].accounting = BOOT_STATE_READY;
    snapshot.record.images[1].trial_id = snapshot.record.sequence;
    assert(boot_flash_erase_begin(&plan, &snapshot, 0, 1) == BOOT_FLASH_ARGUMENT);
    snapshot.record = fixture();
    snapshot.record.images[1] = snapshot.record.images[0];
    snapshot.record.previous_slot = 1;
    snapshot.record.confirmed_mask |= BOOT_CONFIRMED_B;
    assert(boot_flash_erase_begin(&plan, &snapshot, 0, 1) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase_step(NULL, &plan) == BOOT_FLASH_ARGUMENT);
    assert(boot_flash_erase_step(&f, NULL) == BOOT_FLASH_ARGUMENT);
}

int main(void)
{
    primitive();
    store();
    inactive();
    puts("flash primitive + store fault matrix + manifest-first inactive erase: PASS");
    return 0;
}
