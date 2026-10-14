#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_format.h"
#include "flash_model.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "verify_fixtures.h"

#ifdef BOOT_VERIFY_FAULT_INJECTION
void *boot_verify_test_calloc(size_t count, size_t size);
void boot_verify_test_free(void *pointer);
static int fail_allocation;
static size_t live_allocations;
void *boot_verify_test_calloc(size_t count, size_t size)
{
    void *pointer;
    if (fail_allocation) return NULL;
    pointer = calloc(count, size);
    if (pointer != NULL) ++live_allocations;
    return pointer;
}
void boot_verify_test_free(void *pointer)
{
    if (pointer != NULL) --live_allocations;
    free(pointer);
}
#endif

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct
{
    flash_model_t flash;
    boot_store_adapter_t real;
    boot_launch_decision_t out, sentinel;
    uint8_t payload[BOOT_PAYLOAD_MIN];
    uint32_t slot, completed;
    unsigned events, reads;
    int short_read, abort_phase;
    size_t commit_call, switch_call;
    uint8_t switched[BOOT_METADATA_BYTES];
} fixture_t;

static void unpublished(const fixture_t *f)
{
    CHECK(memcmp(&f->out, &f->sentinel, sizeof f->out) == 0);
}
static int metadata_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    fixture_t *f = context;
    int result;
    unpublished(f);
    /* Deliberate exclusive-ownership violation: prove sample freshness checks,
     * not support for concurrent writers. The changed bytes are read truthfully. */
    if (f->switch_call == f->flash.calls + 1u)
        memcpy(f->flash.bytes, f->switched, sizeof f->switched);
    result = f->real.read(f->real.context, address, bytes, size);
    unpublished(f);
    return result;
}
static int metadata_erase(void *context, uint32_t address)
{
    fixture_t *f = context;
    int result;
    unpublished(f);
    result = f->real.erase_page(f->real.context, address);
    unpublished(f);
    return result;
}
static int metadata_program(void *context, uint32_t address, uint32_t word)
{
    fixture_t *f = context;
    int result;
    unpublished(f);
    if ((address - BOOT_METADATA_PAGE0) % BOOT_ERASE_BYTES == BOOT_METADATA_BYTES - 4u)
        f->commit_call = f->flash.calls + 1u;
    result = f->real.program_word(f->real.context, address, word);
    unpublished(f);
    return result;
}
static boot_store_adapter_t adapter(fixture_t *f)
{
    boot_store_adapter_t a = {f, metadata_read, metadata_erase, metadata_program};
    return a;
}
static size_t payload_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    fixture_t *f = context;
    uint32_t base = f->slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    unpublished(f);
    CHECK(address == base + f->completed);
    CHECK(size <= sizeof f->payload - f->completed);
    memcpy(bytes, f->payload + f->completed, size);
    f->completed += (uint32_t)size;
    ++f->reads;
    unpublished(f);
    return f->short_read ? size - 1u : size;
}
static int progress(void *context, boot_verify_phase_t phase, uint32_t completed,
                    uint32_t total)
{
    fixture_t *f = context;
    unpublished(f);
    CHECK(total == sizeof f->payload);
    CHECK(completed == (phase == BOOT_VERIFY_PAYLOAD_PROGRESS ? f->completed : 0u));
    CHECK((unsigned)phase == f->events);
    ++f->events;
    unpublished(f);
    return (int)phase != f->abort_phase;
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) p[i] = (uint8_t)(value >> (8u * i));
}
static boot_store_snapshot_t prepare(fixture_t *f, unsigned slot, unsigned page,
                                      uint32_t state)
{
    boot_manifest_t manifest;
    boot_metadata_t record = {0};
    boot_store_snapshot_t snapshot;
    flash_model_init(&f->flash);
    f->real = flash_model_adapter(&f->flash);
    memset(&f->sentinel, 0xa5, sizeof f->sentinel);
    f->out = f->sentinel;
    f->slot = slot; f->completed = 0u; f->events = 0u; f->reads = 0u;
    f->short_read = 0; f->abort_phase = -1;
    f->commit_call = 0u; f->switch_call = 0u;
    CHECK(boot_manifest_decode(fixture_manifest[slot * 7u], 160u, &manifest) == BOOT_OK);
    for (uint32_t i = 0u; i < sizeof f->payload; ++i)
        f->payload[i] = (uint8_t)(i * 17u + slot * 31u);
    memset(f->payload + 100u, 0xff, 8u);
    memset(f->payload + sizeof f->payload - 4u, 0xff, 4u);
    put32(f->payload, BOOT_INITIAL_MSP);
    put32(f->payload + 4u, manifest.execution_base + 9u);
    record.sequence = 100u; record.current_slot = 1u - slot;
    record.candidate_slot = slot; record.previous_slot = BOOT_SLOT_NONE;
    record.confirmed_mask = UINT32_C(1) << record.current_slot;
    record.intent_slot = BOOT_SLOT_NONE;
    memcpy(record.images[slot].payload_hash, manifest.payload_hash, 32u);
    record.images[slot].release = manifest.release;
    record.images[slot].payload_length = manifest.payload_length;
    record.images[1u - slot] = record.images[slot];
    record.images[slot].trial_id = 100u;
    record.images[slot].accounting = state;
    CHECK(boot_metadata_encode(&record, BOOT_COMMIT_VALID,
        f->flash.bytes + page * BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
    CHECK(boot_store_load(&f->real, &snapshot) == BOOT_STORE_OK);
    f->flash.calls = 0u;
    return snapshot;
}
static boot_launch_result_t launch(fixture_t *f, const boot_store_snapshot_t *snapshot,
                                    const uint8_t *wire)
{
    boot_store_adapter_t a = adapter(f);
    return boot_launch_candidate(&a, snapshot, wire, 160u, payload_read, f,
                                  progress, f, &f->out);
}
static void success(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    {
        fixture_t f;
        boot_store_snapshot_t snapshot = prepare(&f, slot, page, BOOT_STATE_READY);
        boot_launch_result_t result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
        boot_store_snapshot_t loaded;
        CHECK(result.status == BOOT_LAUNCH_OK);
        CHECK(f.events == 3u && f.reads == 1u);
        CHECK(f.flash.erases == 1u && f.flash.writes == 42u);
        CHECK(boot_store_load(&f.real, &loaded) == BOOT_STORE_OK);
        CHECK(memcmp(&f.out.snapshot.record, &loaded.record, sizeof loaded.record) == 0);
        CHECK(f.out.snapshot.page == 1u - page);
        CHECK(loaded.record.sequence == 101u);
        CHECK(loaded.record.images[slot].trial_id == 100u);
        CHECK(loaded.record.images[slot].accounting == BOOT_STATE_TRIAL);
        CHECK(memcmp(&loaded.record.images[1u-slot], &snapshot.record.images[1u-slot],
                     sizeof snapshot.record.images[0]) == 0);
        CHECK(f.out.image.manifest.slot == slot);
        CHECK(f.out.image.initial_msp == BOOT_INITIAL_MSP);
        CHECK(f.out.image.reset_vector == (slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE) + 9u);
        CHECK(memcmp(f.out.image.manifest.payload_hash, loaded.record.images[slot].payload_hash, 32u) == 0);
        snapshot = loaded;
        for (unsigned retry = 0u; retry < 2u; ++retry)
        {
            unsigned writes = f.flash.writes, erases = f.flash.erases;
            f.out = f.sentinel; f.completed = 0u; f.events = 0u; f.reads = 0u;
            CHECK(launch(&f, &snapshot, fixture_manifest[slot * 7u]).status == BOOT_LAUNCH_OK);
            CHECK(f.events == 3u && f.reads == 1u);
            CHECK(f.flash.writes == writes && f.flash.erases == erases);
            CHECK(memcmp(&f.out.snapshot.record, &snapshot.record, sizeof snapshot.record) == 0);
        }
        /* Cached READY must not replay after a successful first launch. */
        snapshot.record.sequence = 100u;
        snapshot.record.images[slot].accounting = BOOT_STATE_READY;
        snapshot.page = page;
        f.out = f.sentinel; f.completed = 0u; f.events = 0u;
        result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
        CHECK(result.status == BOOT_LAUNCH_STORE_FAILED && result.store_result == BOOT_STORE_STALE);
        CHECK(f.events == 0u);
        unpublished(&f);
    }
}
static void rewrite(fixture_t *f, const boot_store_snapshot_t *snapshot)
{
    CHECK(boot_metadata_encode(&snapshot->record, BOOT_COMMIT_VALID,
        f->flash.bytes + snapshot->page * BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
}
static void retry_boundaries(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned state = BOOT_STATE_READY; state <= BOOT_STATE_TRIAL; ++state)
    {
        fixture_t f;
        boot_store_snapshot_t snapshot = prepare(&f, slot, 0u, state);
        boot_launch_result_t result;
        snapshot.record.sequence = UINT32_MAX;
        snapshot.record.images[slot].trial_id = UINT32_MAX;
        if (state == BOOT_STATE_TRIAL)
        {
            snapshot.record.images[slot].accounting |= (2u << BOOT_ACCOUNT_COUNT_SHIFT) |
                (BOOT_REASON_EXTERNAL << BOOT_ACCOUNT_REASON1_SHIFT) |
                (BOOT_REASON_WATCHDOG << BOOT_ACCOUNT_REASON2_SHIFT);
            snapshot.record.intent_kind = BOOT_INTENT_DEADLINE;
            snapshot.record.intent_slot = slot;
            snapshot.record.intent_trial_id = UINT32_MAX;
        }
        rewrite(&f, &snapshot);
        result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
        CHECK(f.flash.writes == 0u && f.flash.erases == 0u);
        if (state == BOOT_STATE_READY)
        {
            CHECK(result.status == BOOT_LAUNCH_LIFECYCLE_FAILED);
            CHECK(result.lifecycle_result == BOOT_LIFECYCLE_ERROR_EXHAUSTED);
            unpublished(&f);
        }
        else
        {
            CHECK(result.status == BOOT_LAUNCH_OK);
            CHECK(memcmp(&f.out.snapshot.record, &snapshot.record, sizeof snapshot.record) == 0);
        }
        snapshot = prepare(&f, slot, 0u, state);
        {
            boot_metadata_t newer = snapshot.record;
            newer.sequence++;
            CHECK(boot_metadata_encode(&newer, BOOT_COMMIT_VALID, f.switched, sizeof f.switched) == BOOT_OK);
        }
        f.switch_call = 3u; /* READY replace re-read / TRIAL publication reload. */
        result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
        CHECK(result.status == BOOT_LAUNCH_STORE_FAILED && result.store_result == BOOT_STORE_STALE);
        CHECK(f.events == 3u && f.reads == 1u);
        CHECK(f.flash.writes == 0u && f.flash.erases == 0u);
        unpublished(&f);
        /* Initial stale TRIAL and READY inputs fail before image callbacks. */
        snapshot = prepare(&f, slot, 0u, state);
        snapshot.record.sequence--;
        snapshot.record.images[slot].trial_id--;
        result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
        CHECK(result.status == BOOT_LAUNCH_STORE_FAILED && result.store_result == BOOT_STORE_STALE);
        CHECK(f.events == 0u && f.flash.writes == 0u && f.flash.erases == 0u);
        unpublished(&f);
    }
}
static void verification_boundaries(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned state = BOOT_STATE_READY; state <= BOOT_STATE_TRIAL; ++state)
    for (unsigned mode = 0u; mode < 15u; ++mode)
    {
        fixture_t f;
        boot_store_snapshot_t snapshot = prepare(&f, slot, 0u, state);
        uint8_t wire[BOOT_MANIFEST_BYTES];
        boot_launch_result_t result;
        memcpy(wire, fixture_manifest[slot * 7u], sizeof wire);
        if (mode < 3u) f.abort_phase = (int)mode;
        else if (mode == 3u) f.short_read = 1;
        else if (mode == 4u) f.payload[100] ^= 1u;
        else if (mode == 5u) wire[96] ^= 1u;
        else if (mode == 6u) wire[0] ^= 1u;
        else if (mode == 7u) snapshot.record.images[slot].release ^= 1u;
        else if (mode == 8u) snapshot.record.images[slot].payload_hash[0] ^= 1u;
        else if (mode == 9u) snapshot.record.images[slot].payload_length += 4u;
        else if (mode == 10u)
        {
            memcpy(wire, fixture_manifest[slot * 7u + 2u], sizeof wire);
            put32(f.payload, UINT32_C(0x2000fffc));
        }
        else if (mode == 11u)
        {
            memcpy(wire, fixture_manifest[slot * 7u + 3u], sizeof wire);
            put32(f.payload + 4u, (slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE) + 8u);
        }
        else if (mode <= 13u)
        {
            uint32_t base = slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
            memcpy(wire, fixture_manifest[slot * 7u + mode - 8u], sizeof wire);
            put32(f.payload + 4u, mode == 12u ? base - 1u : base + BOOT_PAYLOAD_MIN + 1u);
        }
        else memcpy(wire, fixture_manifest[(1u-slot) * 7u], sizeof wire);
        rewrite(&f, &snapshot);
        result = launch(&f, &snapshot, wire);
        if (mode <= 3u)
        {
            CHECK(result.status == BOOT_LAUNCH_VERIFY_FAILED);
            CHECK(result.verify_result == (mode == 3u ? BOOT_VERIFY_ERROR_READ : BOOT_VERIFY_ERROR_ABORTED));
        }
        else if (mode >= 7u && mode <= 9u)
        {
            CHECK(result.status == BOOT_LAUNCH_LIFECYCLE_FAILED);
            CHECK(result.lifecycle_result == BOOT_LIFECYCLE_ERROR_IDENTITY);
        }
        else
        {
            boot_verify_result_t expected = mode == 4u ? BOOT_VERIFY_ERROR_HASH :
                mode == 5u ? BOOT_VERIFY_ERROR_SIGNATURE : mode == 6u ? BOOT_VERIFY_ERROR_FORMAT :
                mode == 10u ? BOOT_VERIFY_ERROR_MSP : mode == 11u ? BOOT_VERIFY_ERROR_THUMB :
                mode <= 13u ? BOOT_VERIFY_ERROR_VECTOR_RANGE : BOOT_VERIFY_ERROR_TARGET;
            CHECK(result.status == BOOT_LAUNCH_IMAGE_INVALID);
            CHECK(result.verify_result == expected);
        }
        CHECK(f.flash.writes == 0u && f.flash.erases == 0u);
        unpublished(&f);
    }
}
static void store_boundaries(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    {
        fixture_t complete;
        boot_store_snapshot_t snapshot = prepare(&complete, slot, page, BOOT_STATE_READY);
        size_t total, commit;
        CHECK(launch(&complete, &snapshot, fixture_manifest[slot * 7u]).status == BOOT_LAUNCH_OK);
        total = complete.flash.calls; commit = complete.commit_call;
        CHECK(commit != 0u && total > commit);
        for (size_t call = 1u; call <= total; ++call)
        for (unsigned effect = 0u; effect < 4u; ++effect)
        {
            fixture_t f;
            uint8_t old_page[BOOT_ERASE_BYTES];
            boot_store_snapshot_t loaded;
            boot_launch_result_t result;
            snapshot = prepare(&f, slot, page, BOOT_STATE_READY);
            memcpy(old_page, f.flash.bytes + page * BOOT_ERASE_BYTES, sizeof old_page);
            f.flash.cut_call = call;
            f.flash.cut_bits = effect == 0u ? 0u : effect == 1u ? 16u : 32u;
            f.flash.fail_after = effect == 3u;
            result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
            CHECK(result.status == (call >= commit ? BOOT_LAUNCH_RECONCILE : BOOT_LAUNCH_STORE_FAILED));
            CHECK(result.store_result == (call >= commit ? BOOT_STORE_UNCERTAIN : BOOT_STORE_IO));
            CHECK(f.flash.calls == call); /* No automatic read/retry after failure. */
            unpublished(&f);
            CHECK(memcmp(old_page, f.flash.bytes + page * BOOT_ERASE_BYTES, sizeof old_page) == 0);
            /* Explicit reconciliation boundary, outside the launch API. */
            f.flash.cut_call = SIZE_MAX; f.flash.stopped = 0;
            CHECK(boot_store_load(&f.real, &loaded) == BOOT_STORE_OK);
            CHECK(loaded.record.sequence == ((call > commit || (call == commit && effect >= 2u)) ? 101u : 100u));
            CHECK(loaded.record.images[slot].trial_id == 100u);
            CHECK((loaded.record.images[slot].accounting & ~BOOT_ACCOUNT_STATE_MASK) == 0u);
            CHECK(memcmp(&loaded.record.images[1u-slot], &snapshot.record.images[1u-slot], sizeof snapshot.record.images[0]) == 0);
        }
        for (size_t call = 1u; call <= 4u; ++call)
        {
            fixture_t f;
            snapshot = prepare(&f, slot, page, BOOT_STATE_TRIAL);
            f.flash.cut_call = call;
            {
                boot_launch_result_t result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
                CHECK(result.status == BOOT_LAUNCH_STORE_FAILED);
                CHECK(result.store_result == BOOT_STORE_IO);
                CHECK(result.verify_result == BOOT_VERIFY_OK);
                CHECK(result.lifecycle_result == BOOT_LIFECYCLE_NO_CHANGE);
                CHECK(f.events == (call <= 2u ? 0u : 3u));
                CHECK(f.reads == (call <= 2u ? 0u : 1u));
            }
            CHECK(f.flash.calls == call && f.flash.erases == 0u && f.flash.writes == 0u);
            unpublished(&f);
        }
    }
}
static void other_store_failures(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        fixture_t f;
        boot_store_snapshot_t snapshot = prepare(&f, slot, 0u, BOOT_STATE_READY);
        size_t total, commit;
        CHECK(launch(&f, &snapshot, fixture_manifest[slot * 7u]).status == BOOT_LAUNCH_OK);
        total = f.flash.calls; commit = f.commit_call;
        /* Silent erase/staging read corruption and silent missing program.
         * The precommit failures cannot become a launch decision. */
        for (unsigned mode = 0u; mode < 3u; ++mode)
        {
            boot_launch_result_t result;
            snapshot = prepare(&f, slot, 0u, BOOT_STATE_READY);
            f.flash.cut_call = mode == 0u ? 6u : mode == 1u ? commit - 1u : 7u;
            f.flash.corrupt_read = mode != 2u;
            f.flash.silent_program = mode == 2u;
            result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
            CHECK(result.status == BOOT_LAUNCH_STORE_FAILED && result.store_result == BOOT_STORE_VERIFY);
            unpublished(&f);
        }
        /* Postcommit readback/select corruption is always UNCERTAIN.
         * Reload may select new authority, or later damage may need recovery. */
        for (unsigned recovery = 0u; recovery < 3u; ++recovery)
        {
            boot_store_snapshot_t loaded;
            boot_launch_result_t result;
            snapshot = prepare(&f, slot, 0u, BOOT_STATE_READY);
            f.flash.cut_call = commit + 1u;
            f.flash.corrupt_read = 1;
            result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
            CHECK(result.status == BOOT_LAUNCH_RECONCILE && result.store_result == BOOT_STORE_UNCERTAIN);
            CHECK(f.flash.calls == commit + 1u && f.flash.calls < total);
            unpublished(&f);
            f.flash.cut_call = SIZE_MAX; f.flash.stopped = 0; f.flash.corrupt_read = 0;
            if (recovery == 1u) /* Independent later media loss, not rollback. */
                memset(f.flash.bytes, 0xff, sizeof f.flash.bytes);
            if (recovery == 2u)
            {
                boot_metadata_t conflict;
                CHECK(boot_metadata_decode(f.flash.bytes + BOOT_ERASE_BYTES,
                    BOOT_METADATA_BYTES, &conflict) == BOOT_OK);
                conflict.images[1u-slot].release ^= 1u;
                CHECK(boot_metadata_encode(&conflict, BOOT_COMMIT_VALID,
                    f.flash.bytes, BOOT_METADATA_BYTES) == BOOT_OK);
            }
            CHECK(boot_store_load(&f.real, &loaded) == (recovery == 0u ? BOOT_STORE_OK :
                  recovery == 1u ? BOOT_STORE_NO_RECORD : BOOT_STORE_CONFLICT));
            if (recovery == 0u)
            {
                unsigned writes = f.flash.writes, erases = f.flash.erases;
                CHECK(loaded.record.sequence == 101u);
                /* Explicit successful reconciliation permits a fresh TRIAL
                 * verification only. It does not replay the READY write. */
                f.completed = 0u; f.events = 0u; f.reads = 0u;
                CHECK(launch(&f, &loaded, fixture_manifest[slot * 7u]).status == BOOT_LAUNCH_OK);
                CHECK(f.flash.writes == writes && f.flash.erases == erases);
            }
            else
            {
                f.completed = 0u; f.events = 0u;
                result = launch(&f, &snapshot, fixture_manifest[slot * 7u]);
                CHECK(result.status == BOOT_LAUNCH_STORE_FAILED);
                CHECK(f.events == 0u);
                unpublished(&f);
            }
        }
    }
}
static void arguments_and_roles(void)
{
    fixture_t f;
    boot_store_snapshot_t snapshot = prepare(&f, 0u, 0u, BOOT_STATE_READY);
    boot_store_adapter_t a = adapter(&f);
    union { boot_store_snapshot_t snapshot; boot_launch_decision_t decision; } alias;
    alias.snapshot = snapshot;
    CHECK(boot_launch_candidate(&a, &alias.snapshot, fixture_manifest[0], 160u,
        payload_read, &f, progress, &f, &alias.decision).status == BOOT_LAUNCH_ARGUMENT);
    CHECK(boot_launch_candidate(NULL, &snapshot, fixture_manifest[0], 160u,
        payload_read, &f, progress, &f, &f.out).status == BOOT_LAUNCH_ARGUMENT);
    CHECK(boot_launch_candidate(&a, &snapshot, (const uint8_t *)&f.out, 160u,
        payload_read, &f, progress, &f, &f.out).status == BOOT_LAUNCH_ARGUMENT);
    CHECK(f.flash.calls == 0u);
    unpublished(&f);
    snapshot.record.images[0].accounting = BOOT_STATE_NONE;
    snapshot.record.candidate_slot = BOOT_SLOT_NONE;
    rewrite(&f, &snapshot);
    CHECK(launch(&f, &snapshot, fixture_manifest[0]).status == BOOT_LAUNCH_INELIGIBLE);
    CHECK(f.events == 0u && f.flash.erases == 0u && f.flash.writes == 0u);
    unpublished(&f);
#ifdef BOOT_VERIFY_FAULT_INJECTION
    snapshot = prepare(&f, 0u, 0u, BOOT_STATE_READY);
    fail_allocation = 1;
    {
        boot_launch_result_t result = launch(&f, &snapshot, fixture_manifest[0]);
        CHECK(result.status == BOOT_LAUNCH_VERIFY_FAILED && result.verify_result == BOOT_VERIFY_ERROR_CRYPTO);
    }
    fail_allocation = 0;
    CHECK(live_allocations == 0u && f.flash.writes == 0u && f.flash.erases == 0u);
    unpublished(&f);
#endif
}
static void review_arguments(void)
{
    fixture_t f;
    boot_store_snapshot_t snapshot = prepare(&f, 0u, 0u, BOOT_STATE_READY);
    boot_store_adapter_t a = adapter(&f);
    /* Aligned overlapping spans, never dereferenced as an output on rejection. */
    union {
        boot_launch_decision_t decision;
        boot_store_adapter_t adapter;
        boot_store_snapshot_t snapshot;
        struct { uint64_t pad; boot_store_adapter_t adapter; } shifted_adapter;
        struct { uint64_t pad; boot_store_snapshot_t snapshot; } shifted_snapshot;
        struct { uint64_t pad; boot_launch_decision_t decision; } shifted_output;
        uint8_t wire[sizeof(boot_launch_decision_t) + sizeof(uint64_t)];
    } alias, before;
    for (unsigned mode = 0u; mode < 7u; ++mode)
    {
        const boot_store_adapter_t *input_adapter = &a;
        const boot_store_snapshot_t *input_snapshot = &snapshot;
        const uint8_t *wire = fixture_manifest[0];
        boot_launch_decision_t *out = &alias.decision;
        memset(&alias, 0xa5, sizeof alias);
        if (mode == 0u) { alias.adapter = a; input_adapter = &alias.adapter; }
        if (mode == 1u) { alias.shifted_adapter.adapter = a; input_adapter = &alias.shifted_adapter.adapter; }
        if (mode == 2u) { alias.adapter = a; input_adapter = &alias.adapter; out = &alias.shifted_output.decision; }
        if (mode == 3u) { alias.shifted_snapshot.snapshot = snapshot; input_snapshot = &alias.shifted_snapshot.snapshot; }
        if (mode == 4u) { alias.snapshot = snapshot; input_snapshot = &alias.snapshot; out = &alias.shifted_output.decision; }
        if (mode == 5u) wire = alias.wire + sizeof(uint64_t);
        if (mode == 6u) { wire = alias.wire; out = &alias.shifted_output.decision; }
        memcpy(&before, &alias, sizeof alias);
        CHECK(boot_launch_candidate(input_adapter, input_snapshot, wire, 160u,
            payload_read, &f, progress, &f, out).status == BOOT_LAUNCH_ARGUMENT);
        CHECK(memcmp(&alias, &before, sizeof alias) == 0);
        CHECK(f.flash.calls == 0u && f.events == 0u && f.reads == 0u);
        unpublished(&f);
    }
    for (unsigned mode = 0u; mode < 7u; ++mode)
    {
        boot_store_adapter_t missing = a;
        boot_launch_result_t result;
        if (mode == 0u) missing.read = NULL;
        if (mode == 1u) missing.erase_page = NULL;
        if (mode == 2u) missing.program_word = NULL;
        result = boot_launch_candidate(&missing, mode == 3u ? NULL : &snapshot,
            mode == 4u ? NULL : fixture_manifest[0], 160u,
            mode == 5u ? NULL : payload_read, &f, progress, &f,
            mode == 6u ? NULL : &f.out);
        CHECK(result.status == BOOT_LAUNCH_ARGUMENT);
        CHECK(f.flash.calls == 0u && f.events == 0u && f.reads == 0u);
        unpublished(&f);
    }
}
static void invalid_snapshots(void)
{
    for (unsigned mode = 0u; mode < 2u; ++mode)
    {
        fixture_t f;
        boot_store_snapshot_t snapshot = prepare(&f, 0u, 0u, BOOT_STATE_READY);
        boot_launch_result_t result;
        if (mode == 0u) snapshot.page = 2u;
        else snapshot.record.candidate_slot = 3u;
        result = launch(&f, &snapshot, fixture_manifest[0]);
        CHECK(result.status == BOOT_LAUNCH_STORE_FAILED);
        CHECK(result.store_result == BOOT_STORE_INVALID_RECORD);
        CHECK(result.verify_result == BOOT_VERIFY_OK);
        CHECK(result.lifecycle_result == BOOT_LIFECYCLE_NO_CHANGE);
        CHECK(f.flash.calls == 0u && f.events == 0u && f.reads == 0u);
        unpublished(&f);
    }
}
static void owned_decision(void)
{
    fixture_t f;
    boot_store_snapshot_t snapshot = prepare(&f, 0u, 0u, BOOT_STATE_READY);
    boot_launch_decision_t saved;
    boot_manifest_t manifest;
    uint8_t wire[BOOT_MANIFEST_BYTES];
    memcpy(wire, fixture_manifest[0], sizeof wire);
    CHECK(boot_manifest_decode(wire, sizeof wire, &manifest) == BOOT_OK);
    CHECK(launch(&f, &snapshot, wire).status == BOOT_LAUNCH_OK);
    memcpy(&saved, &f.out, sizeof saved);
    /* After return, mutate only caller's source copies, not physical authority
     * or payload. This checks value ownership, not permission for later entry. */
    memset(&snapshot, 0x3c, sizeof snapshot);
    memset(wire, 0x3c, sizeof wire);
    CHECK(memcmp(&saved, &f.out, sizeof saved) == 0);
    CHECK(f.out.snapshot.record.sequence == 101u);
    CHECK(f.out.snapshot.record.images[0].accounting == BOOT_STATE_TRIAL);
    CHECK(memcmp(f.out.image.manifest.payload_hash, manifest.payload_hash, 32u) == 0);
    CHECK(f.out.image.manifest.release == manifest.release);
    CHECK(f.out.image.manifest.payload_length == manifest.payload_length);
}
int main(void)
{
    success();
    retry_boundaries();
    verification_boundaries();
    store_boundaries();
    other_store_failures();
    arguments_and_roles();
    review_arguments();
    invalid_snapshots();
    owned_decision();
    printf("boot_launch: %u checks\n", checks);
    return 0;
}
