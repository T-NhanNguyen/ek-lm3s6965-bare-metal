/* HOST setup only. Real validation/WDT APIs; NOT target provenance or a
 * substitute for genuine boot verification/configuration. One case/process. */
#ifndef FOREGROUND_CONTINUITY_H
#define FOREGROUND_CONTINUITY_H
#include "check.h"
#include "lm3s6965/boot_startup.h"
#include <string.h>
boot_startup_t boot_startup_continuity;
typedef struct __attribute__((may_alias)) {
    uint64_t us;
    uint32_t lock, reads, writes, feeds, fail_after_feed;
} owner_model_t;
static uint32_t diagnostics[16];
static owner_model_t *model(void)
{ return (owner_model_t *)boot_startup_continuity.context; }
static int model_read(void *p, uint32_t a, uint32_t *v)
{
    owner_model_t *m = p; ++m->reads;
    switch (a) {
    case BOOT_WATCHDOG_RCGC0: *v = 8; break;
    case BOOT_WATCHDOG_CTL: *v = 3; break;
    case BOOT_WATCHDOG_LOAD: *v = 500; break;
    case BOOT_WATCHDOG_LOCK: *v = m->lock; break;
    case BOOT_WATCHDOG_RIS:
        if (m->fail_after_feed && m->feeds) return -1;
        *v = 0; break;
    default: CHECK(0);
    }
    return 0;
}
static int model_write(void *p, uint32_t a, uint32_t v)
{
    owner_model_t *m = p; ++m->writes;
    /* A turn must NEVER reconfigure CTL/LOAD or cold-start clock hardware. */
    CHECK(a == BOOT_WATCHDOG_LOCK || a == BOOT_WATCHDOG_ICR);
    if (a == BOOT_WATCHDOG_LOCK) m->lock = v == 0 ? 1u : 0u;
    else { CHECK(v == 0); ++m->feeds; }
    return 0;
}
static int sample(void *p, boot_startup_sample_t *s)
{
    owner_model_t *m = p;
    *s = (boot_startup_sample_t){m->us, 0, 0, 100, 1};
    return 0;
}
/* HOST wrapper supplies explicit in-image PC/VTOR, not native executable PC. */
int boot_startup_running(void *p, boot_verified_image_t *out)
{ return boot_startup_running_at(p, 0x8980, 0x8800, out) ? 0 : -1; }
static void continuity_ready(uint64_t us)
{
    boot_startup_t *c = &boot_startup_continuity;
    boot_watchdog_io_t io = {c->context, model_read, model_write};
    boot_startup_clock_t clock = {sample, c->context, 0, 0,
                                 UINT32_MAX, UINT32_MAX, 100, 1};
    boot_runtime_watchdog_launch_t launch = {0};
    boot_runtime_watchdog_result_t result = {
        .status = BOOT_RUNTIME_WATCHDOG_OK,
        .valid = BOOT_RUNTIME_WATCHDOG_HANDOFF, .handoff = BOOT_HANDOFF_OK};
    CHECK(boot_startup_reset(c, diagnostics, 0, &io, &clock));
    model()->us = us; model()->lock = 1;
    /* Scoped HOST admission fixture, matching test_startup.c. */
    c->watchdog.configured = 1; c->watchdog.owner = BOOT_WATCHDOG_APPLICATION;
    c->watchdog.epoch = 9; c->watchdog.reload = 500;
    c->watchdog.verification = 7; c->watchdog.total = c->watchdog.completed = 4096;
    c->watchdog.phase = BOOT_VERIFY_PAYLOAD_PROGRESS;
    launch.decision.image.manifest.execution_base = 0x8800;
    launch.decision.image.manifest.payload_length = 4096;
    launch.decision.image.initial_msp = 0x20010000;
    launch.decision.image.reset_vector = 0x8901;
    launch.intent = (boot_handoff_intent_t){0x8800,0x20010000,0x8901,0,0,0,0,9};
    CHECK(boot_startup_prepare(c, &result, &launch));
    CHECK(boot_startup_begin_dispatch(c));
    c->observed = (boot_startup_observations_t){.msp=0x20010000,
        .vtor=0x8800, .pc=0x8920, .stub=0x8901};
    CHECK(boot_startup_early(c, 0x8901, 0x8920));
    CHECK(boot_startup_c_ready(c));
    CHECK(model()->writes == 0 && model()->feeds == 0);
}
#endif
