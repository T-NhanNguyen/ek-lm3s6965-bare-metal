#include "lm3s6965/boot_runtime.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_reset.h"
#include "lm3s6965/boot_reset_handler.h"
#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"
#include "flash_model.h"

#include <stddef.h>
#include <stdatomic.h>
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
    boot_store_adapter_t inner, store;
    boot_reset_register_adapter_t registers;
    boot_select_images_t images;
    boot_metadata_t original;
    boot_runtime_t runtime;
    boot_launch_decision_t out, sentinel;
    uint8_t payload[2][BOOT_PAYLOAD_MIN];
    uint32_t captured, fresh, written;
    unsigned reg_calls, reg_cut, attempts, selected, reentries;
    unsigned verify_erases, verify_writes;
    int after, mutate, short_read, abort_phase;
    char trace[1024];
    size_t logs, commit[2], commits;
} fixture_t;

static size_t payload_read(void *, uint32_t, uint8_t *, size_t);
static int progress(void *, boot_verify_phase_t, uint32_t, uint32_t);
static boot_runtime_result_t run(fixture_t *f)
{
    return boot_runtime_run(&f->runtime, &f->registers, &f->store, &f->images,
        payload_read, f, progress, f, &f->out);
}
static void boundary(fixture_t *f, char operation)
{
    boot_runtime_result_t nested;
    CHECK(memcmp(&f->out, &f->sentinel, sizeof f->out) == 0);
    CHECK(f->runtime.consumed == 1u);
    nested = run(f);
    CHECK(nested.status == BOOT_RUNTIME_BUSY && nested.valid == 0u);
    ++f->reentries;
    CHECK(f->logs < sizeof f->trace);
    f->trace[f->logs++] = operation;
}
static int reg_read(void *context, uint32_t *out)
{
    fixture_t *f = context;
    boundary(f, 'R');
    ++f->reg_calls;
    *out = f->reg_calls == 1u ? f->captured : f->fresh;
    if (f->mutate && f->reg_calls == 1u)
    {
        /* Adversarial snapshot probe, not permission to mutate live inputs. */
        memset(&f->images, 0, sizeof f->images);
        f->registers.read = NULL; f->registers.write = NULL;
        f->store.read = NULL; f->store.erase_page = NULL; f->store.program_word = NULL;
    }
    return f->reg_calls == f->reg_cut ? -1 : 0;
}
static int reg_write(void *context, uint32_t value)
{
    fixture_t *f = context;
    boundary(f, 'W'); ++f->reg_calls;
    if (f->reg_calls != f->reg_cut || f->after) f->written = value;
    return f->reg_calls == f->reg_cut ? -1 : 0;
}
static int metadata_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    fixture_t *f = context;
    boundary(f, 'r');
    return f->inner.read(f->inner.context, address, bytes, size);
}
static int metadata_erase(void *context, uint32_t address)
{
    fixture_t *f = context;
    boundary(f, 'e');
    return f->inner.erase_page(f->inner.context, address);
}
static int metadata_program(void *context, uint32_t address, uint32_t word)
{
    fixture_t *f = context;
    boundary(f, 'p');
    if ((address - BOOT_METADATA_PAGE0) % BOOT_ERASE_BYTES == BOOT_METADATA_BYTES - 4u)
    {
        CHECK(f->commits < 2u);
        f->commit[f->commits++] = f->flash.calls + 1u;
    }
    return f->inner.program_word(f->inner.context, address, word);
}
static size_t payload_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    fixture_t *f = context;
    unsigned slot = address >= BOOT_SLOT_B_BASE ? 1u : 0u;
    boundary(f, 'b');
    CHECK(address == (slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE));
    CHECK(size == BOOT_PAYLOAD_MIN);
    f->selected = slot;
    memcpy(bytes, f->payload[slot], size);
    return f->short_read ? size - 1u : size;
}
static int progress(void *context, boot_verify_phase_t phase, uint32_t completed,
                    uint32_t total)
{
    fixture_t *f = context;
    boundary(f, 'v');
    CHECK(total == BOOT_PAYLOAD_MIN);
    CHECK(completed <= total);
    if (phase == BOOT_VERIFY_MANIFEST_BEGIN)
    {
        ++f->attempts;
        f->verify_erases = f->flash.erases; f->verify_writes = f->flash.writes;
    }
    return (int)phase != f->abort_phase;
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) p[i] = (uint8_t)(value >> (8u * i));
}
/* kind: current only, READY, TRIAL, rejected, current+previous. Literal wire
 * state/count/history fixtures and independent reason table below are oracles,
 * not expected values computed by the production reset/lifecycle functions. */
