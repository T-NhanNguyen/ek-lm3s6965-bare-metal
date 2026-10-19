#include "lm3s6965/boot_runtime_watchdog.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_reset_handler.h"
#include "lm3s6965/boot_runtime.h"
#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"
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
    boot_runtime_watchdog_t *bridge;
    boot_watchdog_t *w;
    boot_reset_register_adapter_t *registers;
    boot_store_adapter_t *store;
    boot_select_images_t *images;
    boot_launch_decision_t *out;
    boot_launch_decision_t sentinel;
    boot_handoff_t *handoff;
    boot_handoff_context_t *observed;
    boot_runtime_watchdog_launch_t *launch_out;
    boot_runtime_watchdog_launch_t launch_sentinel;
    unsigned h_calls, h_cut, h_after, h_effects, terminal_recorded;
    unsigned h_feeds, h_wdt_calls, h_verification;
    uint32_t h_epoch, h_vtor;
    size_t h_flash_calls;
    boot_handoff_operation_t h_trace[11];
    flash_model_t flash;
    boot_store_adapter_t inner;
    uint8_t payload[2][BOOT_PAYLOAD_MAX];
    uint32_t length[2], clock, ctl, load, ris, locked, captured;
    unsigned active, reg_calls, reg_cut, reads, short_read, mutate;
    unsigned calls, cut, after, feeds, initial_clears, reentries;
    unsigned corrupt, persistent;
    size_t commit_call;
    unsigned commit_feeds;
    struct { uint32_t address, id, epoch, completed; unsigned write; } trace[8192];
} context_t;

typedef struct
{
    context_t c;
    boot_runtime_watchdog_t bridge;
    boot_watchdog_t w;
    boot_reset_register_adapter_t registers;
    boot_store_adapter_t store;
    boot_select_images_t images;
    boot_launch_decision_t out;
    boot_handoff_t handoff;
    boot_handoff_context_t observed;
    boot_runtime_watchdog_launch_t launch_out;
    boot_metadata_t original;
} fixture_t;

