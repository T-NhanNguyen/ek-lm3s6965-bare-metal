#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"
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
    void *p;
    if (fail_allocation) return NULL;
    p = calloc(count, size);
    if (p != NULL) ++live_allocations;
    return p;
}
void boot_verify_test_free(void *p)
{
    if (p != NULL) --live_allocations;
    free(p);
}
#endif

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct
{
    flash_model_t flash;
    boot_store_adapter_t real;
    boot_store_snapshot_t expected;
    boot_select_images_t images;
    boot_launch_decision_t out, sentinel;
    uint8_t payload[2][BOOT_PAYLOAD_MIN];
    unsigned reads[2], attempts, order[2];
    int short_slot, abort_attempt, abort_phase, crypto_attempt;
    size_t commit_call, switch_call;
    int mutate_inputs;
    boot_store_adapter_t *source_adapter;
    uint8_t switched[2u * BOOT_ERASE_BYTES];
} fixture_t;

static void unpublished(const fixture_t *f)
{
    CHECK(memcmp(&f->out, &f->sentinel, sizeof f->out) == 0);
}
static int metadata_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    fixture_t *f = context;
    int r;
    unpublished(f);
    /* Adversarial snapshot probe, NOT permission for callback mutation. */
    if (f->mutate_inputs && f->flash.calls == 0u)
    {
        ++f->expected.record.sequence;
        memset(&f->images, 0, sizeof f->images);
        f->source_adapter->read = NULL;
    }
    /* Deliberate ownership violation probes freshness, not concurrency support. */
    if (f->switch_call == f->flash.calls + 1u)
        memcpy(f->flash.bytes, f->switched, sizeof f->switched);
    r = f->real.read(f->real.context, address, bytes, size);
    unpublished(f);
    return r;
}
static int metadata_erase(void *context, uint32_t address)
{
    fixture_t *f = context;
    int r;
    unpublished(f);
    r = f->real.erase_page(f->real.context, address);
    unpublished(f);
    return r;
}
static int metadata_program(void *context, uint32_t address, uint32_t word)
{
    fixture_t *f = context;
    int r;
    unpublished(f);
    if ((address - BOOT_METADATA_PAGE0) % BOOT_ERASE_BYTES == BOOT_METADATA_BYTES - 4u)
        f->commit_call = f->flash.calls + 1u;
    r = f->real.program_word(f->real.context, address, word);
    unpublished(f);
    return r;
}
static boot_store_adapter_t adapter(fixture_t *f)
{
    boot_store_adapter_t a = {f, metadata_read, metadata_erase, metadata_program};
    return a;
}
static size_t payload_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    fixture_t *f = context;
    unsigned slot = address >= BOOT_SLOT_B_BASE ? 1u : 0u;
    unpublished(f);
    CHECK(f->attempts >= 1u && f->attempts <= 2u);
    CHECK(slot == f->order[f->attempts - 1u]);
    CHECK(address == (slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE));
    CHECK(size == BOOT_PAYLOAD_MIN);
    ++f->reads[slot];
    memcpy(bytes, f->payload[slot], size);
    return (int)slot == f->short_slot ? size - 1u : size;
}
static int progress(void *context, boot_verify_phase_t phase, uint32_t completed,
                    uint32_t total)
{
    fixture_t *f = context;
    unpublished(f);
    if (phase == BOOT_VERIFY_MANIFEST_BEGIN) ++f->attempts;
    CHECK(total == BOOT_PAYLOAD_MIN);
    CHECK(completed == (phase == BOOT_VERIFY_PAYLOAD_PROGRESS ? total : 0u));
#ifdef BOOT_VERIFY_FAULT_INJECTION
    fail_allocation = (int)f->attempts == f->crypto_attempt;
#endif
    return !((int)f->attempts == f->abort_attempt && (int)phase == f->abort_phase);
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) p[i] = (uint8_t)(value >> (8u * i));
}
static void signed_image(fixture_t *f, unsigned slot, unsigned mode)
{
    uint32_t base = slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    memcpy(f->images.wire[slot], fixture_manifest[slot * 7u + mode], BOOT_MANIFEST_BYTES);
    f->images.size[slot] = BOOT_MANIFEST_BYTES;
    for (unsigned i = 0u; i < BOOT_PAYLOAD_MIN; ++i)
        f->payload[slot][i] = (uint8_t)(i * 17u + slot * 31u);
    memset(f->payload[slot] + 100u, 0xff, 8u);
    memset(f->payload[slot] + BOOT_PAYLOAD_MIN - 4u, 0xff, 4u);
    put32(f->payload[slot], mode == 2u ? BOOT_INITIAL_MSP - 4u : BOOT_INITIAL_MSP);
    put32(f->payload[slot] + 4u, mode == 3u ? base + 8u :
        mode == 4u ? base - 1u : mode == 5u ? base + BOOT_PAYLOAD_MIN + 1u : base + 9u);
}
static void rewrite(fixture_t *f)
{
    CHECK(boot_metadata_encode(&f->expected.record, BOOT_COMMIT_VALID,
        f->flash.bytes + f->expected.page * BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
}
/* kind: 0 current only, 1 current+previous, 2 READY, 3 TRIAL. */
static void prepare(fixture_t *f, unsigned current, unsigned page, unsigned kind)
{
    boot_metadata_t *r;
    memset(f, 0, sizeof *f);
    flash_model_init(&f->flash);
    f->real = flash_model_adapter(&f->flash);
    f->expected.page = page;
    r = &f->expected.record;
    r->sequence = 100u;
    r->current_slot = current;
    r->previous_slot = kind == 1u ? 1u - current : BOOT_SLOT_NONE;
    r->candidate_slot = kind >= 2u ? 1u - current : BOOT_SLOT_NONE;
    r->confirmed_mask = kind == 1u ? 3u : UINT32_C(1) << current;
    r->intent_slot = BOOT_SLOT_NONE;
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        boot_manifest_t m;
        signed_image(f, slot, 0u);
        CHECK(boot_manifest_decode(f->images.wire[slot], BOOT_MANIFEST_BYTES, &m) == BOOT_OK);
        memcpy(r->images[slot].payload_hash, m.payload_hash, 32u);
        r->images[slot].release = m.release;
        r->images[slot].payload_length = m.payload_length;
        r->images[slot].trial_id = 20u + slot;
        r->images[slot].accounting = (1u << BOOT_ACCOUNT_COUNT_SHIFT) |
            (BOOT_REASON_WATCHDOG << BOOT_ACCOUNT_REASON1_SHIFT);
    }
    if (kind >= 2u)
        r->images[1u-current].accounting = kind == 2u ? BOOT_STATE_READY :
            r->images[1u-current].accounting | BOOT_STATE_TRIAL;
    if (kind == 3u)
    {
        r->intent_kind = BOOT_INTENT_DEADLINE;
        r->intent_slot = 1u-current;
        r->intent_trial_id = r->images[1u-current].trial_id;
    }
    f->order[0] = kind >= 2u ? 1u-current : current;
    f->order[1] = 1u-f->order[0];
    f->short_slot = -1; f->abort_attempt = -1; f->abort_phase = -1;
    f->crypto_attempt = -1;
    memset(&f->sentinel, 0xa5, sizeof f->sentinel);
    f->out = f->sentinel;
    rewrite(f);
}
static boot_select_result_t select_image(fixture_t *f)
{
    boot_store_adapter_t a = adapter(f);
    boot_select_result_t r;
    f->source_adapter = &a;
    r = boot_select(&a, &f->expected, &f->images,
        payload_read, f, progress, f, &f->out);
    f->source_adapter = NULL;
#ifdef BOOT_VERIFY_FAULT_INJECTION
    fail_allocation = 0;
    CHECK(live_allocations == 0u);
#endif
    return r;
}
static void zero_writes(const fixture_t *f, const uint8_t *before)
{
    CHECK(f->flash.writes == 0u && f->flash.erases == 0u);
    CHECK(memcmp(f->flash.bytes, before, sizeof f->flash.bytes) == 0);
}
static void normal_and_candidate(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned kind = 0u; kind < 4u; ++kind)
    {
        fixture_t f;
        uint8_t before[2u * BOOT_ERASE_BYTES];
        prepare(&f, current, page, kind);
        if (kind != 2u) { f.expected.record.sequence = UINT32_MAX; rewrite(&f); }
        memcpy(before, f.flash.bytes, sizeof before);
        CHECK(select_image(&f).status == BOOT_SELECT_OK);
        CHECK(f.out.image.manifest.slot == (kind >= 2u ? 1u-current : current));
        CHECK(f.reads[f.out.image.manifest.slot] == 1u);
        CHECK(f.reads[1u-f.out.image.manifest.slot] == 0u);
        if (kind == 2u)
        {
            CHECK(f.flash.erases == 1u && f.flash.writes == 42u);
            CHECK(f.out.snapshot.record.sequence == 101u);
            CHECK(f.out.snapshot.record.images[1u-current].accounting ==
                ((f.expected.record.images[1u-current].accounting & ~BOOT_ACCOUNT_STATE_MASK) | BOOT_STATE_TRIAL));
        }
        else
        {
            zero_writes(&f, before);
            CHECK(memcmp(&f.out.snapshot, &f.expected, sizeof f.expected) == 0);
        }
    }
}
static boot_verify_result_t break_image(fixture_t *f, unsigned slot, unsigned mode)
{
    switch (mode)
    {
    case 0u: f->images.wire[slot][0] ^= 1u; return BOOT_VERIFY_ERROR_FORMAT;
    case 1u:
        memcpy(f->images.wire[slot], fixture_manifest[(1u-slot)*7u], BOOT_MANIFEST_BYTES);
        return BOOT_VERIFY_ERROR_TARGET;
    case 2u: f->images.wire[slot][100] ^= 1u; return BOOT_VERIFY_ERROR_SIGNATURE;
    case 3u: f->payload[slot][80] ^= 1u; return BOOT_VERIFY_ERROR_HASH;
    case 4u: signed_image(f, slot, 2u); return BOOT_VERIFY_ERROR_MSP;
    case 5u: signed_image(f, slot, 3u); return BOOT_VERIFY_ERROR_THUMB;
    default: signed_image(f, slot, 4u); return BOOT_VERIFY_ERROR_VECTOR_RANGE;
    }
}
static void invalid_roles(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned role = 0u; role < 4u; ++role)
    for (unsigned mode = 0u; mode < 7u; ++mode)
    {
        fixture_t f;
        uint8_t before[2u * BOOT_ERASE_BYTES];
        boot_select_result_t r;
        unsigned slot = role < 2u || role == 3u ? 1u-current : current;
        boot_verify_result_t failure;
        prepare(&f, current, page, role < 2u ? role + 2u : 1u);
        if (role == 3u)
        {
            (void)break_image(&f, current, 3u);
            f.order[0] = current; f.order[1] = 1u-current;
        }
        failure = break_image(&f, slot, mode);
        if (role < 2u) { f.expected.record.sequence = UINT32_MAX; rewrite(&f); }
        /* Structural/target errors have no BEGIN callback. */
        if (mode < 2u) f.order[0] = 1u-slot;
        memcpy(before, f.flash.bytes, sizeof before);
        r = select_image(&f);
        CHECK(r.verify_result[slot] == failure);
        CHECK(r.status == (role == 3u ? BOOT_SELECT_RECOVERY : BOOT_SELECT_OK));
        if (role == 2u)
        {
            boot_store_snapshot_t loaded;
            CHECK(f.flash.erases == 1u && f.flash.writes == 42u);
            CHECK(f.out.image.manifest.slot == 1u-current);
            CHECK(boot_store_load(&f.real, &loaded) == BOOT_STORE_OK);
            CHECK(memcmp(&loaded, &f.out.snapshot, sizeof loaded) == 0);
            CHECK(loaded.record.current_slot == 1u-current);
            CHECK(loaded.record.previous_slot == BOOT_SLOT_NONE);
            CHECK(loaded.record.candidate_slot == BOOT_SLOT_NONE);
            CHECK(loaded.record.confirmed_mask == (UINT32_C(1) << (1u-current)));
            CHECK(loaded.record.sequence == 101u && loaded.page == 1u-page);
            CHECK(memcmp(loaded.record.images, f.expected.record.images, sizeof loaded.record.images) == 0);
            CHECK(memcmp(before + page * BOOT_ERASE_BYTES,
                f.flash.bytes + page * BOOT_ERASE_BYTES, BOOT_ERASE_BYTES) == 0);
        }
        else
        {
            zero_writes(&f, before);
            if (role == 3u) unpublished(&f);
            else
            {
                CHECK(f.out.image.manifest.slot == current);
                CHECK(memcmp(&f.out.snapshot, &f.expected, sizeof f.expected) == 0);
            }
        }
    }
}
static void operational_and_identity(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned role = 0u; role < 4u; ++role)
    for (unsigned mode = 0u; mode < 8u; ++mode)
    {
        fixture_t f;
        unsigned slot = role == 1u || role == 3u ? current : 1u-current;
        unsigned attempt = 1u;
        uint8_t before[2u * BOOT_ERASE_BYTES];
        boot_select_result_t r;
#ifndef BOOT_VERIFY_FAULT_INJECTION
        if (mode == 7u) continue;
#endif
        prepare(&f, current, current, role == 0u || role == 3u ? 3u : 1u);
        if (role == 2u || role == 3u)
        {
            (void)break_image(&f, role == 2u ? current : 1u-current, 3u);
            attempt = 2u;
        }
        if (mode == 0u) f.short_slot = (int)slot;
        else if (mode <= 3u)
        {
            f.abort_attempt = (int)attempt;
            f.abort_phase = (int)mode - 1;
        }
        else if (mode == 4u) f.expected.record.images[slot].release ^= 1u;
        else if (mode == 5u) f.expected.record.images[slot].payload_hash[0] ^= 1u;
        else if (mode == 6u) f.expected.record.images[slot].payload_length += 4u;
        else f.crypto_attempt = (int)attempt;
        rewrite(&f);
        memcpy(before, f.flash.bytes, sizeof before);
        r = select_image(&f);
        if (mode >= 4u && mode <= 6u)
        {
            CHECK(r.status == (role == 0u ? BOOT_SELECT_LIFECYCLE_FAILED : BOOT_SELECT_IDENTITY_FAILED));
            if (role == 0u) CHECK(r.lifecycle_result == BOOT_LIFECYCLE_ERROR_IDENTITY);
        }
        else
        {
            CHECK(r.status == BOOT_SELECT_VERIFY_FAILED);
            CHECK(r.verify_result[slot] == (mode == 0u ? BOOT_VERIFY_ERROR_READ :
                mode == 7u ? BOOT_VERIFY_ERROR_CRYPTO : BOOT_VERIFY_ERROR_ABORTED));
        }
        CHECK(f.attempts == attempt);
        unpublished(&f);
        zero_writes(&f, before);
    }
}
static void recovery_and_limits(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned mode = 0u; mode < 9u; ++mode)
    {
        fixture_t f;
        boot_select_result_t r;
        prepare(&f, current, 0u, mode == 4u || mode == 7u ? 1u : mode == 5u ? 2u : 0u);
        if (mode == 0u) memset(f.flash.bytes, 0xff, sizeof f.flash.bytes);
        else if (mode == 1u)
        {
            boot_metadata_t conflict = f.expected.record;
            conflict.images[current].release ^= 1u;
            CHECK(boot_metadata_encode(&conflict, BOOT_COMMIT_VALID,
                f.flash.bytes + BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
        }
        else if (mode == 2u)
        {
            f.expected.record.current_slot = BOOT_SLOT_NONE;
            f.expected.record.previous_slot = 1u-current;
            f.expected.record.confirmed_mask = 3u;
            rewrite(&f);
        }
        else if (mode == 3u || mode == 4u || mode == 5u || mode == 7u)
        {
            (void)break_image(&f, current, 3u);
            if (mode == 3u) { f.expected.record.confirmed_mask = 3u; rewrite(&f); }
            if (mode == 5u) (void)break_image(&f, 1u-current, 3u);
            if (mode == 4u || mode == 7u)
            {
                f.expected.record.sequence = UINT32_MAX;
                rewrite(&f);
            }
            if (mode == 7u) ++f.expected.record.images[current].release; /* stale */
        }
        else if (mode == 6u)
        {
            f.expected.record.sequence = UINT32_MAX;
            f.expected.record.candidate_slot = 1u-current;
            f.expected.record.images[1u-current].accounting = BOOT_STATE_READY;
            f.order[0] = 1u-current;
            rewrite(&f);
        }
        else f.expected.page = 1u;
        r = select_image(&f);
        CHECK(r.status == (mode == 4u || mode == 6u ? BOOT_SELECT_LIFECYCLE_FAILED :
            mode == 7u || mode == 8u ? BOOT_SELECT_STORE_FAILED : BOOT_SELECT_RECOVERY));
        if (mode == 4u || mode == 6u) CHECK(r.lifecycle_result == BOOT_LIFECYCLE_ERROR_EXHAUSTED);
        if (mode == 7u || mode == 8u) CHECK(r.store_result == BOOT_STORE_STALE);
        CHECK(f.flash.writes == 0u && f.flash.erases == 0u);
        unpublished(&f);
    }
}
static void store_faults(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned kind = 0u; kind < 5u; ++kind)
    {
        fixture_t baseline;
        size_t total, commit;
        prepare(&baseline, current, page, kind == 0u ? 1u : kind == 1u ? 2u : kind == 2u ? 3u : kind == 3u ? 0u : 3u);
        if (kind == 0u) (void)break_image(&baseline, current, 3u);
        if (kind == 4u) (void)break_image(&baseline, 1u-current, 3u);
        CHECK(select_image(&baseline).status == BOOT_SELECT_OK);
        total = baseline.flash.calls; commit = baseline.commit_call;
        for (size_t call = 1u; call <= total; ++call)
        for (unsigned after = 0u; after < 2u; ++after)
        {
            fixture_t f;
            boot_select_result_t r;
            boot_store_snapshot_t loaded;
            uint8_t old_page[BOOT_ERASE_BYTES];
            prepare(&f, current, page, kind == 0u ? 1u : kind == 1u ? 2u : kind == 2u ? 3u : kind == 3u ? 0u : 3u);
            if (kind == 0u) (void)break_image(&f, current, 3u);
            if (kind == 4u) (void)break_image(&f, 1u-current, 3u);
            memcpy(old_page, f.flash.bytes + page * BOOT_ERASE_BYTES, sizeof old_page);
            f.flash.cut_call = call; f.flash.fail_after = (int)after;
            r = select_image(&f);
            CHECK(r.status == (commit != 0u && call >= commit ? BOOT_SELECT_RECONCILE : BOOT_SELECT_STORE_FAILED));
            CHECK(r.store_result == (commit != 0u && call >= commit ? BOOT_STORE_UNCERTAIN : BOOT_STORE_IO));
            CHECK(f.flash.calls == call); /* stop, no automatic reload or replay */
            unpublished(&f);
            CHECK(memcmp(old_page, f.flash.bytes + page * BOOT_ERASE_BYTES, sizeof old_page) == 0);
            f.flash.cut_call = SIZE_MAX; f.flash.stopped = 0;
            /* Explicit caller reconciliation; only old/new authoritative state. */
            CHECK(boot_store_load(&f.real, &loaded) == BOOT_STORE_OK);
            CHECK(loaded.record.sequence == (commit != 0u &&
                (call > commit || (call == commit && after != 0u)) ? 101u : 100u));
            if (loaded.record.sequence == 101u)
            {
                unsigned writes = f.flash.writes, erases = f.flash.erases;
                f.expected = loaded;
                f.attempts = 0u; f.reads[0] = 0u; f.reads[1] = 0u;
                f.order[0] = 1u-current;
                CHECK(select_image(&f).status == BOOT_SELECT_OK);
                CHECK(f.flash.writes == writes && f.flash.erases == erases);
            }
        }
    }
}
static void freshness(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned kind = 0u; kind < 4u; ++kind)
    for (unsigned change = 0u; change < 3u; ++change)
    {
        fixture_t f;
        boot_metadata_t changed;
        size_t total;
        prepare(&f, current, page, kind == 0u ? 0u : kind == 3u ? 1u : 3u);
        if (kind == 2u) (void)break_image(&f, 1u-current, 3u);
        if (kind == 3u) (void)break_image(&f, current, 3u);
        CHECK(select_image(&f).status == BOOT_SELECT_OK);
        total = f.flash.calls;
        prepare(&f, current, page, kind == 0u ? 0u : kind == 3u ? 1u : 3u);
        if (kind == 2u) (void)break_image(&f, 1u-current, 3u);
        if (kind == 3u) (void)break_image(&f, current, 3u);
        memcpy(f.switched, f.flash.bytes, sizeof f.switched);
        changed = f.expected.record;
        if (change == 0u) ++changed.sequence;
        /* Equal canonical records tie to page 0. Remove the old authority only
         * in the injected bytes so either starting page really changes. */
        if (change == 1u)
            memset(f.switched + page * BOOT_ERASE_BYTES, 0xff, BOOT_ERASE_BYTES);
        /* Retained CURRENT history changes without touching image verification
         * identity or candidate intent, sequence, or selected page. */
        if (change == 2u) ++changed.images[current].trial_id;
        CHECK(boot_metadata_encode(&changed, BOOT_COMMIT_VALID,
            f.switched + (change == 1u ? 1u-page : page) * BOOT_ERASE_BYTES,
            BOOT_METADATA_BYTES) == BOOT_OK);
        {
            flash_model_t probe;
            boot_store_adapter_t real;
            boot_store_snapshot_t loaded;
            uint8_t canonical[BOOT_METADATA_BYTES];
            unsigned selected = change == 1u ? 1u-page : page;
            /* Validate injected authority through the real store before the
             * selector runs, without consuming its callback/fault schedule. */
            flash_model_init(&probe);
            memcpy(probe.bytes, f.switched, sizeof probe.bytes);
            real = flash_model_adapter(&probe);
            CHECK(boot_store_load(&real, &loaded) == BOOT_STORE_OK);
            CHECK(loaded.page == selected);
            CHECK(loaded.record.sequence == changed.sequence);
            CHECK(loaded.record.images[current].trial_id == changed.images[current].trial_id);
            CHECK(boot_metadata_encode(&loaded.record, BOOT_COMMIT_VALID,
                canonical, sizeof canonical) == BOOT_OK);
            CHECK(memcmp(canonical, f.switched + selected * BOOT_ERASE_BYTES,
                sizeof canonical) == 0);
            if (change == 1u)
            {
                CHECK(loaded.page != f.expected.page);
                CHECK(loaded.record.sequence == f.expected.record.sequence);
                CHECK(memcmp(canonical, f.flash.bytes + page * BOOT_ERASE_BYTES,
                    sizeof canonical) == 0);
            }
            if (change == 2u)
            {
                CHECK(loaded.page == f.expected.page);
                CHECK(loaded.record.sequence == f.expected.record.sequence);
                CHECK(loaded.record.images[current].trial_id ==
                    f.expected.record.images[current].trial_id + 1u);
                CHECK(memcmp(canonical, f.flash.bytes + page * BOOT_ERASE_BYTES,
                    sizeof canonical) != 0);
            }
            CHECK(probe.calls == 2u && probe.writes == 0u && probe.erases == 0u);
            CHECK(memcmp(probe.bytes, f.switched, sizeof probe.bytes) == 0);
            CHECK(f.flash.calls == 0u && f.flash.writes == 0u && f.flash.erases == 0u);
        }
        f.switch_call = kind == 3u ? 3u : total - 1u;
        {
            boot_select_result_t r = select_image(&f);
            CHECK(r.status == BOOT_SELECT_STORE_FAILED && r.store_result == BOOT_STORE_STALE);
            zero_writes(&f, f.switched);
            if (change == 2u)
            {
                boot_store_snapshot_t loaded;
                unsigned verified = kind == 1u || kind == 3u ? 1u-current : current;
                if (kind == 1u)
                {
                    /* STORE_FAILED is not a complete verification attempt trace.
                     * Progress and reads prove the candidate attempt, not the
                     * neutral default in an unset verifier-result entry. */
                    CHECK(r.evidence_mask == 0u);
                    CHECK(f.attempts == 1u);
                    CHECK(f.reads[current] == 0u);
                    CHECK(f.flash.calls == 6u);
                }
                else
                {
                    CHECK(r.evidence_mask == (kind == 0u ?
                        UINT32_C(1) << verified : UINT32_C(3)));
                    CHECK(r.verify_result[verified] == BOOT_VERIFY_OK);
                    if (kind == 2u || kind == 3u)
                        CHECK(r.verify_result[1u-verified] == BOOT_VERIFY_ERROR_HASH);
                }
                CHECK(r.lifecycle_result == (kind == 3u ?
                    BOOT_LIFECYCLE_CHANGE : BOOT_LIFECYCLE_NO_CHANGE));
                CHECK(f.reads[verified] == 1u);
                CHECK(f.flash.calls == f.switch_call + 1u);
                CHECK(boot_store_load(&f.real, &loaded) == BOOT_STORE_OK);
                CHECK(loaded.page == f.expected.page);
                CHECK(loaded.record.sequence == f.expected.record.sequence);
                CHECK(loaded.record.images[current].trial_id ==
                    f.expected.record.images[current].trial_id + 1u);
            }
        }
        CHECK(f.flash.writes == 0u && f.flash.erases == 0u);
        unpublished(&f);
    }
}
static void arguments(void)
{
    fixture_t f;
    boot_store_adapter_t a;
    prepare(&f, 0u, 0u, 0u);
    a = adapter(&f);
    CHECK(boot_select(NULL, &f.expected, &f.images, payload_read, &f, progress, &f, &f.out).status == BOOT_SELECT_ARGUMENT);
    CHECK(boot_select(&a, NULL, &f.images, payload_read, &f, progress, &f, &f.out).status == BOOT_SELECT_ARGUMENT);
    CHECK(boot_select(&a, &f.expected, NULL, payload_read, &f, progress, &f, &f.out).status == BOOT_SELECT_ARGUMENT);
    CHECK(boot_select(&a, &f.expected, &f.images, NULL, &f, progress, &f, &f.out).status == BOOT_SELECT_ARGUMENT);
    CHECK(boot_select(&a, &f.expected, &f.images, payload_read, &f, progress, &f, NULL).status == BOOT_SELECT_ARGUMENT);
    for (unsigned which = 0u; which < 3u; ++which)
    {
        boot_store_adapter_t bad = a;
        if (which == 0u) bad.read = NULL;
        if (which == 1u) bad.erase_page = NULL;
        if (which == 2u) bad.program_word = NULL;
        CHECK(boot_select(&bad, &f.expected, &f.images, payload_read, &f, progress, &f, &f.out).status == BOOT_SELECT_ARGUMENT);
    }
    /* Every byte displacement intersecting each known input, both directions. */
    for (unsigned which = 0u; which < 3u; ++which)
    {
        union { max_align_t alignment; uint8_t bytes[2048]; } storage;
        uint8_t before[2048];
        size_t size = which == 0u ? sizeof a : which == 1u ? sizeof f.expected : sizeof f.images;
        for (size_t offset = 0u; offset < size + sizeof f.out - 1u; ++offset)
        {
            size_t input_offset = 512u;
            size_t output_offset = input_offset + 1u + offset - sizeof f.out;
            const boot_store_adapter_t *ap = &a;
            const boot_store_snapshot_t *sp = &f.expected;
            const boot_select_images_t *ip = &f.images;
            memset(storage.bytes, 0x3c, sizeof storage.bytes);
            if (which == 0u) { memcpy(storage.bytes + input_offset, &a, size); ap = (const void *)(storage.bytes + input_offset); }
            if (which == 1u) { memcpy(storage.bytes + input_offset, &f.expected, size); sp = (const void *)(storage.bytes + input_offset); }
            if (which == 2u) { memcpy(storage.bytes + input_offset, &f.images, size); ip = (const void *)(storage.bytes + input_offset); }
            memcpy(before, storage.bytes, sizeof before);
            CHECK(boot_select(ap, sp, ip, payload_read, &f, progress, &f,
                (void *)(storage.bytes + output_offset)).status == BOOT_SELECT_ARGUMENT);
            CHECK(memcmp(before, storage.bytes, sizeof before) == 0);
        }
    }
    CHECK(f.flash.calls == 0u);
    f.expected.page = 2u;
    CHECK(select_image(&f).store_result == BOOT_STORE_INVALID_RECORD);
    f.expected.page = 0u; f.expected.record.sequence = 0u;
    CHECK(select_image(&f).store_result == BOOT_STORE_INVALID_RECORD);
    CHECK(f.flash.calls == 0u);
    unpublished(&f);
}
static void readback_and_owned_copies(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    {
        fixture_t f;
        size_t commit;
        prepare(&f, current, page, 1u);
        (void)break_image(&f, current, 3u);
        CHECK(select_image(&f).status == BOOT_SELECT_OK);
        commit = f.commit_call;
        for (unsigned mode = 0u; mode < 5u; ++mode)
        {
            boot_select_result_t r;
            prepare(&f, current, page, 1u);
            (void)break_image(&f, current, 3u);
            f.flash.cut_call = mode == 0u ? commit - 43u : mode == 1u ? commit - 42u :
                mode == 2u ? commit - 1u : commit + 1u;
            f.flash.corrupt_read = mode != 1u;
            f.flash.silent_program = mode == 1u;
            r = select_image(&f);
            CHECK(r.status == (mode >= 3u ? BOOT_SELECT_RECONCILE : BOOT_SELECT_STORE_FAILED));
            CHECK(r.store_result == (mode >= 3u ? BOOT_STORE_UNCERTAIN : BOOT_STORE_VERIFY));
            unpublished(&f);
            if (mode >= 3u)
            {
                boot_store_snapshot_t loaded;
                f.flash.cut_call = SIZE_MAX; f.flash.corrupt_read = 0;
                if (mode == 3u) memset(f.flash.bytes, 0xff, sizeof f.flash.bytes);
                else
                {
                    boot_metadata_t conflict;
                    CHECK(boot_metadata_decode(f.flash.bytes + (1u-page)*BOOT_ERASE_BYTES,
                        BOOT_METADATA_BYTES, &conflict) == BOOT_OK);
                    conflict.images[current].release ^= 1u;
                    CHECK(boot_metadata_encode(&conflict, BOOT_COMMIT_VALID,
                        f.flash.bytes + page*BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
                }
                CHECK(boot_store_load(&f.real, &loaded) ==
                    (mode == 3u ? BOOT_STORE_NO_RECORD : BOOT_STORE_CONFLICT));
                CHECK(select_image(&f).status == BOOT_SELECT_RECOVERY);
                unpublished(&f);
            }
        }
        for (unsigned kind = 0u; kind < 4u; ++kind)
        {
            boot_launch_decision_t saved;
            prepare(&f, current, page, kind);
            f.mutate_inputs = 1;
            CHECK(select_image(&f).status == BOOT_SELECT_OK);
            CHECK(f.out.snapshot.record.sequence == (kind == 2u ? 101u : 100u));
            saved = f.out;
            memset(&f.expected, 0x33, sizeof f.expected);
            memset(&f.images, 0x44, sizeof f.images);
            CHECK(memcmp(&saved, &f.out, sizeof saved) == 0);
        }
        prepare(&f, current, page, 0u);
        {
            boot_store_adapter_t a = adapter(&f);
            f.attempts = 1u; /* reader oracle without optional progress hook */
            CHECK(boot_select(&a, &f.expected, &f.images, payload_read, &f,
                NULL, NULL, &f.out).status == BOOT_SELECT_OK);
        }
    }
}
int main(void)
{
    normal_and_candidate();
    invalid_roles();
    operational_and_identity();
    recovery_and_limits();
    store_faults();
    freshness();
    arguments();
    readback_and_owned_copies();
    printf("boot select: %u checks\n", checks);
    return 0;
}