static void prepare(fixture_t *f, unsigned current, unsigned page,
                    unsigned kind, unsigned count, unsigned intent)
{
    boot_metadata_t *r;
    memset(f, 0, sizeof *f);
    flash_model_init(&f->flash); f->inner = flash_model_adapter(&f->flash);
    f->store = (boot_store_adapter_t){f, metadata_read, metadata_erase, metadata_program};
    f->registers = (boot_reset_register_adapter_t){f, reg_read, reg_write};
    boot_runtime_init(&f->runtime);
    CHECK(!atomic_flag_test_and_set(&f->runtime.entered));
    atomic_flag_clear(&f->runtime.entered);
    r = &f->original;
    r->sequence = 100u; r->current_slot = current;
    r->previous_slot = kind == 4u ? 1u - current : BOOT_SLOT_NONE;
    r->candidate_slot = kind == 1u || kind == 2u ? 1u - current : BOOT_SLOT_NONE;
    r->confirmed_mask = kind == 4u ? 3u : 1u << current;
    r->intent_slot = BOOT_SLOT_NONE;
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        boot_manifest_t m;
        uint32_t base = slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
        memcpy(f->images.wire[slot], fixture_manifest[slot * 7u], BOOT_MANIFEST_BYTES);
        f->images.size[slot] = BOOT_MANIFEST_BYTES;
        for (unsigned i = 0u; i < BOOT_PAYLOAD_MIN; ++i)
            f->payload[slot][i] = (uint8_t)(i * 17u + slot * 31u);
        memset(f->payload[slot] + 100u, 0xff, 8u);
        memset(f->payload[slot] + BOOT_PAYLOAD_MIN - 4u, 0xff, 4u);
        put32(f->payload[slot], BOOT_INITIAL_MSP); put32(f->payload[slot] + 4u, base + 9u);
        CHECK(boot_manifest_decode(f->images.wire[slot], BOOT_MANIFEST_BYTES, &m) == BOOT_OK);
        memcpy(r->images[slot].payload_hash, m.payload_hash, 32u);
        r->images[slot].release = m.release; r->images[slot].payload_length = m.payload_length;
        r->images[slot].trial_id = 20u + slot;
    }
    if (kind >= 1u && kind <= 3u)
    {
        uint32_t accounting = kind | (count << 2u);
        for (unsigned i = 0u; i < count; ++i) accounting |= 1u << (5u + 3u * i);
        if (kind == 3u) accounting |= 16u;
        r->images[1u-current].accounting = accounting;
    }
    if (intent)
    {
        r->intent_kind = 1u; r->intent_slot = 1u-current;
        r->intent_trial_id = r->images[1u-current].trial_id;
    }
    CHECK(boot_metadata_validate(r) == BOOT_OK);
    CHECK(boot_metadata_encode(r, BOOT_COMMIT_VALID,
        f->flash.bytes + page * BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
    memset(&f->sentinel, 0xa5, sizeof f->sentinel); f->out = f->sentinel;
    f->fresh = UINT32_C(0xffffffe0) | 31u; f->abort_phase = -1;
}
static void duplicate(fixture_t *f)
{
    size_t calls = f->flash.calls, logs = f->logs;
    unsigned regs = f->reg_calls;
    boot_launch_decision_t before = f->out;
    boot_runtime_result_t r = run(f);
    CHECK(r.status == BOOT_RUNTIME_CONSUMED && r.valid == 0u);
    CHECK(f->flash.calls == calls && f->reg_calls == regs && f->logs == logs);
    CHECK(memcmp(&before, &f->out, sizeof before) == 0);
}
static void ordered_trace(const fixture_t *f, unsigned flags, unsigned counted,
                          unsigned candidate_present)
{
    char expected[256];
    size_t n = 0u;
    expected[n++] = 'R';
    if (flags != 0u) { expected[n++] = 'R'; expected[n++] = 'W'; }
    expected[n++] = 'r'; expected[n++] = 'r'; /* handler authority */
    if (counted)
    {
        memcpy(expected + n, "rrer", 4u); n += 4u;
        memset(expected + n, 'p', 41u); n += 41u;
        memcpy(expected + n, "rprrr", 5u); n += 5u; /* staging, commit, readback/load */
    }
    memcpy(expected + n, "rr", 2u); n += 2u; /* selector authority */
    if (candidate_present) { memcpy(expected + n, "rr", 2u); n += 2u; }
    memcpy(expected + n, "vvbvrr", 6u); n += 6u; /* real verifier and final authority */
    CHECK(f->logs == n && memcmp(f->trace, expected, n) == 0);
    CHECK(f->reentries == f->logs);
}
static void matrix(void)
{
    static const unsigned reasons[32] = {0,2,0,2,3,5,3,5,1,5,1,5,5,5,5,5,
        4,5,4,5,5,5,5,5,5,5,5,5,5,5,5,5};
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned count = 0u; count < 4u; ++count)
    for (unsigned flags = 0u; flags < 32u; ++flags)
    {
        fixture_t f;
        boot_runtime_result_t r;
        unsigned counted = count < 3u && reasons[flags] != 0u;
        unsigned candidate = 1u-current;
        prepare(&f, current, page, count == 3u ? 3u : 2u, count, count < 3u);
        f.captured = UINT32_C(0x80000000) | flags;
        r = run(&f);
        CHECK(r.status == BOOT_RUNTIME_OK && r.valid == 3u);
        CHECK(r.reset_status == BOOT_RESET_HANDLER_OK && r.selection.status == BOOT_SELECT_OK);
        CHECK(r.reset.captured_flags == f.captured);
        CHECK(r.reset.decision.counted == counted);
        CHECK(f.reg_calls == (flags == 0u ? 1u : 3u));
        CHECK(memcmp(f.trace, flags == 0u ? "Rrr" : "RRWrr", flags == 0u ? 3u : 5u) == 0);
        if (flags != 0u) CHECK(f.written == (f.fresh & ~flags));
        CHECK(f.verify_erases == counted && f.verify_writes == counted * 42u);
        CHECK(f.flash.erases == counted && f.flash.writes == counted * 42u);
        CHECK(r.reset.snapshot.record.sequence == 100u + counted);
        CHECK(r.reset.snapshot.page == (counted ? 1u-page : page));
        if (counted)
        {
            uint32_t a = r.reset.snapshot.record.images[candidate].accounting;
            CHECK(r.reset.decision.reason == reasons[flags]);
            CHECK(((a >> 2u) & 3u) == count + 1u);
            CHECK(((a >> (5u + count * 3u)) & 7u) == reasons[flags]);
            CHECK(r.reset.snapshot.record.intent_kind == 0u);
            CHECK((a & 3u) == (count == 2u ? 3u : 2u));
            CHECK(r.reset.snapshot.record.candidate_slot == (count == 2u ? BOOT_SLOT_NONE : candidate));
        }
        else CHECK(memcmp(&r.reset.snapshot.record, &f.original, sizeof f.original) == 0);
        CHECK(f.selected == (count == 3u || (count == 2u && counted) ? current : candidate));
        ordered_trace(&f, flags, counted, count < 3u && !(count == 2u && counted));
        CHECK(r.selection.evidence_mask == (1u << f.selected));
        CHECK(f.out.image.manifest.slot == f.selected);
        CHECK(memcmp(&f.out.snapshot, &r.reset.snapshot, sizeof f.out.snapshot) == 0);
        duplicate(&f);
    }
}
static void policies_and_copy(void)
{
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned kind = 0u; kind <= 4u; ++kind)
    {
        fixture_t f;
        boot_runtime_result_t r;
        prepare(&f, current, page, kind, kind == 3u ? 3u : 0u, kind == 2u);
        f.captured = 2u; /* Uncounted POR preserves TRIAL intent. */
        if (kind == 4u) f.payload[current][20] ^= 1u; /* image-invalid -> promotion */
        f.mutate = 1;
        r = run(&f);
        CHECK(r.status == BOOT_RUNTIME_OK);
        CHECK(r.reset.lifecycle_result == BOOT_LIFECYCLE_NO_CHANGE);
        CHECK(f.verify_erases == 0u); /* zero accounting != zero total writes */
        CHECK(f.flash.erases == (kind == 1u || kind == 4u ? 1u : 0u));
        CHECK(r.reset.snapshot.record.intent_kind == (kind == 2u ? 1u : 0u));
        CHECK(f.out.snapshot.record.sequence == (kind == 1u || kind == 4u ? 101u : 100u));
        if (kind == 1u) CHECK((f.out.snapshot.record.images[1u-current].accounting & 3u) == 2u);
        if (kind == 4u) CHECK(f.out.snapshot.record.current_slot == 1u-current);
        {
            boot_launch_decision_t owned = f.out;
            memset(&f.images, 0x55, sizeof f.images);
            memset(f.flash.bytes, 0xff, sizeof f.flash.bytes);
            memset(&r, 0, sizeof r);
            CHECK(memcmp(&owned, &f.out, sizeof owned) == 0);
        }
        duplicate(&f);
    }
}
static void fresh_boot_and_no_intent(void)
{
    fixture_t first, second, no_intent;
    boot_runtime_result_t r;
    boot_store_snapshot_t loaded;
    prepare(&first, 0u, 0u, 2u, 0u, 1u); first.captured = 8u;
    r = run(&first);
    CHECK(r.status == BOOT_RUNTIME_OK && r.reset.snapshot.record.sequence == 101u);
    duplicate(&first);
    /* New model boot: fresh object, same durable bytes, freshly captured POR.
     * Do not reinitialize the old once-per-boot object or replay old WDT. */
    prepare(&second, 0u, 0u, 2u, 0u, 0u);
    memcpy(second.flash.bytes, first.flash.bytes, sizeof second.flash.bytes);
    second.captured = 2u;
    r = run(&second);
    CHECK(r.status == BOOT_RUNTIME_OK && second.reg_calls == 3u);
    CHECK(r.reset.captured_flags == 2u && r.reset.decision.counted == 0u);
    CHECK(r.reset.snapshot.record.sequence == 101u && second.flash.writes == 0u);
    CHECK(boot_store_load(&second.inner, &loaded) == BOOT_STORE_OK);
    CHECK(((loaded.record.images[1].accounting >> 2u) & 3u) == 1u);
    duplicate(&second);
    prepare(&no_intent, 0u, 0u, 2u, 0u, 0u); no_intent.captured = 16u;
    r = run(&no_intent);
    CHECK(r.status == BOOT_RUNTIME_OK && r.reset.decision.counted == 0u);
    CHECK(no_intent.flash.writes == 0u && r.reset.snapshot.record.sequence == 100u);
    duplicate(&no_intent);
}
static void reset_register_faults(void)
{
    for (unsigned cut = 1u; cut <= 3u; ++cut)
    for (unsigned after = 0u; after < 2u; ++after)
    {
        fixture_t f;
        boot_runtime_result_t r;
        prepare(&f, 0u, 0u, 2u, 0u, 1u); f.captured = 8u;
        f.reg_cut = cut; f.after = (int)after;
        r = run(&f);
        CHECK(r.status == BOOT_RUNTIME_RESET_FAILED && r.valid == 1u);
        CHECK(r.reset_status == (cut == 1u ? BOOT_RESET_HANDLER_CAPTURE_FAILED :
            cut == 2u ? BOOT_RESET_HANDLER_CLEAR_READ_FAILED : BOOT_RESET_HANDLER_CLEAR_WRITE_FAILED));
        CHECK(r.reset.valid == (cut == 1u ? 0u : BOOT_RESET_REPORT_CAPTURE));
        CHECK(f.logs == cut && memcmp(f.trace, "RRW", cut) == 0);
        CHECK(f.reentries == f.logs && f.reg_calls == cut);
        CHECK(f.flash.calls == 0u && f.attempts == 0u);
        CHECK(f.written == (cut == 3u && after ? f.fresh & ~UINT32_C(8) : 0u));
        CHECK(memcmp(&f.out, &f.sentinel, sizeof f.out) == 0);
        duplicate(&f);
    }
}
/* Independent callback protocol, not a captured baseline trace or production
 * status/enum arithmetic. Each role permits ONE replacement: a counted TRIAL
 * reset cannot also become READY or promote a previous image in these fixtures. */