static size_t payload_read(void *, uint32_t, uint8_t *, size_t);
static boot_runtime_watchdog_result_t run(context_t *c)
{
    if (c->launch_out != NULL)
        return boot_runtime_watchdog_launch(c->bridge, c->w, c->handoff,
            c->observed, c->registers, c->store, c->images, payload_read, c,
            c->launch_out);
    return boot_runtime_watchdog_run(c->bridge, c->w, c->registers, c->store,
        c->images, payload_read, c, c->out);
}
static void boundary(context_t *c)
{
    if (!c->active) return;
    CHECK(memcmp(c->out, &c->sentinel, sizeof *c->out) == 0);
    CHECK(c->bridge->consumed == 1u && c->bridge->runtime.consumed == 1u);
    if (c->launch_out != NULL)
    {
        boot_handoff_request_t request = {0};
        boot_handoff_intent_t intent;
        CHECK(memcmp(c->launch_out, &c->launch_sentinel, sizeof *c->launch_out) == 0);
        CHECK(boot_handoff_run(c->handoff, c->w, &request, &intent) == BOOT_HANDOFF_BUSY);
        CHECK(boot_runtime_watchdog_run(c->bridge, c->w, c->registers, c->store,
            c->images, payload_read, c, c->out).status == BOOT_RUNTIME_WATCHDOG_BUSY);
    }
    CHECK(run(c).status == BOOT_RUNTIME_WATCHDOG_BUSY);
    CHECK(boot_watchdog_service(c->w, BOOT_WATCHDOG_BOOT, c->w->epoch) == BOOT_WATCHDOG_BUSY);
    CHECK(boot_watchdog_boot_report(c->w, c->w->epoch, c->w->verification + 1u,
        BOOT_VERIFY_MANIFEST_BEGIN, 0u, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_BUSY);
    CHECK(boot_watchdog_transfer(c->w, c->w->epoch) == BOOT_WATCHDOG_BUSY);
    CHECK(boot_watchdog_configure(c->w, 100u, 500u) == BOOT_WATCHDOG_BUSY);
    CHECK(boot_watchdog_application_report(c->w, c->w->epoch,
        BOOT_WATCHDOG_ETHERNET, 1u) == BOOT_WATCHDOG_BUSY);
    ++c->reentries;
}
static int reg_read(void *p, uint32_t *value)
{
    context_t *c = p;
    boundary(c); ++c->reg_calls;
    CHECK(c->calls == 0u); /* No invented progress before BEGIN. */
    *value = c->captured;
    if (c->mutate && c->reg_calls == 1u)
    {
        memset(c->images, 0, sizeof *c->images);
        c->registers->read = NULL; c->registers->write = NULL;
        c->store->read = NULL; c->store->erase_page = NULL; c->store->program_word = NULL;
    }
    return c->reg_calls == c->reg_cut ? -1 : 0;
}
static int reg_write(void *p, uint32_t value)
{
    context_t *c = p;
    boundary(c); ++c->reg_calls;
    CHECK(value == 0u && c->calls == 0u);
    return c->reg_calls == c->reg_cut ? -1 : 0;
}
static int metadata_read(void *p, uint32_t address, uint8_t *bytes, size_t size)
{
    context_t *c = p; boundary(c);
    return c->inner.read(c->inner.context, address, bytes, size);
}
static int metadata_erase(void *p, uint32_t address)
{
    context_t *c = p; boundary(c);
    return c->inner.erase_page(c->inner.context, address);
}
static int metadata_program(void *p, uint32_t address, uint32_t word)
{
    context_t *c = p; boundary(c);
    if ((address - BOOT_METADATA_PAGE0) % BOOT_ERASE_BYTES == BOOT_METADATA_BYTES - 4u)
    {
        c->commit_call = c->flash.calls + 1u;
        c->commit_feeds = c->feeds;
    }
    return c->inner.program_word(c->inner.context, address, word);
}
static size_t payload_read(void *p, uint32_t address, uint8_t *bytes, size_t size)
{
    context_t *c = p;
    unsigned slot = address >= BOOT_SLOT_B_BASE ? 1u : 0u;
    uint32_t base = slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    uint32_t offset;
    boundary(c);
    CHECK(c->w->verification != 0u && c->w->phase == 2u && c->w->evidence == 0u);
    CHECK(address == base + c->w->completed);
    CHECK(size == (c->length[slot] - c->w->completed < BOOT_VERIFY_CHUNK_BYTES ?
        c->length[slot] - c->w->completed : BOOT_VERIFY_CHUNK_BYTES));
    CHECK(address >= base);
    offset = address - base;
    CHECK(c->length[slot] <= sizeof c->payload[slot]);
    CHECK(offset <= c->length[slot] && size <= c->length[slot] - offset);
    ++c->reads;
    memcpy(bytes, c->payload[slot] + offset, size);
    return c->short_read ? size - 1u : size;
}
static int wdt_fail(context_t *c)
{
    return c->calls == c->cut || (c->persistent && c->calls >= c->cut);
}
static void wdt_boundary(context_t *c, uint32_t address, unsigned write)
{
    boundary(c);
    CHECK(c->calls < sizeof c->trace / sizeof c->trace[0]);
    c->trace[c->calls].address = address;
    c->trace[c->calls].write = write;
    c->trace[c->calls].id = c->w->verification;
    c->trace[c->calls].epoch = c->w->epoch;
    c->trace[c->calls].completed = c->w->completed;
    ++c->calls;
    if (c->active)
    {
        CHECK(c->w->phase == 2u); /* BEGIN is NEVER a feed. */
        CHECK(c->w->total == BOOT_PAYLOAD_MIN || c->w->total == BOOT_PAYLOAD_MAX);
    }
}
static int wdt_read(void *p, uint32_t address, uint32_t *value)
{
    context_t *c = p;
    wdt_boundary(c, address, 0u);
    if (wdt_fail(c) && !c->after) return -1;
    switch (address)
    {
        case BOOT_WATCHDOG_RCGC0: *value = c->clock; break;
        case BOOT_WATCHDOG_CTL: *value = c->ctl; break;
        case BOOT_WATCHDOG_LOAD: *value = c->load; break;
        case BOOT_WATCHDOG_RIS: *value = c->ris; break;
        case BOOT_WATCHDOG_LOCK: *value = c->locked; break;
        default: CHECK(0); return -1;
    }
    if (c->active && address == c->corrupt)
        *value ^= address == BOOT_WATCHDOG_RCGC0 ? 8u : 1u;
    return wdt_fail(c) ? -1 : 0;
}
static int wdt_write(void *p, uint32_t address, uint32_t value)
{
    context_t *c = p;
    wdt_boundary(c, address, 1u);
    if (wdt_fail(c) && !c->after) return -1;
    if (address == BOOT_WATCHDOG_LOCK)
        c->locked = value == UINT32_C(0x1acce551) ? 0u : 1u;
    else
    {
        CHECK(c->locked == 0u);
        switch (address)
        {
            case BOOT_WATCHDOG_LOAD: c->load = value; break;
            case BOOT_WATCHDOG_CTL: if (!(c->ctl & 1u)) c->ctl = value; break;
            case BOOT_WATCHDOG_ICR:
                c->ris = 0u;
                if (c->ctl & 1u) ++c->feeds; else ++c->initial_clears;
                break;
            default: CHECK(0); return -1;
        }
    }
    return wdt_fail(c) ? -1 : 0;
}
static void handoff_boundary(context_t *c)
{
    boundary(c);
    if (c->h_calls == 0u)
    {
        c->h_feeds = c->feeds; c->h_wdt_calls = c->calls;
        c->h_flash_calls = c->flash.calls;
        c->h_epoch = c->w->epoch; c->h_verification = c->w->verification;
    }
    CHECK(c->feeds == c->h_feeds && c->calls == c->h_wdt_calls);
    CHECK(c->flash.calls == c->h_flash_calls);
    CHECK(c->w->verification == c->h_verification && c->w->evidence == 0u);
    ++c->h_calls;
}
static int handoff_operation(void *p, boot_handoff_operation_t op, uint32_t value)
{
    static const boot_handoff_operation_t order[11] = {
        BOOT_HANDOFF_EXCLUDE, BOOT_HANDOFF_STOP_SYSTICK,
        BOOT_HANDOFF_DISABLE_BANK0, BOOT_HANDOFF_DISABLE_BANK1,
        BOOT_HANDOFF_QUIESCE_SOURCES, BOOT_HANDOFF_CLEAR_CORE_PENDING,
        BOOT_HANDOFF_CLEAR_BANK0_PENDING, BOOT_HANDOFF_CLEAR_BANK1_PENDING,
        BOOT_HANDOFF_BARRIER_BEFORE_VTOR, BOOT_HANDOFF_SET_VTOR,
        BOOT_HANDOFF_BARRIER_AFTER_VTOR
    };
    context_t *c = p;
    handoff_boundary(c);
    CHECK(c->h_calls <= 11u && op == order[c->h_calls - 1u]);
    CHECK(value == (op == BOOT_HANDOFF_SET_VTOR ? c->h_vtor : 0u));
    CHECK(c->w->owner == BOOT_WATCHDOG_BOOT && c->w->epoch == c->h_epoch);
    c->h_trace[c->h_calls - 1u] = op;
    if (c->h_calls == c->h_cut && !c->h_after) return -1;
    ++c->h_effects;
    return c->h_calls == c->h_cut ? -1 : 0;
}
static int terminal_intent(void *p, const boot_handoff_intent_t *intent)
{
    context_t *c = p;
    handoff_boundary(c);
    CHECK(c->h_calls == 12u && c->w->owner == BOOT_WATCHDOG_APPLICATION);
    CHECK(c->w->epoch == c->h_epoch + 1u);
    CHECK(intent->application_epoch == c->w->epoch);
    CHECK(intent->msp == BOOT_INITIAL_MSP && intent->control == 0u);
    CHECK(intent->primask == 0u && intent->basepri == 0u && intent->faultmask == 0u);
    CHECK(intent->vtor == c->h_vtor);
    CHECK(intent->reset_vector == intent->vtor + 9u);
    if (c->h_calls == c->h_cut && !c->h_after) return -1;
    c->terminal_recorded = 1u; ++c->h_effects;
    return c->h_calls == c->h_cut ? -1 : 0;
}
static void enable_launch(fixture_t *f)
{
    boot_handoff_io_t io = {&f->c, handoff_operation, terminal_intent};
    uint32_t slot = f->original.candidate_slot == BOOT_SLOT_NONE ?
        f->original.current_slot : f->original.candidate_slot;
    f->c.h_vtor = slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    f->c.handoff = &f->handoff; f->c.observed = &f->observed;
    f->c.launch_out = &f->launch_out;
    memset(&f->c.launch_sentinel, 0xa6, sizeof f->c.launch_sentinel);
    f->launch_out = f->c.launch_sentinel;
    f->observed = (boot_handoff_context_t){0u, 2u, 1u, 0xe0u, 1u};
    CHECK(boot_handoff_init(&f->handoff, &io) == BOOT_HANDOFF_OK);
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) p[i] = (uint8_t)(value >> (8u * i));
}
/* Each helper declaration below owns a distinct block-static fixture to avoid
 * oversized inlined host stack frames. prepare() resets the full object and
 * rebinds its pointers before each use; baseline and trial never share storage.
 * Array-bound sanitizer coverage remains; fixture stack-lifetime diagnostics
 * (use-after-scope/return) are intentionally no longer available.
 * kind 0=current, 1=READY candidate, 2=current+previous, 3=TRIAL candidate. */
