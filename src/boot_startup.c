#include "lm3s6965/boot_startup.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lm3s6965/boot_diag.h"
#include "lm3s6965/boot_diag_publication.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_handoff.h"
#include "lm3s6965/boot_runtime_watchdog.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"

static bool overlap(const void *a, size_t n, const void *b, size_t m)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < n : x - y < m;
}
static void state(boot_startup_t *c, boot_startup_state_t s)
{
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("dmb" ::: "memory");
#endif
    c->state = (uint32_t)s;
}
static bool fail(boot_startup_t *c)
{
    if (c != NULL) { c->active = 0; state(c, BOOT_STARTUP_FAILED); }
    return false;
}
static bool abi(const boot_startup_t *c)
{
    return c != NULL && c->magic == BOOT_STARTUP_MAGIC && c->version == 1 &&
        c->bytes == BOOT_STARTUP_BYTES && c->abi == BOOT_STARTUP_ABI;
}
static bool claim(boot_startup_t *c, boot_startup_state_t expected)
{
    if (!abi(c) || c->active || c->state != (uint32_t)expected) return fail(c);
    c->active = 1;
    return true;
}
static bool complete(boot_startup_t *c, boot_startup_state_t before,
                     boot_startup_state_t after)
{
    if (!c->active || c->state != (uint32_t)before) return fail(c);
    c->active = 0;
    state(c, after);
    return true;
}
static bool boot_code(uintptr_t p)
{
#if defined(__arm__) || defined(__thumb__)
    return (p & 1u) != 0 && p < 0x8000u;
#else
    return p != 0; /* HOST cannot establish target code placement. */
#endif
}
static bool ownership(const boot_startup_t *c)
{
    uintptr_t start = (uintptr_t)c->context;
    return boot_code((uintptr_t)c->watchdog.io.read_register) &&
        boot_code((uintptr_t)c->watchdog.io.write_register) &&
        boot_code((uintptr_t)c->clock.read) &&
        (uintptr_t)c->watchdog.io.context == start &&
        (uintptr_t)c->clock.context == start &&
        c->publication.word == NULL && c->publication.barrier == NULL &&
        c->publication.user == NULL && c->publication.associated && !c->publication.busy
#if defined(__arm__) || defined(__thumb__)
        && (uintptr_t)c == 0x2000dbc0u &&
        (uintptr_t)c->publication.words == 0x2000dfc0u
#endif
        ;
}
static bool clock_read(boot_startup_t *c, uint64_t *us)
{
    boot_startup_sample_t sample;
    if (!ownership(c) || c->clock.hz == 0 || c->clock.hz > UINT32_MAX / 5u ||
        c->clock.rcc_mask == 0 || c->clock.rcc2_mask == 0 || c->clock.source == 0 ||
        c->hz != c->clock.hz || c->reload != c->hz * 5u ||
        c->watchdog.reload != c->reload ||
        c->clock.read(c->clock.context, &sample) != 0 ||
        sample.source != c->clock.source || sample.hz != c->hz ||
        ((sample.rcc ^ c->clock.rcc) & c->clock.rcc_mask) != 0 ||
        ((sample.rcc2 ^ c->clock.rcc2) & c->clock.rcc2_mask) != 0)
        return false;
    *us = sample.us;
    return true;
}
static bool controller(const boot_startup_t *c, bool entry)
{
    return c->watchdog.configured && !c->watchdog.blocked &&
        c->watchdog.owner == BOOT_WATCHDOG_APPLICATION &&
        (!entry || (c->watchdog.epoch == c->entry_epoch && !c->watchdog.evidence));
}
static bool excluded(void)
{
#if defined(__arm__) || defined(__thumb__)
    uint32_t mask, ipsr;
    __asm__ volatile("mrs %0, primask\n mrs %1, ipsr" : "=r"(mask), "=r"(ipsr));
    return mask == 1 && ipsr == 0;
#else
    return true;
#endif
}
bool boot_startup_reset(boot_startup_t *c, volatile uint32_t *words,
    uint32_t flags, const boot_watchdog_io_t *io, const boot_startup_clock_t *clock)
{
    if (c == NULL || words == NULL || io == NULL || clock == NULL ||
        overlap(c, sizeof(*c), io, sizeof(*io)) ||
        overlap(c, sizeof(*c), clock, sizeof(*clock)) ||
        overlap(c, sizeof(*c), (const void *)words, 64)) return false;
    c->magic = 0; /* genuine reset invalidates before touching native objects */
    memset(c, 0, sizeof(*c));
    c->publication.words = words;
    if (boot_diag_publication_boot(&c->publication, flags) != BOOT_DIAG_PUBLICATION_OK ||
        boot_watchdog_init(&c->watchdog, io) != BOOT_WATCHDOG_OK) return fail(c);
    c->clock = *clock;
    c->magic = BOOT_STARTUP_MAGIC; c->version = 1;
    c->bytes = BOOT_STARTUP_BYTES; c->abi = BOOT_STARTUP_ABI;
    return ownership(c) || fail(c);
}
static bool prepare(boot_startup_t *c, const boot_verified_image_t *image,
                    const boot_handoff_intent_t *intent)
{
    if (!claim(c, BOOT_STARTUP_EMPTY)) return false;
    if (image == NULL || intent == NULL ||
        overlap(c, sizeof(*c), image, sizeof(*image)) ||
        overlap(c, sizeof(*c), intent, sizeof(*intent)) ||
        c->state != BOOT_STARTUP_EMPTY || !excluded() || !ownership(c) ||
        !controller(c, false) || c->watchdog.evidence ||
        image->manifest.slot > 1u ||
        image->manifest.execution_base != (image->manifest.slot == 0u ? 0x8800u : 0x24000u) ||
        image->manifest.payload_length < 236u || image->manifest.payload_length > 111616u ||
        (image->manifest.payload_length & 3u) != 0 ||
        (image->reset_vector & ~1u) < image->manifest.execution_base ||
        (image->reset_vector & ~1u) - image->manifest.execution_base >= image->manifest.payload_length ||
        intent->vtor != image->manifest.execution_base ||
        intent->msp != 0x20010000u || intent->msp != image->initial_msp ||
        intent->reset_vector != image->reset_vector || !(intent->reset_vector & 1u) ||
        intent->application_epoch != c->watchdog.epoch ||
        intent->control || intent->primask || intent->basepri || intent->faultmask)
        return fail(c);
    c->image = *image; c->intent = *intent;
    c->base = intent->vtor; c->reset_vector = intent->reset_vector;
    c->entry_epoch = intent->application_epoch;
    c->hz = c->clock.hz; c->reload = c->watchdog.reload;
    { uint64_t us; if (!clock_read(c, &us)) return fail(c); }
    return complete(c, BOOT_STARTUP_EMPTY, BOOT_STARTUP_PREPARED);
}
bool boot_startup_prepare(boot_startup_t *c, const boot_runtime_watchdog_result_t *r,
                          const boot_runtime_watchdog_launch_t *launch)
{
    if (r == NULL || launch == NULL || c == NULL ||
        overlap(c, sizeof(*c), r, sizeof(*r)) ||
        overlap(c, sizeof(*c), launch, sizeof(*launch)) ||
        r->status != BOOT_RUNTIME_WATCHDOG_OK ||
        !(r->valid & BOOT_RUNTIME_WATCHDOG_HANDOFF) || r->handoff != BOOT_HANDOFF_OK)
        return fail(c);
    return prepare(c, &launch->decision.image, &launch->intent);
}
bool boot_startup_prepare_handoff(boot_startup_t *c, const boot_handoff_t *h,
    boot_handoff_result_t r, const boot_verified_image_t *image,
    const boot_handoff_intent_t *intent)
{
    if (c == NULL || h == NULL || overlap(c, sizeof(*c), h, sizeof(*h)) ||
        r != BOOT_HANDOFF_OK || !h->finished || h->blocked) return fail(c);
    return prepare(c, image, intent);
}
bool boot_startup_begin_dispatch(boot_startup_t *c)
{
    uint64_t us;
    if (!claim(c, BOOT_STARTUP_PREPARED)) return false;
    if (!excluded() ||
        !controller(c, true) || !clock_read(c, &us)) return fail(c);
    c->dispatch_us = us;
    return complete(c, BOOT_STARTUP_PREPARED, BOOT_STARTUP_DISPATCHING);
}
static bool in_image(const boot_startup_t *c, uint32_t pc)
{
    return pc >= c->base && pc - c->base < c->image.manifest.payload_length;
}
bool boot_startup_early(boot_startup_t *c, uint32_t stub, uint32_t witness)
{
    uint64_t us;
    if (!claim(c, BOOT_STARTUP_DISPATCHING)) return false;
    if (!excluded() ||
        !controller(c, true) || c->reset_vector != stub ||
        c->image.reset_vector != stub || c->image.manifest.execution_base != c->base ||
        c->observed.stub != stub || c->observed.pc != witness ||
        !in_image(c, stub & ~1u) || !in_image(c, witness) ||
        c->observed.vtor != c->base || c->observed.msp != 0x20010000u ||
        c->observed.control || c->observed.ipsr || c->observed.primask ||
        c->observed.basepri || c->observed.faultmask ||
        !clock_read(c, &us) || us < c->dispatch_us) return fail(c);
    c->entry_us = us;
    return complete(c, BOOT_STARTUP_DISPATCHING, BOOT_STARTUP_ENTERED);
}
bool boot_startup_c_ready(boot_startup_t *c)
{
    if (!claim(c, BOOT_STARTUP_ENTERED)) return false;
    /* Validate continuity before masked publication, not the clock switch.
     * The separate final sample belongs AFTER application SysTick starts. */
    uint64_t us;
    if (!excluded() || !controller(c, false) ||
        !clock_read(c, &us) || us < c->entry_us ||
        c->state != BOOT_STARTUP_ENTERED || !c->active ||
        boot_diag_publication_progress(&c->publication, BOOT_DIAG_STAGE_STARTUP,
                                      BOOT_DIAG_HEALTH_APP) != BOOT_DIAG_PUBLICATION_OK)
        return fail(c);
    c->observed.initialized = 1;
    return complete(c, BOOT_STARTUP_ENTERED, BOOT_STARTUP_C_READY);
}
bool boot_startup_time(boot_startup_t *c, uint64_t *us)
{
    uint64_t sample;
    if (!abi(c) || us == NULL ||
        overlap(c, sizeof(*c), us, sizeof(*us)) ||
        overlap((const void *)c->publication.words, 64, us, sizeof(*us)) ||
        c->active ||
        (c->state != BOOT_STARTUP_ENTERED && c->state != BOOT_STARTUP_C_READY))
        return fail(c);
    const uint32_t before = c->state;
    c->active = 1;
    if (!controller(c, false) || !clock_read(c, &sample) ||
        sample < c->entry_us || !c->active || c->state != before)
        return fail(c);
    c->active = 0;
    *us = sample;
    return true;
}
bool boot_startup_running_at(boot_startup_t *c, uint32_t pc, uint32_t vtor,
                              boot_verified_image_t *out)
{
    if (!abi(c) || out == NULL || overlap(c, sizeof(*c), out, sizeof(*out)) ||
        (c->state != BOOT_STARTUP_ENTERED && c->state != BOOT_STARTUP_C_READY) ||
        !controller(c, false) || !in_image(c, pc & ~1u) || vtor != c->base ||
        overlap((const void *)c->publication.words, 64, out, sizeof(*out)))
        return false;
    if (c->active) return false;
    *out = c->image;
    return true;
}