typedef struct
{
    char trace[256];
    size_t length, reset_end, commit_at, replace_end;
    size_t store_calls, commit_call;
} fault_trace_t;
static void trace_part(fault_trace_t *t, const char *part)
{
    size_t size = strlen(part);
    CHECK(t->length + size <= sizeof t->trace);
    memcpy(t->trace + t->length, part, size);
    t->length += size;
}
static void trace_replace(fault_trace_t *t)
{
    trace_part(t, "rrer"); /* authority, erase, erased-page readback */
    for (unsigned word = 0u; word < 41u; ++word) trace_part(t, "p");
    trace_part(t, "r"); /* staged record readback: still ordinary IO */
    t->commit_at = t->length;
    trace_part(t, "prrr"); /* commit, committed readback, two-page authority */
    t->replace_end = t->length; /* only this interval is uncertain */
}
static fault_trace_t expected_fault_trace(unsigned mode)
{
    fault_trace_t t = {0};
    trace_part(&t, "RRWrr"); /* capture/clear and handler authority */
    if (mode == 0u) trace_replace(&t); /* TRIAL reset accounting */
    t.reset_end = t.length;
    trace_part(&t, "rr"); /* selector authority, outside reset uncertainty */
    if (mode != 2u) trace_part(&t, "rr"); /* candidate authority */
    trace_part(&t, "vvbv");
    if (mode == 2u) trace_part(&t, "vvbv"); /* invalid current, valid previous */
    if (mode != 0u) trace_replace(&t); /* READY trial or previous promotion */
    else trace_part(&t, "rr"); /* TRIAL final authority: ordinary selector IO */
    for (size_t i = 0u; i < t.length; ++i)
        if (t.trace[i] == 'r' || t.trace[i] == 'e' || t.trace[i] == 'p')
        {
            ++t.store_calls;
            if (i == t.commit_at) t.commit_call = t.store_calls;
        }
    CHECK(t.commit_call != 0u);
    return t;
}
static void failed_trace(const fixture_t *f, const fault_trace_t *t, size_t cut,
                         const boot_runtime_result_t *r)
{
    size_t calls = 0u, end = 0u, attempts = 0u;
    for (size_t i = 0u; i < t->length; ++i)
        if (t->trace[i] == 'r' || t->trace[i] == 'e' || t->trace[i] == 'p')
        {
            if (++calls == cut) { end = i + 1u; break; }
        }
    CHECK(end != 0u);
    /* Includes the failing callback and forbids ALL later external callbacks,
     * including register, verifier, or metadata work after stage failure. */
    CHECK(f->logs == end && memcmp(f->trace, t->trace, end) == 0);
    CHECK(f->flash.calls == cut && f->reentries == end);
    for (size_t i = 0u; i < end; ++i)
        if (t->trace[i] == 'b') ++attempts; /* each complete vvbv attempt */
    CHECK(f->attempts == attempts);
    if (end <= t->reset_end)
    {
        CHECK(r->valid == 1u && f->attempts == 0u);
        CHECK(r->reset_status == BOOT_RESET_HANDLER_STORE_FAILED);
        CHECK(r->reset.store_result == (end > t->commit_at && end <= t->replace_end ?
            BOOT_STORE_UNCERTAIN : BOOT_STORE_IO));
        CHECK(r->status == (end > t->commit_at && end <= t->replace_end ?
            BOOT_RUNTIME_RECONCILE : BOOT_RUNTIME_RESET_FAILED));
    }
    else
    {
        CHECK(r->valid == 3u && r->reset_status == BOOT_RESET_HANDLER_OK);
        CHECK(r->selection.store_result == (end > t->commit_at && end <= t->replace_end ?
            BOOT_STORE_UNCERTAIN : BOOT_STORE_IO));
        CHECK(r->selection.status == (end > t->commit_at && end <= t->replace_end ?
            BOOT_SELECT_RECONCILE : BOOT_SELECT_STORE_FAILED));
        CHECK(r->status == (end > t->commit_at && end <= t->replace_end ?
            BOOT_RUNTIME_RECONCILE : BOOT_RUNTIME_SELECT_FAILED));
    }
}
/* Enumerate every real flash callback boundary, including both sides of reset
 * and selection commit. Explicit read-only reload is not composition retry. */
