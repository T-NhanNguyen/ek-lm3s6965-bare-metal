/* Included after existing fixture helpers. Actual protected launch return,
 * independent retained controller/IO, no controller copy or accounting replay. */
#include "lm3s6965/boot_startup.h"
typedef struct __attribute__((may_alias)) { uint32_t load, ctl, lock, writes; } seam_io_t;
static int seam_read(void *p, uint32_t address, uint32_t *out)
{
    seam_io_t *m = p;
    switch (address) {
    case BOOT_WATCHDOG_RCGC0: *out = 8; break;
    case BOOT_WATCHDOG_LOAD: *out = m->load; break;
    case BOOT_WATCHDOG_CTL: *out = m->ctl; break;
    case BOOT_WATCHDOG_LOCK: *out = m->lock; break;
    case BOOT_WATCHDOG_RIS: *out = 0; break;
    default: return -1;
    }
    return 0;
}
static int seam_write(void *p, uint32_t address, uint32_t value)
{
    seam_io_t *m = p;
    ++m->writes;
    switch (address) {
    case BOOT_WATCHDOG_LOAD: m->load = value; break;
    case BOOT_WATCHDOG_CTL: m->ctl = value; break;
    case BOOT_WATCHDOG_LOCK: m->lock = value == 0x1acce551u ? 0u : 1u; break;
    case BOOT_WATCHDOG_ICR: break;
    default: return -1;
    }
    return 0;
}
static int seam_clock(void *p, boot_startup_sample_t *out)
{
    (void)p;
    *out = (boot_startup_sample_t){1000, 0, 0, 100, 1};
    return 0;
}
static void startup_return_seam(void)
{
    for (unsigned slot = 0; slot < 2; ++slot)
    for (unsigned failure = 0; failure < 2; ++failure) {
        fixture_t f;
        boot_startup_t retained;
        uint32_t diagnostic[16];
        boot_watchdog_io_t io = {retained.context, seam_read, seam_write};
        boot_startup_clock_t clock = {seam_clock, retained.context, 0,0,UINT32_MAX,UINT32_MAX,100,1};
        boot_runtime_watchdog_result_t result;
        boot_verified_image_t running;
        prepare(&f, slot, 0, 0, 0);
        enable_launch(&f);
        CHECK(boot_startup_reset(&retained, diagnostic, 0, &io, &clock));
        CHECK(boot_watchdog_configure(&retained.watchdog, 100, 500) == BOOT_WATCHDOG_OK);
        f.c.w = &retained.watchdog;
        if (failure) f.c.h_cut = 12; /* transferred, terminal failure != success */
        result = run(&f.c); /* callbacks assert frozen output and BUSY guards */
        { size_t flash = f.c.flash.calls;
          unsigned regs = f.c.reg_calls, operations = f.c.h_calls;
          uint32_t writes = ((seam_io_t *)retained.context)->writes;
          if (failure) {
              CHECK(result.status == BOOT_RUNTIME_WATCHDOG_HANDOFF_FAILED);
              CHECK(retained.watchdog.owner == BOOT_WATCHDOG_APPLICATION);
              CHECK(memcmp(&f.launch_out, &f.c.launch_sentinel, sizeof f.launch_out) == 0);
              CHECK(!boot_startup_prepare(&retained, &result, &f.launch_out));
              CHECK(retained.state == BOOT_STARTUP_FAILED);
          } else {
              CHECK(result.status == BOOT_RUNTIME_WATCHDOG_OK);
              CHECK(boot_startup_prepare(&retained, &result, &f.launch_out));
              CHECK(!boot_startup_running_at(&retained, retained.base + 100, retained.base, &running));
              CHECK(boot_startup_begin_dispatch(&retained));
              CHECK(!boot_startup_begin_dispatch(&retained));
              CHECK(retained.state == BOOT_STARTUP_FAILED);
          }
          CHECK(f.c.flash.calls == flash && f.c.reg_calls == regs && f.c.h_calls == operations);
          CHECK(((seam_io_t *)retained.context)->writes == writes);
        }
    }
}
