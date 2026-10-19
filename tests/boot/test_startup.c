#include "lm3s6965/boot_startup.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct __attribute__((may_alias)) { uint64_t us; uint32_t rcc, rcc2; int error; } model_t;
static int read_reg(void *p, uint32_t a, uint32_t *v)
{ (void)p; (void)a; *v = 0; return 0; }
static int write_reg(void *p, uint32_t a, uint32_t v)
{ (void)p; (void)a; (void)v; return 0; }
static int clock_read(void *p, boot_startup_sample_t *s)
{
    model_t *m = p;
    *s = (boot_startup_sample_t){m->us, m->rcc, m->rcc2, 100, 1};
    if (m->error == 2) {
        boot_startup_t *c = (boot_startup_t *)((uint8_t *)p - offsetof(boot_startup_t, context));
        assert(!boot_startup_begin_dispatch(c));
        return 0;
    }
    return m->error;
}
static void fresh(boot_startup_t *c, uint32_t *words)
{
    boot_watchdog_io_t io = { c->context, read_reg, write_reg };
    boot_startup_clock_t clock = {clock_read, c->context, 0, 0, UINT32_MAX, UINT32_MAX, 100, 1};
    assert(boot_startup_reset(c, words, 0, &io, &clock));
    /* HOST state fixture only: real configure/handoff exercised by target. */
    c->watchdog.configured = 1; c->watchdog.owner = BOOT_WATCHDOG_APPLICATION;
    c->watchdog.epoch = 9; c->watchdog.reload = 500;
}
static void prepared(boot_startup_t *c, uint32_t *words)
{
    boot_runtime_watchdog_launch_t launch = {0};
    boot_runtime_watchdog_result_t r = {0};
    fresh(c, words);
    launch.decision.image.manifest.execution_base = 0x8800;
    launch.decision.image.manifest.payload_length = 4096;
    launch.decision.image.initial_msp = 0x20010000;
    launch.decision.image.reset_vector = 0x8901;
    launch.intent = (boot_handoff_intent_t){0x8800, 0x20010000, 0x8901, 0,0,0,0,9};
    r.status = BOOT_RUNTIME_WATCHDOG_OK;
    r.valid = BOOT_RUNTIME_WATCHDOG_HANDOFF; r.handoff = BOOT_HANDOFF_OK;
    assert(boot_startup_prepare(c, &r, &launch));
}
static void enter(boot_startup_t *c)
{
    c->observed = (boot_startup_observations_t){.msp=0x20010000,
        .vtor=0x8800, .pc=0x8920, .stub=0x8901};
}
int main(void)
{
    boot_startup_t c;
    uint32_t words[16];
    boot_verified_image_t out, sentinel;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    prepared(&c, words); out = sentinel;
    assert(!boot_startup_running_at(&c, 0x8980, 0x8800, &out));
    assert(memcmp(&out, &sentinel, sizeof(out)) == 0);
    enter(&c);
    assert(!boot_startup_early(&c, 0x8901, 0x8920)); /* PREPARED is not entry */
    assert(c.state == BOOT_STARTUP_FAILED);
    prepared(&c, words); assert(boot_startup_begin_dispatch(&c));
    assert(!boot_startup_begin_dispatch(&c)); assert(c.state == BOOT_STARTUP_FAILED);
    prepared(&c, words); assert(boot_startup_begin_dispatch(&c)); enter(&c);
    assert(boot_startup_early(&c, 0x8901, 0x8920));
    assert(boot_startup_c_ready(&c));
    assert(!boot_startup_running_at(&c, 0x8980, 0x8800, &c.image));
    c.watchdog.epoch++; /* evolving service epoch must not invalidate identity */
    assert(c.entry_epoch == 9);
    assert(boot_startup_running_at(&c, 0x8980, 0x8800, &out));
    assert(out.reset_vector == 0x8901);
    assert(!boot_startup_running_at(&c, 0x24080, 0x8800, &out));
    assert(!boot_startup_early(&c, 0x8901, 0x8920));
    assert(!boot_startup_running_at(&c, 0x8980, 0x8800, &out));
    for (unsigned fault = 0; fault < 11; ++fault) {
        prepared(&c, words); assert(boot_startup_begin_dispatch(&c)); enter(&c);
        switch (fault) {
        case 0: c.observed.pc++; break;
        case 1: c.observed.primask = 1; break;
        case 2: c.watchdog.epoch++; break;
        case 3: ((model_t *)c.context)->rcc = 1; break;
        case 4: ((model_t *)c.context)->rcc2 = 1; break;
        case 5: ((model_t *)c.context)->error = 1; break;
        case 6: c.watchdog.blocked = 1; break;
        case 7: c.hz++; break;
        case 8: c.clock.source++; break;
        case 9: c.watchdog.io.context = NULL; break;
        default: c.clock.read = NULL; break;
        }
        assert(!boot_startup_early(&c, 0x8901, 0x8920));
        assert(c.state == BOOT_STARTUP_FAILED);
        assert(!boot_startup_running_at(&c, 0x8980, 0x8800, &out));
    }
    prepared(&c, words);
    ((model_t *)c.context)->error = 2; /* reentry from clock callback consumes */
    assert(!boot_startup_begin_dispatch(&c));
    assert(c.state == BOOT_STARTUP_FAILED);
    prepared(&c, words);
    ((model_t *)c.context)->us = 20;
    assert(boot_startup_begin_dispatch(&c)); enter(&c);
    ((model_t *)c.context)->us = 19;
    assert(!boot_startup_early(&c, 0x8901, 0x8920));
    prepared(&c, words); assert(boot_startup_begin_dispatch(&c)); enter(&c);
    assert(boot_startup_early(&c, 0x8901, 0x8920));
    words[3] = 0; /* invalid publication cannot commit C_READY/health */
    assert(!boot_startup_c_ready(&c)); assert(c.state == BOOT_STARTUP_FAILED);
    assert(!boot_startup_running_at(&c, 0x8980, 0x8800, &out));
    prepared(&c, words);
    { boot_runtime_watchdog_result_t r = {.status=BOOT_RUNTIME_WATCHDOG_OK,
          .valid=BOOT_RUNTIME_WATCHDOG_HANDOFF, .handoff=BOOT_HANDOFF_OK};
      boot_runtime_watchdog_launch_t launch = {0};
      assert(!boot_startup_prepare(&c, &r, &launch)); /* duplicate prepare */
      assert(c.state == BOOT_STARTUP_FAILED); }
    prepared(&c, words); c.abi++;
    assert(!boot_startup_begin_dispatch(&c));
    /* Genuine reset destroys stale entry and stale health7; handoff does not. */
    fresh(&c, words);
    assert(boot_diag_publication_progress(&c.publication, BOOT_DIAG_STAGE_STARTUP, 7) == BOOT_DIAG_PUBLICATION_OK);
    c.state = BOOT_STARTUP_ENTERED;
    fresh(&c, words);
    { boot_diag_t diag; boot_diag_context_t context;
      assert(boot_diag_publication_snapshot(&c.publication, &diag, &context) == BOOT_DIAG_PUBLICATION_OK);
      assert(diag.health == 0); }
    assert(c.state == BOOT_STARTUP_EMPTY);
    puts("STARTUP HOST PASS");
    return 0;
}