static void prepare(fixture_t *f, unsigned current, unsigned page,
                    unsigned kind, unsigned mode)
{
    context_t *c;
    boot_watchdog_io_t io;
    memset(f, 0, sizeof *f); c = &f->c;
    c->bridge = &f->bridge; c->w = &f->w; c->registers = &f->registers;
    c->store = &f->store; c->images = &f->images; c->out = &f->out;
    flash_model_init(&c->flash); c->inner = flash_model_adapter(&c->flash);
    f->store = (boot_store_adapter_t){c, metadata_read, metadata_erase, metadata_program};
    f->registers = (boot_reset_register_adapter_t){c, reg_read, reg_write};
    boot_runtime_watchdog_init(&f->bridge);
    c->clock = 8u; c->locked = 1u;
    io = (boot_watchdog_io_t){c, wdt_read, wdt_write};
    CHECK(boot_watchdog_init(&f->w, &io) == BOOT_WATCHDOG_OK);
    CHECK(boot_watchdog_configure(&f->w, 100u, 500u) == BOOT_WATCHDOG_OK);
    CHECK(c->feeds == 0u && c->initial_clears == 1u);
    c->calls = 0u; c->captured = 2u; /* uncounted POR */
    f->original.sequence = 100u; f->original.current_slot = current;
    f->original.previous_slot = kind == 2u ? 1u-current : BOOT_SLOT_NONE;
    f->original.candidate_slot = kind == 1u || kind == 3u ? 1u-current : BOOT_SLOT_NONE;
    f->original.confirmed_mask = kind == 2u ? 3u : 1u << current;
    f->original.intent_slot = BOOT_SLOT_NONE;
    for (unsigned slot = 0u; slot < 2u; ++slot)
    {
        boot_manifest_t m;
        uint32_t base = slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
        memcpy(f->images.wire[slot], fixture_manifest[slot * 7u + mode], BOOT_MANIFEST_BYTES);
        f->images.size[slot] = BOOT_MANIFEST_BYTES;
        CHECK(boot_manifest_decode(f->images.wire[slot], BOOT_MANIFEST_BYTES, &m) == BOOT_OK);
        c->length[slot] = m.payload_length;
        for (unsigned i = 0u; i < m.payload_length; ++i)
            c->payload[slot][i] = (uint8_t)(i * 17u + slot * 31u);
        memset(c->payload[slot] + 100u, 0xff, 8u);
        memset(c->payload[slot] + m.payload_length - 4u, 0xff, 4u);
        put32(c->payload[slot], mode == 2u ? BOOT_INITIAL_MSP - 4u : BOOT_INITIAL_MSP);
        put32(c->payload[slot] + 4u, base + (mode == 3u ? 8u : 9u));
        memcpy(f->original.images[slot].payload_hash, m.payload_hash, 32u);
        f->original.images[slot].release = m.release;
        f->original.images[slot].payload_length = m.payload_length;
        f->original.images[slot].trial_id = 20u + slot;
    }
    if (kind == 1u || kind == 3u)
        f->original.images[1u-current].accounting = kind == 1u ? 1u : 2u;
    CHECK(boot_metadata_validate(&f->original) == BOOT_OK);
    CHECK(boot_metadata_encode(&f->original, BOOT_COMMIT_VALID,
        c->flash.bytes + page * BOOT_ERASE_BYTES, BOOT_METADATA_BYTES) == BOOT_OK);
    memset(&c->sentinel, 0xa5, sizeof c->sentinel); f->out = c->sentinel;
    c->active = 1u;
}
static void duplicate(fixture_t *f)
{
    unsigned calls = f->c.calls, regs = f->c.reg_calls, feeds = f->c.feeds;
    size_t flash = f->c.flash.calls;
    boot_launch_decision_t before = f->out;
    boot_runtime_watchdog_launch_t launch_before = f->launch_out;
    unsigned h_calls = f->c.h_calls;
    boot_runtime_watchdog_result_t r = run(&f->c);
    CHECK(r.status == BOOT_RUNTIME_WATCHDOG_CONSUMED && r.valid == 0u);
    CHECK(f->c.calls == calls && f->c.reg_calls == regs && f->c.feeds == feeds);
    CHECK(f->c.flash.calls == flash && memcmp(&before, &f->out, sizeof before) == 0);
    CHECK(f->c.h_calls == h_calls);
    if (f->c.launch_out != NULL)
    {
        CHECK(memcmp(&launch_before, &f->launch_out, sizeof launch_before) == 0);
        CHECK(boot_runtime_watchdog_run(f->c.bridge, &f->w, &f->registers,
            &f->store, &f->images, payload_read, &f->c, &f->out).status ==
            BOOT_RUNTIME_WATCHDOG_CONSUMED);
    }
    if (f->w.blocked != 0u)
    {
        CHECK(boot_watchdog_boot_report(&f->w, f->w.epoch, 1u,
            BOOT_VERIFY_MANIFEST_BEGIN, 0u, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_BLOCKED);
        CHECK(boot_watchdog_service(&f->w, BOOT_WATCHDOG_BOOT,
            f->w.epoch) == BOOT_WATCHDOG_BLOCKED);
        CHECK(f->w.evidence == 0u);
        CHECK(f->c.calls == calls && f->c.reg_calls == regs && f->c.feeds == feeds);
        CHECK(f->c.flash.calls == flash && memcmp(&before, &f->out, sizeof before) == 0);
    }
}
static void single(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned mode = 0u; mode < 2u; ++mode)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        unsigned chunks, feeds;
        prepare(&f, slot, page, 0u, mode);
        /* Adopt an already-used lifetime stream counter, not slot/trial IDs. */
        CHECK(boot_watchdog_boot_report(&f.w, f.w.epoch, 73u,
            BOOT_VERIFY_MANIFEST_BEGIN, 0u, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
        r = run(&f.c);
        chunks = (f.c.length[slot] + 255u) / 256u; feeds = chunks + 1u;
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_OK && r.valid == 7u);
        CHECK(r.runtime.status == BOOT_RUNTIME_OK && r.runtime.valid == 3u);
        CHECK(r.watchdog == BOOT_WATCHDOG_OK && r.operation == BOOT_RUNTIME_WATCHDOG_SERVICE);
        CHECK(r.phase == BOOT_VERIFY_PAYLOAD_PROGRESS && r.completed == f.c.length[slot]);
        CHECK(r.verification == 74u && r.epoch == feeds && f.w.epoch == feeds + 1u);
        CHECK(f.w.verification == 74u && f.w.evidence == 0u && f.w.blocked == 0u);
        CHECK(f.c.feeds == feeds && f.c.calls == feeds * 9u && f.c.reads == chunks);
        CHECK(f.c.flash.erases == 0u && f.c.flash.writes == 0u);
        CHECK(f.out.image.manifest.slot == slot);
        for (unsigned n = 0u; n < feeds; ++n)
        {
            uint32_t completed = n == 0u ? 0u : n * 256u;
            if (completed > f.c.length[slot]) completed = f.c.length[slot];
            for (unsigned j = 0u; j < 9u; ++j)
            {
                CHECK(f.c.trace[n*9u+j].id == 74u);
                CHECK(f.c.trace[n*9u+j].epoch == n + 1u);
                CHECK(f.c.trace[n*9u+j].completed == completed);
            }
        }
        duplicate(&f);
    }
}
static void preflight(void)
{
    static fixture_t f;
    boot_runtime_watchdog_result_t r;
    for (unsigned bad = 0u; bad < 8u; ++bad)
    {
        prepare(&f, 0u, 0u, 0u, 0u);
        if (bad == 0u) f.w.configured = 0u;
        if (bad == 1u) f.w.blocked = 1u;
        if (bad == 2u) f.w.owner = BOOT_WATCHDOG_APPLICATION;
        if (bad == 3u) f.w.reload = 0u;
        if (bad == 4u) atomic_flag_test_and_set(&f.w.entered);
        if (bad == 5u) f.w.epoch = 0u;
        if (bad == 6u) f.w.io.read_register = NULL;
        if (bad == 7u) f.w.io.write_register = NULL;
        r = run(&f.c);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_REJECTED && r.valid == 1u);
        CHECK(r.operation == BOOT_RUNTIME_WATCHDOG_PREFLIGHT);
        CHECK(r.watchdog == (bad == 1u ? BOOT_WATCHDOG_BLOCKED :
            bad == 4u ? BOOT_WATCHDOG_BUSY : BOOT_WATCHDOG_STATE));
        CHECK(f.bridge.consumed == 0u && f.c.calls == 0u && f.c.reg_calls == 0u);
        CHECK(f.c.flash.calls == 0u && f.c.feeds == 0u);
        CHECK(memcmp(&f.out, &f.c.sentinel, sizeof f.out) == 0);
        CHECK(f.bridge.runtime.consumed == 0u && f.c.reads == 0u);
        /* Correct only the rejected precondition, without recreating either object. */
        if (bad == 0u) f.w.configured = 1u;
        if (bad == 1u) f.w.blocked = 0u;
        if (bad == 2u) f.w.owner = BOOT_WATCHDOG_BOOT;
        if (bad == 3u) f.w.reload = 500u;
        if (bad == 4u) atomic_flag_clear(&f.w.entered);
        if (bad == 5u) f.w.epoch = 1u;
        if (bad == 6u) f.w.io.read_register = wdt_read;
        if (bad == 7u) f.w.io.write_register = wdt_write;
        CHECK(run(&f.c).status == BOOT_RUNTIME_WATCHDOG_OK);
        duplicate(&f);
    }
}
static void structural(void)
{
    for (unsigned bad = 0u; bad < 12u; ++bad)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u);
        if (bad == 7u) f.registers.read = NULL;
        if (bad == 8u) f.registers.write = NULL;
        if (bad == 9u) f.store.read = NULL;
        if (bad == 10u) f.store.erase_page = NULL;
        if (bad == 11u) f.store.program_word = NULL;
        r = boot_runtime_watchdog_run(bad == 0u ? NULL : &f.bridge,
            bad == 1u ? NULL : &f.w, bad == 2u ? NULL : &f.registers,
            bad == 3u ? NULL : &f.store, bad == 4u ? NULL : &f.images,
            bad == 5u ? NULL : payload_read, &f.c, bad == 6u ? NULL : &f.out);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_ARGUMENT && r.valid == 0u);
        CHECK(f.bridge.consumed == 0u && f.bridge.runtime.consumed == 0u);
        CHECK(f.c.calls == 0u && f.c.reg_calls == 0u && f.c.flash.calls == 0u);
        CHECK(f.c.feeds == 0u && f.c.reads == 0u);
        CHECK(memcmp(&f.out, &f.c.sentinel, sizeof f.out) == 0);
        f.registers.read = reg_read; f.registers.write = reg_write;
        f.store.read = metadata_read; f.store.erase_page = metadata_erase;
        f.store.program_word = metadata_program;
        CHECK(run(&f.c).status == BOOT_RUNTIME_WATCHDOG_OK);
        duplicate(&f);
    }
}
static void multistream(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned kind = 1u; kind < 3u; ++kind)
    for (unsigned signature = 0u; signature < 2u; ++signature)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        unsigned invalid = kind == 1u ? 1u-slot : slot;
        unsigned selected = 1u-invalid;
        unsigned feeds = signature ? 2u : 4u;
        prepare(&f, slot, page, kind, 0u);
        if (signature) f.images.wire[invalid][96] ^= 1u;
        else f.c.payload[invalid][20] ^= 1u;
        r = run(&f.c);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_OK);
        CHECK(r.runtime.selection.evidence_mask == 3u);
        CHECK(r.runtime.selection.verify_result[invalid] ==
            (signature ? BOOT_VERIFY_ERROR_SIGNATURE : BOOT_VERIFY_ERROR_HASH));
        CHECK(r.runtime.selection.verify_result[selected] == BOOT_VERIFY_OK);
        CHECK(f.out.image.manifest.slot == selected && f.w.verification == 2u);
        CHECK(f.c.feeds == feeds && f.w.epoch == feeds + 1u);
        CHECK(f.c.reads == (signature ? 1u : 2u));
        CHECK(f.c.flash.erases == (kind == 2u ? 1u : 0u));
        CHECK(f.c.flash.writes == (kind == 2u ? 42u : 0u));
        CHECK(f.c.trace[0].id == (signature ? 2u : 1u));
        CHECK(f.c.trace[(feeds - 1u) * 9u].id == 2u);
        duplicate(&f);
    }
}
static void failures(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned error = 0u; error < 5u; ++error)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        boot_verify_result_t expected = BOOT_VERIFY_ERROR_READ;
        unsigned feeds = 1u;
        prepare(&f, slot, 0u, 2u, error == 3u ? 2u : error == 4u ? 3u : 0u);
        if (error == 0u) f.c.short_read = 1u;
        if (error == 1u) { f.w.verification = UINT32_MAX; expected = BOOT_VERIFY_ERROR_ABORTED; feeds = 0u; }
        if (error == 2u) { f.w.epoch = UINT32_MAX; expected = BOOT_VERIFY_ERROR_ABORTED; feeds = 0u; }
        if (error == 3u) { expected = BOOT_VERIFY_ERROR_MSP; feeds = 4u; }
        if (error == 4u) { expected = BOOT_VERIFY_ERROR_THUMB; feeds = 4u; }
        r = run(&f.c);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED);
        CHECK(r.runtime.selection.verify_result[slot] == expected);
        CHECK(f.w.blocked == 1u && f.w.evidence == 0u && f.c.feeds == feeds);
        CHECK(memcmp(&f.out, &f.c.sentinel, sizeof f.out) == 0);
        if (error == 1u || error == 2u)
        {
            CHECK(r.watchdog == BOOT_WATCHDOG_STATE);
            CHECK(r.operation == (error == 1u ? BOOT_RUNTIME_WATCHDOG_REPORT : BOOT_RUNTIME_WATCHDOG_SERVICE));
            CHECK(r.runtime.selection.status == BOOT_SELECT_VERIFY_FAILED);
            CHECK(r.runtime.selection.evidence_mask == (1u << slot));
            CHECK(f.w.epoch == (error == 2u ? UINT32_MAX : 1u));
            CHECK(f.c.reads == 0u);
        }
        else CHECK(r.watchdog == BOOT_WATCHDOG_OK); /* Progress is not validation. */
        duplicate(&f);
    }
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u);
        f.w.epoch = UINT32_MAX - 1u;
        r = run(&f.c);
        CHECK(r.watchdog == BOOT_WATCHDOG_STATE && r.phase == BOOT_VERIFY_PAYLOAD_PROGRESS);
        CHECK(r.runtime.selection.verify_result[0] == BOOT_VERIFY_ERROR_ABORTED);
        CHECK(f.w.epoch == UINT32_MAX && f.c.feeds == 1u && f.c.reads == 1u);
        CHECK(f.w.blocked == 1u && f.w.evidence == 0u);
        duplicate(&f);
        prepare(&f, 0u, 0u, 0u, 0u);
        f.images.wire[0][96] ^= 1u;
        r = run(&f.c);
        CHECK(r.runtime.selection.verify_result[0] == BOOT_VERIFY_ERROR_SIGNATURE);
        CHECK(r.watchdog == BOOT_WATCHDOG_OK && r.phase == BOOT_VERIFY_MANIFEST_BEGIN);
        CHECK(f.c.feeds == 0u && f.c.calls == 0u && f.c.reads == 0u);
        CHECK(f.w.blocked == 1u && f.w.evidence == 0u);
        duplicate(&f);
    }
    /* The final legal ID is usable; only the subsequent BEGIN fails closed. */
    for (unsigned fallback = 0u; fallback < 2u; ++fallback)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 1u, fallback ? 2u : 0u, 0u);
        f.w.verification = UINT32_MAX - 1u;
        if (fallback) f.c.payload[0][20] ^= 1u;
        r = run(&f.c);
        CHECK(r.status == (fallback ? BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED : BOOT_RUNTIME_WATCHDOG_OK));
        CHECK(f.w.verification == UINT32_MAX && f.c.feeds == 2u);
        if (fallback)
        {
            CHECK(r.watchdog == BOOT_WATCHDOG_STATE && r.verification == UINT32_MAX);
            CHECK(r.runtime.selection.verify_result[1] == BOOT_VERIFY_ERROR_ABORTED);
        }
        duplicate(&f);
    }
