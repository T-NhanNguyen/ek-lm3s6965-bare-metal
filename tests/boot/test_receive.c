#include "lm3s6965/boot_receive.h"
#include "lm3s6965/boot_watchdog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "verify_fixtures.h"

/* Real controller/store/lifecycle/verifier; only the register/flash device is
 * modeled. Events include failed callbacks, allowing exact stop-prefix checks. */
typedef struct { unsigned kind; uint32_t address, value; } event_t;
typedef struct {
    uint8_t bytes[0x40000];
    uint32_t fma, fmd, fmc, ris;
    event_t events[8192];
    unsigned calls, fail, after, commands, reentry, observer_abort;
    boot_flash_t flash;
    boot_receive_t receiver;
    boot_receive_acceptance_t output, sentinel;
    boot_metadata_t initial;
    uint32_t destination;
    unsigned mode, no_trace, bad_clock, stall, stall_command, controller_reader;
    boot_flash_result_t running_flash_result;
} model_t;
static model_t m;
static event_t baseline[8192];
static unsigned baseline_count;
#ifdef BOOT_RECEIVE_FAULT_INJECTION
static unsigned fail_alloc, live_allocations;
void *boot_verify_test_calloc(size_t count, size_t size);
void boot_verify_test_free(void *p);
void *boot_verify_test_calloc(size_t count, size_t size)
{
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    void *p = calloc(count, size);
    if (p) ++live_allocations;
    return p;
}
void boot_verify_test_free(void *p)
{
    if (p) { assert(live_allocations); --live_allocations; }
    free(p);
}
#endif
static uint32_t base(uint32_t slot) { return slot ? BOOT_SLOT_B_BASE : BOOT_SLOT_A_BASE; }
static uint32_t manifest_address(uint32_t slot) { return slot ? BOOT_SLOT_B_MANIFEST : BOOT_SLOT_A_MANIFEST; }
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8u*i));
}
static void payload(uint8_t *p, uint32_t slot, unsigned mode)
{
    unsigned n = mode == 1 ? BOOT_PAYLOAD_MAX : BOOT_PAYLOAD_MIN;
    for (unsigned i = 0; i < n; ++i) p[i] = (uint8_t)(i*17u + slot*31u);
    memset(p+100, 255, 8); memset(p+n-4, 255, 4);
    put32(p, mode == 2 ? BOOT_INITIAL_MSP-4u : BOOT_INITIAL_MSP);
    uint32_t vector = base(slot)+9u;
    if (mode == 3) vector = base(slot)+8u;
    if (mode == 4) vector = base(slot)-1u;
    if (mode == 5) vector = base(slot)+n+1u;
    if (mode == 6) vector = base(slot)+n-1u;
    put32(p+4, vector);
}
static void check_frozen(void) { assert(!memcmp(&m.output, &m.sentinel, sizeof m.output)); }
static void reentry(void)
{
    if (!m.reentry) return;
    check_frozen();
    size_t accepted = 99;
    boot_receive_config_t c = {0};
    assert(boot_receive_begin(&m.receiver, &c) == BOOT_RECEIVE_BUSY);
    assert(boot_receive_feed(&m.receiver, NULL, 0, &accepted) == BOOT_RECEIVE_BUSY);
    assert(accepted == 99);
    assert(boot_receive_step(&m.receiver, &m.output) == BOOT_RECEIVE_BUSY);
    assert(boot_receive_eof(&m.receiver) == BOOT_RECEIVE_BUSY);
    assert(boot_receive_abort(&m.receiver) == BOOT_RECEIVE_BUSY);
}
static int event(unsigned kind, uint32_t address, uint32_t value)
{
    reentry();
    if (!m.no_trace) {
        assert(m.calls < 8192);
        m.events[m.calls] = (event_t){kind, address, value};
    }
    ++m.calls;
    return m.calls == m.fail;
}
static int read_register(void *ctx, uint32_t address, uint32_t *value)
{
    assert(ctx == &m);
    int fail = event(0, address, 0);
    if (fail && !m.after) return -1;
    switch (address) {
    case BOOT_FLASH_USECRL: *value = m.bad_clock ? 6u : 7u; break;
    case BOOT_FLASH_FCRIS: *value = m.ris; break;
    case BOOT_FLASH_FMC:
        if (m.stall) { *value = 2; break; }
        if (m.fmc) {
            if (m.fmc == 2) memset(m.bytes+m.fma, 255, BOOT_ERASE_BYTES);
            else for (unsigned i = 0; i < 4; ++i) m.bytes[m.fma+i] &= (uint8_t)(m.fmd >> (8u*i));
            m.fmc = 0; m.ris = 2;
        }
        *value = m.fmc; break;
    default: assert(0);
    }
    return fail ? -1 : 0;
}
static int write_register(void *ctx, uint32_t address, uint32_t value)
{
    assert(ctx == &m);
    int fail = event(1, address, value);
    if (fail && !m.after) return -1;
    switch (address) {
    case BOOT_FLASH_FCMISC: assert(value == 3); m.ris &= ~value; break;
    case BOOT_FLASH_FMA: m.fma = value; break;
    case BOOT_FLASH_FMD: m.fmd = value; break;
    case BOOT_FLASH_FMC:
        assert(!m.fmc && !m.ris);
        assert(value == UINT32_C(0xa4420001) || value == UINT32_C(0xa4420002));
        assert(m.fma >= BOOT_METADATA_PAGE0 && m.fma < BOOT_SLOT_B_END);
        m.fmc = value & 3u; ++m.commands;
        if (m.stall_command) m.stall = 1;
        break;
    default: assert(0);
    }
    return fail ? -1 : 0;
}
static int read_flash(void *ctx, uint32_t address, uint8_t *out, size_t n)
{
    assert(ctx == &m);
    int fail = event(2, address, (uint32_t)n);
    if (fail && !m.after) return -1;
    assert(!m.fmc && address <= sizeof m.bytes && n <= sizeof m.bytes-address);
    memcpy(out, m.bytes+address, n);
    return fail ? -1 : 0;
}
static size_t image_read(void *ctx, uint32_t address, uint8_t *out, size_t n)
{
    return read_flash(ctx, address, out, n) ? 0 : n;
}
static size_t controller_image_read(void *ctx, uint32_t address, uint8_t *out, size_t n)
{
    assert(ctx == &m);
    boot_flash_result_t result = boot_flash_read(&m.flash, address, out, n);
    if (m.running_flash_result == BOOT_FLASH_OK) m.running_flash_result = result;
    return result == BOOT_FLASH_OK ? n : 0;
}
static int running(void *ctx, boot_verified_image_t *out)
{
    assert(ctx == &m); reentry();
    uint32_t slot = m.destination ^ 1u;
    if (m.controller_reader) {
        if (m.controller_reader == 1) m.fail = m.calls+1;
        else m.stall = 1;
    }
    return boot_verify_stored(m.bytes+manifest_address(slot), BOOT_MANIFEST_BYTES,
        slot, m.controller_reader ? controller_image_read : image_read,
        &m, NULL, NULL, out) != BOOT_VERIFY_OK;
}
static int progress(void *ctx, boot_verify_phase_t phase, uint32_t done, uint32_t total)
{
    assert(ctx == &m); (void)phase; (void)done; (void)total; reentry();
    return !m.observer_abort;
}
static void encode_initial(void)
{
    memset(m.bytes+BOOT_METADATA_PAGE0, 255, 2u*BOOT_ERASE_BYTES);
    assert(boot_metadata_encode(&m.initial, BOOT_COMMIT_VALID,
        m.bytes+BOOT_METADATA_PAGE0, BOOT_METADATA_BYTES) == BOOT_OK);
}
static void setup(uint32_t destination, unsigned mode, unsigned empty)
{
    /* Fresh unpublished receiver/controller for each independent device case. */
    memset(&m, 0, sizeof m); memset(m.bytes, 255, sizeof m.bytes);
    atomic_flag_clear(&m.receiver.entered);
    m.destination = destination; m.mode = mode;
    memset(&m.output, 0xa5, sizeof m.output); m.sentinel = m.output;
    uint32_t current = destination ^ 1u;
    payload(m.bytes+base(current), current, 0);
    memcpy(m.bytes+manifest_address(current), fixture_manifest[current*7u], BOOT_MANIFEST_BYTES);
    boot_verified_image_t image;
    assert(boot_verify_stored(fixture_manifest[current*7u], BOOT_MANIFEST_BYTES,
        current, image_read, &m, NULL, NULL, &image) == BOOT_VERIFY_OK);
    m.initial.sequence = 20; m.initial.current_slot = current;
    m.initial.previous_slot = empty ? BOOT_SLOT_NONE : destination;
    m.initial.candidate_slot = m.initial.intent_slot = BOOT_SLOT_NONE;
    m.initial.confirmed_mask = empty ? 1u << current : 3u;
    m.initial.images[current].payload_length = image.manifest.payload_length;
    m.initial.images[current].release = image.manifest.release;
    memcpy(m.initial.images[current].payload_hash, image.manifest.payload_hash, 32);
    /* Retained confirmed history must survive reception byte-for-byte. */
    m.initial.images[current].trial_id = 9;
    m.initial.images[current].accounting = (1u << 2) | (BOOT_REASON_EXTERNAL << 5);
    if (!empty) m.initial.images[destination] = m.initial.images[current];
    encode_initial();
    boot_flash_io_t io = {&m, read_register, write_register, read_flash};
    assert(boot_flash_init(&m.flash, &io, 8, 3) == BOOT_FLASH_OK);
    m.calls = 0;
}
static boot_receive_config_t config(void)
{
    return (boot_receive_config_t){ .flash = &m.flash, .running_context = &m,
        .running = running, .progress = progress, .progress_context = &m,
        .permitted_slot = m.destination };
}
static void begin(void)
{
    boot_receive_config_t c = config();
    assert(boot_receive_begin(&m.receiver, &c) == BOOT_RECEIVE_OK);
}
static boot_receive_result_t step(void)
{
    boot_receive_result_t result = boot_receive_step(&m.receiver, &m.output);
    if (result != BOOT_RECEIVE_ACCEPTED) check_frozen();
    return result;
}
static void to_phase(boot_receive_phase_t phase)
{
    unsigned limit = 1000;
    while (m.receiver.phase != phase) { assert(limit--); assert(step() == BOOT_RECEIVE_OK); }
}
static void header(void)
{
    size_t accepted;
    assert(boot_receive_feed(&m.receiver, fixture_manifest[m.destination*7u+m.mode],
        BOOT_MANIFEST_BYTES, &accepted) == BOOT_RECEIVE_OK);
    assert(accepted == BOOT_MANIFEST_BYTES);
}
static void supply_payload(unsigned fragment)
{
    static uint8_t bytes[BOOT_PAYLOAD_MAX];
    size_t length = m.mode == 1 ? BOOT_PAYLOAD_MAX : BOOT_PAYLOAD_MIN;
    payload(bytes, m.destination, m.mode);
    size_t used = 0;
    while (used < length) {
        size_t accepted, n = length-used;
        if (n > fragment) n = fragment;
        boot_receive_result_t result = boot_receive_feed(&m.receiver, bytes+used, n, &accepted);
        assert(result == BOOT_RECEIVE_OK || result == BOOT_RECEIVE_WAIT);
        assert(accepted <= n && accepted <= 4);
        used += accepted;
        if (m.mode == 1 && m.receiver.word_used == 4 && used < length) {
            size_t blocked = 99;
            assert(boot_receive_feed(&m.receiver, bytes+used, 1, &blocked) == BOOT_RECEIVE_WAIT);
            assert(!blocked);
        }
        assert(step() == BOOT_RECEIVE_OK || m.receiver.word_used < 4);
    }
    if (m.mode == 1) {
        assert(m.receiver.programmed == BOOT_PAYLOAD_MAX);
        assert(m.fma == manifest_address(m.destination)-4u);
    }
    assert(boot_receive_eof(&m.receiver) == BOOT_RECEIVE_OK);
}
static void duplicates(void)
{
    unsigned calls = m.calls, commands = m.commands;
    boot_receive_acceptance_t saved = m.output;
    boot_receive_phase_t failed_phase = m.receiver.failed_phase;
    boot_receive_result_t failure = m.receiver.failure;
    boot_store_result_t store_result = m.receiver.store_result;
    boot_flash_result_t flash_result = m.receiver.flash_result;
    size_t accepted = 99; uint8_t byte = 0;
    boot_receive_config_t c = config();
    assert(boot_receive_begin(&m.receiver, &c) == BOOT_RECEIVE_CONSUMED);
    assert(boot_receive_step(&m.receiver, &m.output) == BOOT_RECEIVE_CONSUMED);
    assert(boot_receive_feed(&m.receiver, &byte, 1, &accepted) == BOOT_RECEIVE_CONSUMED);
    assert(accepted == 0);
    assert(boot_receive_eof(&m.receiver) == BOOT_RECEIVE_CONSUMED);
    assert(boot_receive_abort(&m.receiver) == BOOT_RECEIVE_CONSUMED);
    assert(m.calls == calls && m.commands == commands && !memcmp(&saved, &m.output, sizeof saved));
    assert(m.receiver.failed_phase == failed_phase && m.receiver.failure == failure);
    assert(m.receiver.store_result == store_result && m.receiver.flash_result == flash_result);
}
static void success(void)
{
    const unsigned fragments[] = {1, 3, 17};
    for (uint32_t slot = 0; slot < 2; ++slot) for (unsigned k = 0; k < 3; ++k) {
        setup(slot, 0, k == 0); m.reentry = 1; begin();
        uint8_t package[BOOT_MANIFEST_BYTES+BOOT_PAYLOAD_MIN]; size_t accepted;
        memcpy(package, fixture_manifest[slot*7u], BOOT_MANIFEST_BYTES);
        payload(package+BOOT_MANIFEST_BYTES, slot, 0);
        /* Fragmented manifest, then input crossing into payload: the caller
         * retains the exact unaccepted suffix through admission/backpressure. */
        size_t prefix = k == 0 ? 0u : (k == 1 ? 1u : 159u);
        assert(boot_receive_feed(&m.receiver, package, prefix, &accepted) == BOOT_RECEIVE_OK);
        assert(accepted == prefix);
        assert(boot_receive_feed(&m.receiver, package+prefix, sizeof package-prefix, &accepted) == BOOT_RECEIVE_OK);
        assert(accepted == BOOT_MANIFEST_BYTES-prefix && !m.commands);
        accepted = 99;
        assert(boot_receive_feed(&m.receiver, package+BOOT_MANIFEST_BYTES, BOOT_PAYLOAD_MIN,
            &accepted) == BOOT_RECEIVE_WAIT && !accepted && !m.commands);
        to_phase(BOOT_RECEIVE_ERASE);
        assert(m.receiver.authority.record.sequence == 21);
        assert(!m.receiver.authority.record.images[slot].payload_length);
        unsigned before = m.commands;
        assert(step() == BOOT_RECEIVE_OK && m.commands == before+1);
        assert(m.fma == manifest_address(slot));
        to_phase(BOOT_RECEIVE_PAYLOAD);
        assert(m.commands == before+110); /* fixed slot geometry, page-at-a-time */
        supply_payload(fragments[k]); to_phase(BOOT_RECEIVE_READY);
        assert(step() == BOOT_RECEIVE_ACCEPTED);
        assert(m.output.reset_requested == 1 && m.output.metadata.record.sequence == 22);
        assert(m.output.metadata.record.candidate_slot == slot);
        assert(m.output.metadata.record.images[slot].trial_id == 22);
        assert(m.output.metadata.record.images[slot].accounting == BOOT_STATE_READY);
        assert(!memcmp(&m.initial.images[slot^1u], &m.output.metadata.record.images[slot^1u], sizeof(boot_image_entry_t)));
        assert(!memcmp(m.bytes+base(slot), package+BOOT_MANIFEST_BYTES, BOOT_PAYLOAD_MIN));
        uint8_t current[BOOT_PAYLOAD_MIN]; payload(current, slot^1u, 0);
        assert(!memcmp(m.bytes+base(slot^1u), current, sizeof current));
        assert(!memcmp(m.bytes+manifest_address(slot^1u), fixture_manifest[(slot^1u)*7u], BOOT_MANIFEST_BYTES));
        for (unsigned i = BOOT_MANIFEST_BYTES; i < BOOT_ERASE_BYTES; ++i)
            assert(m.bytes[manifest_address(slot)+i] == 255);
        m.reentry = 0;
        boot_store_snapshot_t loaded;
        boot_store_adapter_t adapter = boot_flash_store_adapter(&m.flash);
        assert(boot_store_load(&adapter, &loaded) == BOOT_STORE_OK);
        assert(!memcmp(&loaded, &m.output.metadata, sizeof loaded));
        boot_receive_acceptance_t owned = m.output;
        memset(&m.receiver.stored, 0, sizeof m.receiver.stored);
        assert(!memcmp(&owned, &m.output, sizeof owned)); duplicates();
    }
}
static void preadmission(void)
{
    /* Invalid signature, target, active READY/TRIAL, noncurrent running,
     * unconfirmed current, exhausted first/second advance, running read error. */
    for (unsigned kind = 0; kind < 11; ++kind) {
        setup(1, 0, 1);
        if (kind == 2 || kind == 3) {
            m.initial.images[1] = m.initial.images[0];
            m.initial.images[1].trial_id = 19;
            m.initial.images[1].accounting = kind == 2 ? BOOT_STATE_READY : BOOT_STATE_TRIAL;
            m.initial.candidate_slot = 1;
        }
        if (kind == 4) { m.initial.current_slot = 1; m.initial.previous_slot = 0;
            m.initial.images[1] = m.initial.images[0]; m.initial.confirmed_mask = 3; }
        if (kind == 5) { m.initial.confirmed_mask = 0; m.initial.current_slot = BOOT_SLOT_NONE; }
        if (kind == 6) m.initial.sequence = UINT32_MAX;
        if (kind == 7) m.initial.sequence = UINT32_MAX-1u;
        encode_initial();
        if (kind == 10) memset(m.bytes+BOOT_METADATA_PAGE0, 255, 2u*BOOT_ERASE_BYTES);
        uint8_t before[2u*BOOT_ERASE_BYTES]; memcpy(before, m.bytes+BOOT_METADATA_PAGE0, sizeof before);
        boot_receive_config_t c = config();
        if (kind == 8) m.bytes[base(0)+40] ^= 1;
        boot_receive_result_t result = boot_receive_begin(&m.receiver, &c);
        if (result == BOOT_RECEIVE_OK) {
            uint8_t wire[BOOT_MANIFEST_BYTES]; memcpy(wire, fixture_manifest[kind == 1 ? 0 : 7], sizeof wire);
            if (kind == 0) wire[159] ^= 1;
            if (kind == 9) wire[0] ^= 1;
            size_t accepted;
            assert(boot_receive_feed(&m.receiver, wire, sizeof wire, &accepted) == BOOT_RECEIVE_OK);
            result = step();
        }
        assert(result != BOOT_RECEIVE_OK && m.receiver.phase == BOOT_RECEIVE_TERMINAL);
        assert(!m.commands && !memcmp(before, m.bytes+BOOT_METADATA_PAGE0, sizeof before));
        check_frozen(); duplicates();
    }
}
static void lengths_and_abort(void)
{
    const unsigned cuts[] = {0, 159, 160, 161, 162, 163, 395};
    for (unsigned k = 0; k < sizeof cuts/sizeof cuts[0]; ++k) {
        setup(1, 0, 1); begin(); size_t accepted;
        unsigned cut = cuts[k];
        if (cut < 160) {
            assert(boot_receive_feed(&m.receiver, fixture_manifest[7], cut, &accepted) == BOOT_RECEIVE_OK);
        } else {
            header(); to_phase(BOOT_RECEIVE_PAYLOAD);
            uint8_t bytes[BOOT_PAYLOAD_MIN]; payload(bytes, 1, 0);
            unsigned used = 0;
            while (used < cut-160) {
                assert(boot_receive_feed(&m.receiver, bytes+used, cut-160-used, &accepted) == BOOT_RECEIVE_OK);
                used += (unsigned)accepted;
                (void)step();
            }
        }
        assert(boot_receive_eof(&m.receiver) == BOOT_RECEIVE_LENGTH);
        if (cut < 160) assert(!m.commands);
        else assert(m.receiver.authority.record.images[1].payload_length == 0);
        check_frozen(); duplicates();
    }
    setup(1, 0, 1); begin(); header(); to_phase(BOOT_RECEIVE_PAYLOAD);
    uint8_t excess[BOOT_PAYLOAD_MIN+1u] = {0}; size_t accepted = 99;
    unsigned commands = m.commands;
    assert(boot_receive_feed(&m.receiver, excess, sizeof excess, &accepted) == BOOT_RECEIVE_LENGTH);
    assert(!accepted && m.commands == commands); duplicates();
    for (unsigned kind = 0; kind < 3; ++kind) {
        setup(1, 0, 1); begin(); header(); to_phase(BOOT_RECEIVE_PAYLOAD);
        supply_payload(3);
        if (kind == 0) { uint8_t b = 0;
            assert(boot_receive_feed(&m.receiver, &b, 1, &accepted) == BOOT_RECEIVE_LENGTH); }
        if (kind == 1) assert(boot_receive_abort(&m.receiver) == BOOT_RECEIVE_ABORTED);
        if (kind == 2) { to_phase(BOOT_RECEIVE_VERIFY); assert(boot_receive_abort(&m.receiver) == BOOT_RECEIVE_ABORTED); }
        check_frozen(); duplicates();
    }
}
static void stored_failures(void)
{
    for (unsigned kind = 0; kind < 6; ++kind) {
        setup(1, kind == 0 ? 2u : 0u, 1); begin(); header();
        to_phase(BOOT_RECEIVE_PAYLOAD); supply_payload(17); to_phase(BOOT_RECEIVE_VERIFY);
        if (kind == 1) m.bytes[base(1)+100] ^= 1;
        if (kind == 2) m.bytes[manifest_address(1)+159] ^= 1;
        if (kind == 3) m.fail = m.calls+2;
        if (kind == 4) m.observer_abort = 1;
        if (kind == 5) {
#ifdef BOOT_RECEIVE_FAULT_INJECTION
            fail_alloc = 1;
#else
            /* Failure after manifest read, at the first payload read. */
            m.fail = m.calls+4;
#endif
        }
        assert(step() == (kind == 3 ? BOOT_RECEIVE_FLASH : BOOT_RECEIVE_STORED));
        if (kind == 0) assert(m.receiver.verify_result == BOOT_VERIFY_ERROR_MSP);
        if (kind == 1) assert(m.receiver.verify_result == BOOT_VERIFY_ERROR_HASH);
        if (kind == 4) assert(m.receiver.verify_result == BOOT_VERIFY_ERROR_ABORTED);
#ifdef BOOT_RECEIVE_FAULT_INJECTION
        if (kind == 5) assert(m.receiver.verify_result == BOOT_VERIFY_ERROR_CRYPTO);
#endif
        assert(!m.receiver.authority.record.images[1].payload_length); duplicates();
    }
}
static void prepare(boot_receive_phase_t phase)
{
    begin(); header();
    if (phase <= BOOT_RECEIVE_PAYLOAD) to_phase(phase);
    else { to_phase(BOOT_RECEIVE_PAYLOAD); supply_payload(17); to_phase(phase); }
}
static void fault_prefixes(void)
{
    /* Six semantic stages, sampled callback classes (pre-command, command,
     * completion, readback), before/after effects. Not a Cartesian image matrix. */
    const boot_receive_phase_t phases[] = { BOOT_RECEIVE_INVALIDATE, BOOT_RECEIVE_ERASE,
        BOOT_RECEIVE_PAYLOAD, BOOT_RECEIVE_WRITE_MANIFEST, BOOT_RECEIVE_VERIFY, BOOT_RECEIVE_READY };
    for (unsigned k = 0; k < 6; ++k) {
        setup(1, 0, 1); prepare(phases[k]);
        if (phases[k] == BOOT_RECEIVE_PAYLOAD) {
            uint8_t bytes[BOOT_PAYLOAD_MIN]; size_t accepted; payload(bytes, 1, 0);
            assert(boot_receive_feed(&m.receiver, bytes, 4, &accepted) == BOOT_RECEIVE_OK);
        }
        unsigned start = m.calls;
        assert(step() == (phases[k] == BOOT_RECEIVE_READY ? BOOT_RECEIVE_ACCEPTED : BOOT_RECEIVE_OK));
        baseline_count = m.calls-start;
        memcpy(baseline, m.events+start, baseline_count*sizeof *baseline);
        unsigned cuts[6] = {1, baseline_count, 0, 0, 0, 0};
        unsigned count = 2;
        for (unsigned i = 0; i < baseline_count; ++i) {
            if (baseline[i].kind == 1 && baseline[i].address == BOOT_FLASH_FMC) {
                cuts[count++] = i+1; cuts[count++] = i+2;
                break;
            }
        }
        /* Store commit-last command and final committed readback uncertainty. */
        if (phases[k] == BOOT_RECEIVE_INVALIDATE || phases[k] == BOOT_RECEIVE_READY) {
            for (unsigned i = baseline_count; i > 0; --i)
                if (baseline[i-1].kind == 1 && baseline[i-1].address == BOOT_FLASH_FMC) {
                    cuts[count++] = i; break;
                }
        }
        for (unsigned j = 0; j < count; ++j) for (unsigned after = 0; after < 2; ++after) {
            setup(1, 0, 1); prepare(phases[k]);
            if (phases[k] == BOOT_RECEIVE_PAYLOAD) {
                uint8_t bytes[BOOT_PAYLOAD_MIN]; size_t accepted; payload(bytes, 1, 0);
                assert(boot_receive_feed(&m.receiver, bytes, 4, &accepted) == BOOT_RECEIVE_OK);
            }
            start = m.calls; m.fail = start+cuts[j]; m.after = after;
            boot_receive_result_t result = step();
            assert(result != BOOT_RECEIVE_OK && result != BOOT_RECEIVE_ACCEPTED);
            assert(m.receiver.phase == BOOT_RECEIVE_TERMINAL && m.receiver.failed_phase == phases[k]);
            assert(m.calls == start+cuts[j]);
            if (phases[k] == BOOT_RECEIVE_INVALIDATE || phases[k] == BOOT_RECEIVE_READY)
                assert(m.receiver.flash_result == BOOT_FLASH_IO);
            assert(!memcmp(m.events+start, baseline, cuts[j]*sizeof *baseline));
            if ((phases[k] == BOOT_RECEIVE_INVALIDATE || phases[k] == BOOT_RECEIVE_READY) && cuts[j] == baseline_count)
                assert(result == BOOT_RECEIVE_RECONCILE && m.receiver.store_result == BOOT_STORE_UNCERTAIN);
            check_frozen(); duplicates();
            if ((phases[k] == BOOT_RECEIVE_INVALIDATE || phases[k] == BOOT_RECEIVE_READY) &&
                cuts[j] == baseline_count) {
                /* Explicit read-only reconciliation is caller work; it does
                 * not revive this transaction or publish cached acceptance. */
                assert(!m.flash.blocked); m.fail = 0;
                boot_store_adapter_t adapter = boot_flash_store_adapter(&m.flash);
                boot_store_snapshot_t reconciled;
                assert(boot_store_load(&adapter, &reconciled) == BOOT_STORE_OK);
                assert(reconciled.record.sequence == (phases[k] == BOOT_RECEIVE_READY ? 22u : 21u));
                duplicates();
            }
        }
    }
}
static void metadata_diagnostics(void)
{
    /* Authority read: ordinary device IO versus blocked idle timeout. */
    for (unsigned kind = 0; kind < 2; ++kind) {
        setup(1, 0, 1);
        if (kind) m.stall = 1; else m.fail = 2;
        boot_receive_config_t c = config();
        assert(boot_receive_begin(&m.receiver, &c) ==
            (kind ? BOOT_RECEIVE_RECONCILE : BOOT_RECEIVE_AUTHORITY));
        assert(m.receiver.store_result == BOOT_STORE_IO);
        assert(m.receiver.flash_result == (kind ? BOOT_FLASH_TIMEOUT : BOOT_FLASH_IO));
        assert(m.receiver.failed_phase == BOOT_RECEIVE_HEADER && !m.commands);
        assert(m.calls == (kind ? 1u : 2u)); check_frozen(); duplicates();
    }
    const boot_receive_phase_t phases[] = {BOOT_RECEIVE_INVALIDATE, BOOT_RECEIVE_READY};
    for (unsigned p = 0; p < 2; ++p) {
        setup(1, 0, 1); prepare(phases[p]);
        unsigned start = m.calls;
        assert(step() == (p ? BOOT_RECEIVE_ACCEPTED : BOOT_RECEIVE_OK));
        baseline_count = m.calls-start;
        memcpy(baseline, m.events+start, baseline_count*sizeof *baseline);
        unsigned clock = 0, command = 0;
        for (unsigned i = 0; i < baseline_count; ++i) {
            if (!clock && baseline[i].kind == 0 && baseline[i].address == BOOT_FLASH_USECRL) clock = i+1;
            if (!command && baseline[i].kind == 1 && baseline[i].address == BOOT_FLASH_FMC) command = i+1;
        }
        assert(clock && command);
        for (unsigned kind = 0; kind < 3; ++kind) {
            setup(1, 0, 1); prepare(phases[p]); start = m.calls;
            unsigned commands = m.commands;
            if (kind == 0) m.bad_clock = 1;
            if (kind == 1) m.fail = start+clock;
            if (kind == 2) m.stall_command = 1;
            assert(step() == (kind == 2 ? BOOT_RECEIVE_RECONCILE : BOOT_RECEIVE_STORE));
            assert(m.receiver.store_result == BOOT_STORE_IO);
            assert(m.receiver.flash_result == (kind == 0 ? BOOT_FLASH_CLOCK :
                kind == 1 ? BOOT_FLASH_IO : BOOT_FLASH_TIMEOUT));
            assert(m.receiver.failed_phase == phases[p]);
            unsigned prefix = kind == 2 ? command : clock;
            assert(!memcmp(m.events+start, baseline, prefix*sizeof *baseline));
            assert(m.calls == start+prefix+(kind == 2 ? m.flash.polls : 0u));
            assert(m.commands == commands+(kind == 2 ? 1u : 0u));
            if (kind == 2) for (unsigned i = 0; i < m.flash.polls; ++i) {
                event_t expected = {0, BOOT_FLASH_FMC, 0};
                assert(!memcmp(&m.events[start+prefix+i], &expected, sizeof expected));
            }
            check_frozen(); duplicates();
        }
    }
}
static void running_controller_failures(void)
{
    for (unsigned kind = 1; kind <= 2; ++kind) {
        setup(1, 0, 1); m.controller_reader = kind;
        boot_receive_config_t c = config();
        assert(boot_receive_begin(&m.receiver, &c) == BOOT_RECEIVE_RECONCILE);
        assert(m.receiver.store_result == BOOT_STORE_OK);
        assert(m.receiver.flash_result == (kind == 1 ? BOOT_FLASH_IO : BOOT_FLASH_TIMEOUT));
        assert(m.receiver.flash_result == m.running_flash_result);
        assert(m.receiver.failed_phase == BOOT_RECEIVE_HEADER && m.flash.blocked && !m.commands);
        assert(m.calls == 5); /* two authoritative idle/read pairs, one failed idle */
        assert(m.events[4].kind == 0 && m.events[4].address == BOOT_FLASH_FMC);
        /* A later BLOCKED observation must not replace the initiating cause. */
        uint8_t byte;
        assert(boot_flash_read(&m.flash, base(0), &byte, 1) == BOOT_FLASH_BLOCKED);
        assert(m.flash.blocked_result == m.running_flash_result);
        check_frozen(); duplicates();
    }
}
static void maximum_receive(void)
{
    static uint8_t expected[BOOT_PAYLOAD_MAX];
    for (uint32_t slot = 0; slot < 2; ++slot) {
        setup(slot, 1, 1); m.no_trace = 1;
        prepare(BOOT_RECEIVE_READY);
        assert(m.receiver.received == BOOT_PAYLOAD_MAX && m.receiver.programmed == BOOT_PAYLOAD_MAX);
        assert(m.receiver.manifest_programmed == BOOT_MANIFEST_BYTES && !m.receiver.word_used);
        assert(m.fma == manifest_address(slot)+BOOT_MANIFEST_BYTES-4u);
        payload(expected, slot, 1);
        assert(base(slot)+BOOT_PAYLOAD_MAX == manifest_address(slot));
        assert(!memcmp(m.bytes+base(slot), expected, sizeof expected));
        assert(!memcmp(m.bytes+manifest_address(slot), fixture_manifest[slot*7u+1u], BOOT_MANIFEST_BYTES));
        for (unsigned i = BOOT_MANIFEST_BYTES; i < BOOT_ERASE_BYTES; ++i)
            assert(m.bytes[manifest_address(slot)+i] == 255);
        assert(m.receiver.verify_result == BOOT_VERIFY_OK);
        assert(step() == BOOT_RECEIVE_ACCEPTED);
        assert(m.output.image.manifest.payload_length == BOOT_PAYLOAD_MAX);
        assert(m.output.metadata.record.candidate_slot == slot);
        duplicates();
    }
}
static void sequence_boundary(void)
{
    setup(1, 0, 1); m.initial.sequence = UINT32_MAX-2u; encode_initial();
    prepare(BOOT_RECEIVE_READY);
    assert(step() == BOOT_RECEIVE_ACCEPTED);
    assert(m.output.metadata.record.sequence == UINT32_MAX);
    assert(m.output.metadata.record.images[1].trial_id == UINT32_MAX);
    duplicates();
}
static void arguments(void)
{
    setup(1, 0, 1); boot_receive_config_t c = config();
    assert(boot_receive_begin(NULL, &c) == BOOT_RECEIVE_ARGUMENT);
    assert(boot_receive_begin(&m.receiver, NULL) == BOOT_RECEIVE_ARGUMENT && !m.calls);
    assert(boot_receive_begin(&m.receiver, (void *)&m.receiver) == BOOT_RECEIVE_ARGUMENT);
    begin(); unsigned calls = m.calls; size_t accepted = 99;
    assert(boot_receive_begin(&m.receiver, &c) == BOOT_RECEIVE_BUSY);
    assert(boot_receive_step(&m.receiver, (void *)((uint8_t *)&m.receiver+1)) == BOOT_RECEIVE_ARGUMENT);
    assert(boot_receive_step(&m.receiver, (void *)&m.flash) == BOOT_RECEIVE_ARGUMENT);
    assert(boot_receive_feed(&m.receiver, m.receiver.manifest, 1, &accepted) == BOOT_RECEIVE_ARGUMENT);
    assert(accepted == 99);
    uint8_t b = 0;
    assert(boot_receive_feed(&m.receiver, &b, 1, (void *)&m.receiver.received) == BOOT_RECEIVE_ARGUMENT);
    assert(boot_receive_step(&m.receiver, (void *)&m.receiver) == BOOT_RECEIVE_ARGUMENT);
    assert(boot_receive_feed(&m.receiver, NULL, 0, &accepted) == BOOT_RECEIVE_OK && accepted == 0);
    assert(m.calls == calls); assert(boot_receive_abort(&m.receiver) == BOOT_RECEIVE_ABORTED); duplicates();
}
typedef struct { uint32_t load, ctl, lock; unsigned calls; } watchdog_io_t;
static int watchdog_read(void *ctx, uint32_t address, uint32_t *value)
{
    watchdog_io_t *io = ctx; ++io->calls;
    switch (address) {
    case BOOT_WATCHDOG_RCGC0: *value = 8; break;
    case BOOT_WATCHDOG_LOAD: *value = io->load; break;
    case BOOT_WATCHDOG_CTL: *value = io->ctl; break;
    case BOOT_WATCHDOG_LOCK: *value = io->lock; break;
    case BOOT_WATCHDOG_RIS: *value = 0; break;
    default: assert(0);
    }
    return 0;
}
static int watchdog_write(void *ctx, uint32_t address, uint32_t value)
{
    watchdog_io_t *io = ctx; ++io->calls;
    switch (address) {
    case BOOT_WATCHDOG_LOAD: io->load = value; break;
    case BOOT_WATCHDOG_CTL: io->ctl = value; break;
    case BOOT_WATCHDOG_LOCK: io->lock = value == UINT32_C(0x1acce551) ? 0u : 1u; break;
    case BOOT_WATCHDOG_ICR: break;
    default: assert(0);
    }
    return 0;
}
static void watchdog_continuity(void)
{
    watchdog_io_t io = {0}; boot_watchdog_t watchdog;
    boot_watchdog_io_t callbacks = {&io, watchdog_read, watchdog_write};
    assert(boot_watchdog_init(&watchdog, &callbacks) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_configure(&watchdog, 8, 40) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&watchdog, 1, 7, BOOT_VERIFY_MANIFEST_BEGIN, 0, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_boot_report(&watchdog, 1, 7, BOOT_VERIFY_MANIFEST_AUTHENTICATED, 0, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_transfer(&watchdog, 1) == BOOT_WATCHDOG_OK);
    assert(boot_watchdog_application_report(&watchdog, 2, BOOT_WATCHDOG_ETHERNET, 1) == BOOT_WATCHDOG_OK);
    unsigned calls = io.calls;
    setup(1, 0, 1); prepare(BOOT_RECEIVE_READY);
    assert(step() == BOOT_RECEIVE_ACCEPTED); duplicates();
    /* No copied watchdog or forged feeds: the same independently held object
     * retains application epoch, partial genuine evidence and lifetime ID. */
    assert(io.calls == calls && watchdog.owner == BOOT_WATCHDOG_APPLICATION);
    assert(watchdog.epoch == 2 && watchdog.verification == 7);
    assert(watchdog.evidence == BOOT_WATCHDOG_ETHERNET);
    assert(boot_watchdog_service(&watchdog, BOOT_WATCHDOG_APPLICATION, 2) == BOOT_WATCHDOG_PROGRESS);
}
int main(void)
{
#ifdef BOOT_RECEIVE_FAULT_INJECTION
    /* Keep shared normal oracles typechecked without rerunning their cases. */
    (void)success; (void)preadmission; (void)lengths_and_abort;
    (void)stored_failures; (void)fault_prefixes; (void)arguments;
    (void)watchdog_continuity; (void)sequence_boundary;
    (void)metadata_diagnostics; (void)running_controller_failures; (void)maximum_receive;
    /* Normal suite runs once; isolated archive adds one operational class. */
    setup(1, 0, 1); prepare(BOOT_RECEIVE_VERIFY); fail_alloc = 1;
    assert(step() == BOOT_RECEIVE_STORED);
    assert(m.receiver.verify_result == BOOT_VERIFY_ERROR_CRYPTO);
    assert(!m.receiver.authority.record.images[1].payload_length);
    duplicates(); assert(!live_allocations);
#else
    success(); preadmission(); lengths_and_abort(); stored_failures(); fault_prefixes();
    arguments(); sequence_boundary(); watchdog_continuity();
    metadata_diagnostics(); running_controller_failures(); maximum_receive();
#endif
    puts("boot_receive: transaction, framing, admission, durable ordering and stop-prefix checks passed");
    return 0;
}