static void flash_faults(void)
{
    for (unsigned mode = 0u; mode < 3u; ++mode)
    for (unsigned current = 0u; current < 2u; ++current)
    for (unsigned page = 0u; page < 2u; ++page)
    {
        fixture_t baseline;
        fault_trace_t expected = expected_fault_trace(mode);
        boot_runtime_result_t good;
        unsigned kind = mode == 0u ? 2u : mode == 1u ? 1u : 4u;
        prepare(&baseline, current, page, kind, 0u, kind == 2u);
        baseline.captured = mode == 0u ? 8u : 2u;
        if (mode == 2u) baseline.payload[current][20] ^= 1u;
        good = run(&baseline);
        CHECK(good.status == BOOT_RUNTIME_OK && baseline.commits == 1u);
        CHECK(baseline.logs == expected.length &&
            memcmp(baseline.trace, expected.trace, expected.length) == 0);
        CHECK(baseline.flash.calls == expected.store_calls);
        CHECK(baseline.commit[0] == expected.commit_call);
        for (size_t cut = 1u; cut <= expected.store_calls; ++cut)
        for (unsigned after = 0u; after < 2u; ++after)
        {
            fixture_t f;
            boot_runtime_result_t r;
            boot_store_snapshot_t loaded;
            prepare(&f, current, page, kind, 0u, kind == 2u);
            f.captured = baseline.captured;
            if (mode == 2u) f.payload[current][20] ^= 1u;
            f.flash.cut_call = cut; f.flash.fail_after = (int)after;
            r = run(&f);
            failed_trace(&f, &expected, cut, &r);
            CHECK(r.status != BOOT_RUNTIME_OK);
            if (cut >= expected.commit_call &&
                (mode != 0u || cut <= expected.commit_call + 3u))
            {
                CHECK(r.status == BOOT_RUNTIME_RECONCILE);
                if (mode == 0u)
                    CHECK(r.reset_status == BOOT_RESET_HANDLER_STORE_FAILED &&
                        r.reset.store_result == BOOT_STORE_UNCERTAIN && r.valid == 1u);
                else
                    CHECK(r.selection.status == BOOT_SELECT_RECONCILE &&
                        r.selection.store_result == BOOT_STORE_UNCERTAIN && r.valid == 3u);
            }
            CHECK(memcmp(&f.out, &f.sentinel, sizeof f.out) == 0);
            if (r.reset_status != BOOT_RESET_HANDLER_OK)
            {
                CHECK(r.valid == 1u && f.attempts == 0u);
                CHECK((r.reset.valid & BOOT_RESET_REPORT_SNAPSHOT) == 0u);
                CHECK(r.status == (r.reset.store_result == BOOT_STORE_UNCERTAIN ?
                    BOOT_RUNTIME_RECONCILE : BOOT_RUNTIME_RESET_FAILED));
            }
            else
            {
                CHECK(r.valid == 3u);
                CHECK(r.status == (r.selection.status == BOOT_SELECT_RECONCILE ?
                    BOOT_RUNTIME_RECONCILE : BOOT_RUNTIME_SELECT_FAILED));
                /* Evidence mask is NOT a complete verifier attempt trace. */
                if (r.selection.evidence_mask != 0u) CHECK(f.attempts != 0u);
            }
            duplicate(&f);
            f.flash.stopped = 0; f.flash.cut_call = SIZE_MAX;
            CHECK(boot_store_load(&f.inner, &loaded) == BOOT_STORE_OK);
            /* Independent durable-effect oracle: commit before-effect leaves
             * old authority, after-effect or failed later read leaves new. */
            if (cut > expected.commit_call || (cut == expected.commit_call && after))
            {
                CHECK(loaded.record.sequence == 101u && loaded.page == 1u-page);
                CHECK(memcmp(&loaded.record, &baseline.out.snapshot.record, sizeof loaded.record) == 0);
            }
            else
            {
                CHECK(loaded.record.sequence == 100u && loaded.page == page);
                CHECK(memcmp(&loaded.record, &f.original, sizeof loaded.record) == 0);
            }
            duplicate(&f); /* Reload never authorizes replay or selection-only retry. */
        }
    }
}
static void arguments_and_verify_failures(void)
{
    fixture_t f;
    boot_runtime_result_t r;
    prepare(&f, 0u, 0u, 2u, 0u, 1u);
#define INVALID(call) do { r = (call); CHECK(r.status == BOOT_RUNTIME_ARGUMENT && r.valid == 0u); CHECK(f.flash.calls == 0u && f.reg_calls == 0u && f.runtime.consumed == 0u); CHECK(memcmp(&f.out, &f.sentinel, sizeof f.out) == 0); } while (0)
    INVALID(boot_runtime_run(NULL, &f.registers, &f.store, &f.images, payload_read, &f, progress, &f, &f.out));
    INVALID(boot_runtime_run(&f.runtime, NULL, &f.store, &f.images, payload_read, &f, progress, &f, &f.out));
    INVALID(boot_runtime_run(&f.runtime, &f.registers, NULL, &f.images, payload_read, &f, progress, &f, &f.out));
    INVALID(boot_runtime_run(&f.runtime, &f.registers, &f.store, NULL, payload_read, &f, progress, &f, &f.out));
    INVALID(boot_runtime_run(&f.runtime, &f.registers, &f.store, &f.images, NULL, &f, progress, &f, &f.out));
    INVALID(boot_runtime_run(&f.runtime, &f.registers, &f.store, &f.images, payload_read, &f, progress, &f, NULL));
    for (unsigned offset = 0u; offset < 2u; ++offset)
    {
        INVALID(boot_runtime_run(&f.runtime, &f.registers, &f.store, &f.images, payload_read, &f, progress, &f,
            (boot_launch_decision_t *)(void *)((uint8_t *)&f.images + offset)));
        INVALID(boot_runtime_run(&f.runtime, &f.registers, &f.store, &f.images, payload_read, &f, progress, &f,
            (boot_launch_decision_t *)(void *)((uint8_t *)&f.store + offset)));
        INVALID(boot_runtime_run(&f.runtime, &f.registers, &f.store, &f.images, payload_read, &f, progress, &f,
            (boot_launch_decision_t *)(void *)((uint8_t *)&f.registers + offset)));
        INVALID(boot_runtime_run(&f.runtime, &f.registers, &f.store, &f.images, payload_read, &f, progress, &f,
            (boot_launch_decision_t *)(void *)((uint8_t *)&f.runtime + offset)));
    }
    f.registers.read = NULL; INVALID(run(&f)); f.registers.read = reg_read;
    f.registers.write = NULL; INVALID(run(&f)); f.registers.write = reg_write;
    f.store.read = NULL; INVALID(run(&f)); f.store.read = metadata_read;
    f.store.erase_page = NULL; INVALID(run(&f)); f.store.erase_page = metadata_erase;
    f.store.program_word = NULL; INVALID(run(&f)); f.store.program_word = metadata_program;
#undef INVALID
    CHECK(run(&f).status == BOOT_RUNTIME_OK); duplicate(&f);
    for (unsigned mode = 0u; mode < 4u; ++mode)
    {
        prepare(&f, 0u, 0u, 2u, 0u, 1u); f.captured = 8u;
        if (mode == 0u) f.short_read = 1;
        if (mode == 1u) f.abort_phase = BOOT_VERIFY_MANIFEST_BEGIN;
        if (mode == 2u) f.images.size[1] = 0u;
        if (mode == 3u) memset(f.flash.bytes, 0xff, sizeof f.flash.bytes);
        r = run(&f);
        CHECK(r.reset_status == (mode == 3u ? BOOT_RESET_HANDLER_RECOVERY : BOOT_RESET_HANDLER_OK));
        if (mode < 2u) CHECK(r.status == BOOT_RUNTIME_SELECT_FAILED && r.selection.status == BOOT_SELECT_VERIFY_FAILED);
        if (mode == 2u) CHECK(r.status == BOOT_RUNTIME_OK && f.selected == 0u); /* candidate invalid fallback */
        if (mode == 3u) CHECK(r.status == BOOT_RUNTIME_RESET_FAILED && f.attempts == 0u);
        duplicate(&f);
    }
#ifdef BOOT_VERIFY_FAULT_INJECTION
    prepare(&f, 0u, 0u, 2u, 0u, 1u); f.captured = 8u;
    fail_allocation = 1; r = run(&f); fail_allocation = 0;
    CHECK(r.status == BOOT_RUNTIME_SELECT_FAILED && r.selection.status == BOOT_SELECT_VERIFY_FAILED);
    CHECK(r.selection.verify_result[1] == BOOT_VERIFY_ERROR_CRYPTO && live_allocations == 0u);
    duplicate(&f);
#endif
}
int main(void)
{
    matrix(); policies_and_copy(); fresh_boot_and_no_intent();
    reset_register_faults(); flash_faults();
    arguments_and_verify_failures();
    printf("boot runtime: %u checks\n", checks);
    return 0;
}