#ifdef BOOT_VERIFY_FAULT_INJECTION
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 2u, 0u);
        fail_allocation = 1; r = run(&f.c); fail_allocation = 0;
        CHECK(r.runtime.selection.status == BOOT_SELECT_VERIFY_FAILED);
        CHECK(r.runtime.selection.verify_result[0] == BOOT_VERIFY_ERROR_CRYPTO);
        CHECK(r.runtime.selection.evidence_mask == 1u);
        CHECK(r.watchdog == BOOT_WATCHDOG_OK && r.phase == BOOT_VERIFY_MANIFEST_BEGIN);
        CHECK(f.c.feeds == 0u && f.c.reads == 0u && f.w.evidence == 0u && f.w.blocked == 1u);
        duplicate(&f);
    }
#endif
}
static void wdt_faults(void)
{
    static const uint32_t addresses[9] = {
        BOOT_WATCHDOG_RCGC0, BOOT_WATCHDOG_CTL, BOOT_WATCHDOG_LOAD,
        BOOT_WATCHDOG_LOCK, BOOT_WATCHDOG_LOCK, BOOT_WATCHDOG_ICR,
        BOOT_WATCHDOG_RIS, BOOT_WATCHDOG_LOCK, BOOT_WATCHDOG_LOCK};
    static const unsigned writes[9] = {0u,0u,0u,1u,0u,1u,0u,1u,0u};
    for (unsigned stream = 0u; stream < 2u; ++stream)
    for (unsigned cut = 1u; cut <= 18u; ++cut)
    for (unsigned after = 0u; after < 2u; ++after)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        unsigned prior = stream * 18u, local = (cut - 1u) % 9u + 1u;
        unsigned feeds = stream * 2u + (cut > 9u ? 1u : 0u);
        prepare(&f, 0u, stream, stream ? 2u : 0u, 0u);
        if (stream) f.c.payload[0][20] ^= 1u;
        f.c.cut = prior + cut; f.c.after = after;
        r = run(&f.c);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED);
        CHECK(r.watchdog == BOOT_WATCHDOG_IO && r.operation == BOOT_RUNTIME_WATCHDOG_SERVICE);
        CHECK(r.runtime.selection.status == BOOT_SELECT_VERIFY_FAILED);
        CHECK(r.runtime.selection.verify_result[stream] == BOOT_VERIFY_ERROR_ABORTED);
        CHECK(r.verification == stream + 1u);
        CHECK(r.phase == (cut <= 9u ? BOOT_VERIFY_MANIFEST_AUTHENTICATED : BOOT_VERIFY_PAYLOAD_PROGRESS));
        CHECK(f.c.calls == prior + cut + (local > 3u ? 2u : 0u));
        if (local > 6u || (local == 6u && after)) ++feeds;
        CHECK(f.c.feeds == feeds && f.w.evidence == 0u && f.w.blocked == 1u);
        CHECK(f.c.reads == stream + (cut > 9u ? 1u : 0u));
        CHECK(f.w.epoch == stream * 2u + (cut > 9u ? 2u : 1u));
        CHECK(memcmp(&f.out, &f.c.sentinel, sizeof f.out) == 0);
        for (unsigned i = 0u; i < prior + cut; ++i)
        {
            CHECK(f.c.trace[i].address == addresses[i % 9u]);
            CHECK(f.c.trace[i].write == writes[i % 9u]);
            CHECK(f.c.trace[i].id == (i < prior ? 1u : stream + 1u));
            CHECK(f.c.trace[i].epoch == i / 9u + 1u);
        }
        if (local > 3u)
        {
            CHECK(f.c.trace[prior+cut].address == BOOT_WATCHDOG_LOCK && f.c.trace[prior+cut].write == 1u);
            CHECK(f.c.trace[prior+cut+1u].address == BOOT_WATCHDOG_LOCK && f.c.trace[prior+cut+1u].write == 0u);
        }
        duplicate(&f);
    }
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u);
        f.c.cut = 6u; f.c.after = 1u; f.c.persistent = 1u;
        r = run(&f.c);
        CHECK(r.watchdog == BOOT_WATCHDOG_IO && f.w.blocked == 1u && f.w.evidence == 0u);
        CHECK(f.c.feeds == 1u && f.c.calls == 7u); /* Failed cleanup, no retry. */
        duplicate(&f);
    }
    for (unsigned n = 0u; n < 5u; ++n)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u);
        f.c.corrupt = addresses[n == 4u ? 6u : n];
        r = run(&f.c);
        CHECK(r.watchdog == BOOT_WATCHDOG_VERIFY && f.w.blocked == 1u && f.w.evidence == 0u);
        CHECK(r.runtime.selection.verify_result[0] == BOOT_VERIFY_ERROR_ABORTED);
        duplicate(&f);
    }
}
static void ready_auth_failure(void)
{
    /* READY's candidate authenticates first. A service failure is operational:
     * never try CURRENT, commit READY->TRIAL, or feed after cleanup. */
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned cut = 1u; cut <= 9u; ++cut)
    for (unsigned after = 0u; after < 2u; ++after)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        unsigned candidate = 1u - slot;
        prepare(&f, slot, page, 1u, 0u);
        f.c.cut = cut; f.c.after = after;
        r = run(&f.c);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED && r.valid == 7u);
        CHECK(r.runtime.status == BOOT_RUNTIME_SELECT_FAILED);
        CHECK(r.runtime.selection.status == BOOT_SELECT_VERIFY_FAILED);
        CHECK(r.runtime.selection.evidence_mask == (1u << candidate));
        CHECK(r.runtime.selection.verify_result[candidate] == BOOT_VERIFY_ERROR_ABORTED);
        CHECK(r.watchdog == BOOT_WATCHDOG_IO && r.operation == BOOT_RUNTIME_WATCHDOG_SERVICE);
        CHECK(r.phase == BOOT_VERIFY_MANIFEST_AUTHENTICATED);
        CHECK(r.verification == 1u && f.w.verification == 1u && r.epoch == 1u);
        CHECK(r.completed == 0u && r.total == f.c.length[candidate]);
        CHECK(f.w.epoch == 1u && f.w.blocked == 1u && f.w.evidence == 0u);
        CHECK(f.c.reads == 0u && f.c.flash.erases == 0u && f.c.flash.writes == 0u);
        CHECK(f.c.commit_call == 0u);
        CHECK(f.c.calls == cut + (cut > 3u ? 2u : 0u));
        CHECK(f.c.feeds == (cut > 6u || (cut == 6u && after) ? 1u : 0u));
        CHECK(memcmp(&f.out, &f.c.sentinel, sizeof f.out) == 0);
        duplicate(&f); /* Also proves public report/service BLOCKED with no IO. */
    }
}
static void reset_and_uncertain(void)
{
    for (unsigned cut = 1u; cut <= 3u; ++cut)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u); f.c.reg_cut = cut;
        f.w.evidence = 1u; /* Hostile preexisting evidence must not survive adoption. */
        r = run(&f.c);
        CHECK(r.runtime.status == BOOT_RUNTIME_RESET_FAILED && r.runtime.valid == 1u);
        CHECK(r.valid == 3u && r.watchdog == BOOT_WATCHDOG_OK);
        CHECK(f.c.calls == 0u && f.c.flash.calls == 0u && f.c.feeds == 0u);
        CHECK(f.w.blocked == 1u && f.w.evidence == 0u);
        duplicate(&f);
    }
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned kind = 1u; kind <= 3u; ++kind)
    {
        static fixture_t baseline;
        size_t commit;
        unsigned feeds;
        prepare(&baseline, slot, page, kind, 0u);
        if (kind == 2u) baseline.c.payload[slot][20] ^= 1u;
        if (kind == 3u) baseline.c.captured = 8u; /* counted WDT => accounting */
        CHECK(run(&baseline.c).status == BOOT_RUNTIME_WATCHDOG_OK);
        commit = baseline.c.commit_call; feeds = baseline.c.commit_feeds;
        CHECK(commit != 0u && feeds == (kind == 1u ? 2u : kind == 2u ? 4u : 0u));
        for (size_t offset = 0u; offset < 4u; ++offset)
        for (unsigned after = 0u; after < 2u; ++after)
        {
            static fixture_t f;
            boot_runtime_watchdog_result_t r;
            prepare(&f, slot, page, kind, 0u);
            if (kind == 2u) f.c.payload[slot][20] ^= 1u;
            if (kind == 3u) f.c.captured = 8u;
            f.c.flash.cut_call = commit + offset;
            f.c.flash.fail_after = (int)after;
            r = run(&f.c);
            CHECK(r.status == BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED && r.runtime.status == BOOT_RUNTIME_RECONCILE);
            CHECK(f.c.flash.calls == commit + offset && f.c.feeds == feeds);
            CHECK(f.w.blocked == 1u && f.w.evidence == 0u);
            CHECK(memcmp(&f.out, &f.c.sentinel, sizeof f.out) == 0);
            if (kind == 3u)
            {
                CHECK(r.runtime.valid == 1u && r.valid == 3u);
                CHECK(r.runtime.reset.store_result == BOOT_STORE_UNCERTAIN);
            }
            else CHECK(r.runtime.selection.store_result == BOOT_STORE_UNCERTAIN);
            duplicate(&f);
            {
                boot_store_snapshot_t observed;
                unsigned erases = f.c.flash.erases, writes = f.c.flash.writes;
                f.c.flash.stopped = 0; f.c.flash.cut_call = SIZE_MAX;
                /* Explicit read-only authority reconciliation is not boot replay. */
                CHECK(boot_store_load(&f.c.inner, &observed) == BOOT_STORE_OK);
                CHECK(observed.record.sequence == 100u + (offset != 0u || after ? 1u : 0u));
                CHECK(f.c.feeds == feeds && f.c.flash.erases == erases && f.c.flash.writes == writes);
                duplicate(&f);
            }
        }
    }
}
static void snapshots_and_aliases(void)
{
    static fixture_t f;
    boot_runtime_watchdog_result_t r;
    prepare(&f, 0u, 0u, 1u, 0u); f.c.mutate = 1u;
    r = run(&f.c);
    CHECK(r.status == BOOT_RUNTIME_WATCHDOG_OK && f.out.image.manifest.slot == 1u);
    CHECK(f.c.feeds == 2u && f.c.reads == 1u);
    duplicate(&f);
    /* Every pointer names an aligned, full-sized union member. Only the bridge
     * guard and valid transport table fields can be inspected before rejection. */
    for (unsigned alias = 0u; alias < 13u; ++alias)
    {
        union input_table {
            boot_reset_register_adapter_t registers;
            boot_store_adapter_t store;
            boot_select_images_t images;
        };
        union {
            boot_runtime_watchdog_t bridge;
            boot_watchdog_t watchdog;
            boot_launch_decision_t out;
            union input_table input;
            struct {
                unsigned char prefix[offsetof(boot_runtime_watchdog_t, runtime)];
                union input_table input;
            } tail;
            struct { size_t prefix; boot_launch_decision_t out; } partial;
        } overlap;
        boot_runtime_watchdog_t *bridge = &f.bridge;
        boot_watchdog_t *watchdog = &f.w;
        boot_launch_decision_t *out = &f.out;
        const boot_reset_register_adapter_t *registers = &f.registers;
        const boot_store_adapter_t *store = &f.store;
        const boot_select_images_t *images = &f.images;
        union input_table *input = &overlap.input;
        _Static_assert(offsetof(union input_table, registers) == 0u, "union base");
        _Static_assert(offsetof(boot_runtime_watchdog_t, runtime) %
            _Alignof(union input_table) == 0u, "aligned bridge tail");
        prepare(&f, 0u, 0u, 0u, 0u);
        memset(&overlap, 0, sizeof overlap);
        if (alias == 0u || alias == 1u || (alias >= 3u && alias < 6u))
        {
            boot_runtime_watchdog_init(&overlap.bridge);
            bridge = &overlap.bridge;
            if (alias == 0u) watchdog = &overlap.watchdog;
            if (alias == 1u) out = &overlap.out;
            input = &overlap.tail.input;
        }
        if (alias == 2u) { watchdog = &overlap.watchdog; out = &overlap.out; }
        if (alias >= 3u && alias < 12u)
        {
            unsigned table = (alias - 3u) % 3u;
            if (alias >= 6u && alias < 9u) watchdog = &overlap.watchdog;
            if (alias >= 9u) out = &overlap.out;
            /* NULL opaque contexts keep the overlapped embedded runtime's
             * zero state intact; no context is invoked on structural rejection. */
            if (table == 0u)
            {
                input->registers = (boot_reset_register_adapter_t){NULL, reg_read, reg_write};
                registers = &input->registers;
            }
            if (table == 1u)
            {
                input->store = (boot_store_adapter_t){NULL, metadata_read, metadata_erase, metadata_program};
                store = &input->store;
            }
            if (table == 2u) images = &input->images;
        }
        if (alias == 12u) { images = &overlap.input.images; out = &overlap.partial.out; }
        r = boot_runtime_watchdog_run(bridge, watchdog, registers, store,
            images, payload_read, &f.c, out);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_ARGUMENT && r.valid == 0u);
        CHECK(bridge->consumed == 0u && bridge->runtime.consumed == 0u);
        CHECK(f.c.calls == 0u && f.c.reg_calls == 0u && f.c.flash.calls == 0u);
        CHECK(f.c.feeds == 0u && f.c.reads == 0u);
        CHECK(memcmp(&f.out, &f.c.sentinel, sizeof f.out) == 0);
        /* Correct aliases, retaining the very bridge whose run was rejected. */
        f.c.bridge = bridge;
        CHECK(run(&f.c).status == BOOT_RUNTIME_WATCHDOG_OK);
        duplicate(&f);
    }
}
static void launch_success(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned role = 0u; role < 6u; ++role)
    for (unsigned mode = 0u; mode < 2u; ++mode)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        unsigned kind = role < 4u ? role : role == 4u ? 1u : 3u;
        unsigned selected = role == 1u || role == 2u || role == 3u || role == 5u ? 1u-slot : slot;
        unsigned streams = role == 2u || role == 4u ? 2u : 1u;
        unsigned chunks, feeds;
        boot_runtime_watchdog_launch_t saved;
        prepare(&f, slot, page, kind, mode); enable_launch(&f);
        f.c.h_vtor = selected == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
        if (role == 2u) f.c.payload[slot][20] ^= 1u;
        if (role == 4u) f.c.payload[1u-slot][20] ^= 1u;
        if (role == 5u) f.c.captured = 8u;
        CHECK(boot_watchdog_boot_report(&f.w, f.w.epoch, 73u,
            BOOT_VERIFY_MANIFEST_BEGIN, 0u, BOOT_PAYLOAD_MIN) == BOOT_WATCHDOG_OK);
        r = run(&f.c);
        chunks = (f.c.length[selected] + 255u) / 256u;
        feeds = streams * (chunks + 1u);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_OK && r.valid == 15u);
        CHECK(r.runtime.status == BOOT_RUNTIME_OK && r.handoff == BOOT_HANDOFF_OK);
        CHECK(f.c.h_calls == 12u && f.c.h_effects == 12u && f.c.terminal_recorded == 1u);
        CHECK(f.w.owner == BOOT_WATCHDOG_APPLICATION && f.w.epoch == feeds + 2u);
        CHECK(f.w.verification == 73u + streams && f.w.blocked == 0u && f.w.evidence == 0u);
        CHECK(f.handoff.finished == 1u && f.handoff.blocked == 0u);
        CHECK(f.c.feeds == feeds && f.c.calls == feeds * 9u);
        CHECK(f.launch_out.decision.image.manifest.slot == selected);
        CHECK(f.launch_out.intent.application_epoch == f.w.epoch);
        CHECK(f.launch_out.intent.vtor == f.c.h_vtor);
        CHECK(f.launch_out.intent.reset_vector == f.c.h_vtor + 9u);
        CHECK(f.c.flash.erases == (role == 1u || role == 2u || role == 5u ? 1u : 0u));
        CHECK(f.c.flash.writes == f.c.flash.erases * 42u);
        CHECK(f.launch_out.decision.snapshot.record.sequence == 100u + f.c.flash.erases);
        saved = f.launch_out;
        memset(&f.images, 0, sizeof f.images);
        memset(f.c.payload, 0, sizeof f.c.payload);
        memset(f.c.flash.bytes, 0, sizeof f.c.flash.bytes);
        CHECK(memcmp(&saved, &f.launch_out, sizeof saved) == 0);
        duplicate(&f);
    }
}
static void launch_handoff_faults(void)
{
    for (unsigned slot = 0u; slot < 2u; ++slot)
    for (unsigned page = 0u; page < 2u; ++page)
    for (unsigned cut = 1u; cut <= 12u; ++cut)
    for (unsigned after = 0u; after < 2u; ++after)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        boot_store_snapshot_t authority;
        prepare(&f, slot, page, 1u, 0u); enable_launch(&f);
        f.c.h_cut = cut; f.c.h_after = after;
        r = run(&f.c);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_HANDOFF_FAILED && r.handoff == BOOT_HANDOFF_IO);
        CHECK(r.runtime.status == BOOT_RUNTIME_OK && r.valid == 15u);
        CHECK(f.c.h_calls == cut && f.c.h_effects == cut - 1u + after);
        CHECK(f.c.terminal_recorded == (cut == 12u ? after : 0u));
        CHECK(f.w.owner == (cut == 12u ? BOOT_WATCHDOG_APPLICATION : BOOT_WATCHDOG_BOOT));
        CHECK(f.w.epoch == (cut == 12u ? 4u : 3u) && f.w.verification == 1u);
        CHECK(f.w.blocked == 1u && f.w.evidence == 0u);
        CHECK(f.handoff.blocked == 1u && f.handoff.finished == 0u);
        CHECK(f.c.feeds == 2u && f.c.calls == 18u && f.c.reg_calls == 3u);
        CHECK(f.c.flash.erases == 1u && f.c.flash.writes == 42u);
        CHECK(memcmp(&f.launch_out, &f.c.launch_sentinel, sizeof f.launch_out) == 0);
        /* The durable TRIAL remains authoritative. Handoff failure cannot undo it. */
        CHECK(boot_store_load(&f.c.inner, &authority) == BOOT_STORE_OK);
        CHECK(authority.record.sequence == 101u && authority.record.images[1u-slot].accounting == 2u);
        duplicate(&f);
        {
            boot_handoff_request_t request = {0};
            boot_handoff_intent_t intent;
            CHECK(boot_handoff_run(&f.handoff, &f.w, &request, &intent) == BOOT_HANDOFF_BLOCKED);
            CHECK(f.c.h_calls == cut);
        }
    }
}
static void launch_stops(void)
{
    for (unsigned error = 0u; error < 9u; ++error)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u); enable_launch(&f);
        if (error < 3u) f.c.reg_cut = error + 1u;
        if (error == 3u) f.c.short_read = 1u;
        if (error == 4u) f.c.cut = 6u;
        if (error == 5u) f.c.payload[0][20] ^= 1u;
        if (error == 6u) f.w.verification = UINT32_MAX;
        if (error == 7u) f.w.epoch = UINT32_MAX - 2u;
        if (error == 8u) memset(f.c.flash.bytes, 0xff, sizeof f.c.flash.bytes);
        r = run(&f.c);
        CHECK(r.status == (error == 7u ? BOOT_RUNTIME_WATCHDOG_HANDOFF_FAILED :
            BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED));
        if (error == 7u)
        {
            CHECK(r.handoff == BOOT_HANDOFF_ARGUMENT && r.runtime.status == BOOT_RUNTIME_OK);
            CHECK(f.w.epoch == UINT32_MAX && f.c.feeds == 2u);
        }
        if (error == 4u || error == 6u)
            CHECK(r.runtime.selection.verify_result[0] == BOOT_VERIFY_ERROR_ABORTED);
        CHECK(f.c.h_calls == 0u && f.w.owner == BOOT_WATCHDOG_BOOT);
        CHECK(f.w.blocked == 1u && f.handoff.blocked == 1u && f.w.evidence == 0u);
        CHECK(f.c.flash.erases == 0u && f.c.flash.writes == 0u);
        CHECK(memcmp(&f.launch_out, &f.c.launch_sentinel, sizeof f.launch_out) == 0);
        duplicate(&f);
    }
    for (unsigned kind = 1u; kind <= 3u; ++kind)
    {
        static fixture_t baseline;
        size_t commit;
        prepare(&baseline, 0u, 0u, kind, 0u); enable_launch(&baseline);
        if (kind == 2u)
        {
            baseline.c.payload[0][20] ^= 1u;
            baseline.c.h_vtor = BOOT_SLOT_B_BASE;
        }
        if (kind == 3u) baseline.c.captured = 8u;
        CHECK(run(&baseline.c).status == BOOT_RUNTIME_WATCHDOG_OK);
        commit = baseline.c.commit_call;
        for (unsigned after = 0u; after < 2u; ++after)
        for (size_t offset = 0u; offset < 4u; ++offset)
        {
            static fixture_t f;
            boot_runtime_watchdog_result_t r;
            boot_store_snapshot_t authority;
            prepare(&f, 0u, 0u, kind, 0u); enable_launch(&f);
            if (kind == 2u) f.c.payload[0][20] ^= 1u;
            if (kind == 3u) f.c.captured = 8u;
            f.c.flash.cut_call = commit + offset; f.c.flash.fail_after = (int)after;
            r = run(&f.c);
            CHECK(r.status == BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED);
            CHECK(r.runtime.status == BOOT_RUNTIME_RECONCILE && f.c.h_calls == 0u);
            CHECK(f.c.flash.calls == commit + offset);
            CHECK(f.w.blocked == 1u && f.handoff.blocked == 1u && f.w.evidence == 0u);
            CHECK(memcmp(&f.launch_out, &f.c.launch_sentinel, sizeof f.launch_out) == 0);
            duplicate(&f);
            f.c.flash.stopped = 0; f.c.flash.cut_call = SIZE_MAX;
            CHECK(boot_store_load(&f.c.inner, &authority) == BOOT_STORE_OK);
            CHECK(authority.record.sequence == 100u + (offset != 0u || after ? 1u : 0u));
            duplicate(&f);
        }
    }
}
static void launch_preflight(void)
{
    for (unsigned bad = 0u; bad < 11u; ++bad)
    {
        static fixture_t f;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u); enable_launch(&f);
        if (bad == 2u) f.handoff.io.operation = NULL;
        if (bad == 3u) f.handoff.io.terminal_intent = NULL;
        if (bad == 4u) f.handoff.blocked = 1u;
        if (bad == 5u) f.handoff.finished = 1u;
        if (bad == 6u) atomic_flag_test_and_set(&f.handoff.entered);
        if (bad == 7u) f.observed.ipsr = 1u;
        if (bad == 8u) f.observed.control = 1u;
        if (bad == 9u) f.observed.basepri = 1u;
        if (bad == 10u) f.observed.primask = 2u;
        r = boot_runtime_watchdog_launch(&f.bridge, &f.w,
            bad == 0u ? NULL : &f.handoff, bad == 1u ? NULL : &f.observed,
            &f.registers, &f.store, &f.images, payload_read, &f.c, &f.launch_out);
        CHECK(r.status == (bad < 2u ? BOOT_RUNTIME_WATCHDOG_ARGUMENT : BOOT_RUNTIME_WATCHDOG_REJECTED));
        CHECK(f.bridge.consumed == 0u && f.bridge.runtime.consumed == 0u);
        CHECK(f.c.calls == 0u && f.c.reg_calls == 0u && f.c.flash.calls == 0u && f.c.h_calls == 0u);
        CHECK(f.w.blocked == 0u && f.w.owner == BOOT_WATCHDOG_BOOT);
        CHECK(memcmp(&f.launch_out, &f.c.launch_sentinel, sizeof f.launch_out) == 0);
        f.handoff.io.operation = handoff_operation; f.handoff.io.terminal_intent = terminal_intent;
        f.handoff.blocked = 0u; f.handoff.finished = 0u;
        if (bad == 6u) atomic_flag_clear(&f.handoff.entered);
        f.observed = (boot_handoff_context_t){0};
        CHECK(run(&f.c).status == BOOT_RUNTIME_WATCHDOG_OK);
        duplicate(&f);
    }
}
static void launch_aliases(void)
{
    for (unsigned alias = 0u; alias < 9u; ++alias)
    {
        static fixture_t f;
        union {
            boot_runtime_watchdog_t bridge;
            boot_watchdog_t watchdog;
            boot_handoff_t handoff;
            boot_reset_register_adapter_t registers;
            boot_store_adapter_t store;
            boot_select_images_t images;
            boot_handoff_context_t observed;
            boot_runtime_watchdog_launch_t out;
        } overlap;
        boot_runtime_watchdog_t *bridge;
        boot_watchdog_t *watchdog;
        boot_handoff_t *h;
        const boot_reset_register_adapter_t *registers;
        const boot_store_adapter_t *store;
        const boot_select_images_t *images;
        const boot_handoff_context_t *observed;
        boot_runtime_watchdog_launch_t *out;
        boot_runtime_watchdog_result_t r;
        prepare(&f, 0u, 0u, 0u, 0u); enable_launch(&f);
        memset(&overlap, 0, sizeof overlap);
        bridge = &f.bridge; watchdog = &f.w; h = &overlap.handoff;
        registers = &f.registers; store = &f.store; images = &f.images;
        observed = &f.observed; out = &f.launch_out;
        if (alias == 0u || alias == 8u)
        {
            boot_runtime_watchdog_init(&overlap.bridge);
            bridge = &overlap.bridge;
        }
        if (alias == 1u) watchdog = &overlap.watchdog;
        if (alias == 2u) out = &overlap.out;
        if (alias == 3u) { overlap.registers = f.registers; registers = &overlap.registers; }
        if (alias == 4u) { overlap.store = f.store; store = &overlap.store; }
        if (alias == 5u) { overlap.images = f.images; images = &overlap.images; }
        if (alias == 6u || alias == 7u) observed = &overlap.observed;
        if (alias == 7u || alias == 8u) { h = &f.handoff; out = &overlap.out; }
        r = boot_runtime_watchdog_launch(bridge, watchdog, h, observed,
            registers, store, images, payload_read, &f.c, out);
        CHECK(r.status == BOOT_RUNTIME_WATCHDOG_ARGUMENT && r.valid == 0u);
        CHECK(bridge->consumed == 0u && bridge->runtime.consumed == 0u);
        CHECK(f.c.calls == 0u && f.c.reg_calls == 0u && f.c.flash.calls == 0u && f.c.h_calls == 0u);
        CHECK(memcmp(&f.launch_out, &f.c.launch_sentinel, sizeof f.launch_out) == 0);
        f.c.bridge = bridge;
        CHECK(run(&f.c).status == BOOT_RUNTIME_WATCHDOG_OK);
        duplicate(&f);
    }
}
#include "startup_seam.h"
int main(void)
{
    startup_return_seam();
    launch_aliases(); launch_preflight(); launch_success(); launch_handoff_faults(); launch_stops();
    preflight(); structural(); single(); multistream(); failures(); wdt_faults();
    ready_auth_failure(); reset_and_uncertain(); snapshots_and_aliases();
#ifdef BOOT_VERIFY_FAULT_INJECTION
    CHECK(live_allocations == 0u);
#endif
    printf("runtime watchdog: %u checks\n", checks);
    return 0;
}
