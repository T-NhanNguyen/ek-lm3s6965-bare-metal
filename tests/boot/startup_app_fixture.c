#include "startup_fixture.h"
#include "lm3s6965/boot_snapshot.h"
#include <string.h>
#if defined(FOREGROUND_CONTINUITY_TEST)
#include "foreground_continuity_fixture.h"
#endif

/* Real flash LMA/data/BSS/constructor: boot poisons all destination words. */
volatile uint32_t initialized_data = 0x1234abcdu;
volatile uint32_t zero_bss[64];
volatile uint32_t constructed;
volatile uint32_t svc_slot;
void SVCall_Handler(void);
void SVCall_Handler(void) { svc_slot = SLOT_INDEX + 1u; }
/* Failure oracle only, before C init: no ordinary globals used as operational
 * state. Destination sentinels are read solely to prove initialization did not
 * run. The target stop implementation is separately compiled under an internal
 * symbol in this fixture; production has no override/readiness hook. */
_Noreturn void boot_startup_stop(void)
{
    boot_startup_t *c = &boot_startup_continuity;
    startup_model_t *m = startup_model(c);
    boot_verified_image_t running;
    CHECK(c->state == BOOT_STARTUP_FAILED && m->fail_clock_on_read == 3);
    CHECK(m->rcc2 == 1 && m->writes == m->dispatch_writes);
    CHECK(m->feeds == m->dispatch_feeds);
    CHECK(initialized_data == 0xa5a5a5a5u && zero_bss[0] == 0xa5a5a5a5u && constructed == 0xa5a5a5a5u);
    CHECK(c->watchdog.epoch == c->entry_epoch && diagnostic_digest(c->publication.words) == m->digest);
    CHECK(boot_startup_running(c, &running) != 0);
    semi(4, "STARTUP CLOCK REJECT PASS\n");
    finish(0);
}
static void constructor(void) __attribute__((constructor));
static void constructor(void)
{
    boot_startup_t *c = &boot_startup_continuity;
    CHECK(initialized_data == 0x1234abcdu && zero_bss[0] == 0 && zero_bss[63] == 0);
    CHECK(c->state == BOOT_STARTUP_ENTERED);
    startup_model_t *m = startup_model(c);
    CHECK(m->writes == m->dispatch_writes && m->feeds == m->dispatch_feeds);
#if defined(FOREGROUND_CONTINUITY_TEST)
    CHECK(m->us == c->entry_us && m->clock_reads == 3);
#endif
    m->us += 300;
    constructed = 0xfeedbeefu;
}
static int metadata_read(void *context, uint32_t address, uint8_t *out, size_t n)
{
    (void)context;
    memcpy(out, (const void *)(uintptr_t)address, n);
    return 0;
}
int main(void);
/* Three modeled bounded foreground functions; no client/no timer is explicit. */
static int poll_ethernet(void) { return 1; }
static int due_timers(void) { return 1; }
static int ftp_idle(void) { return 1; }
int main(void)
{
    boot_startup_t *c = &boot_startup_continuity;
    startup_model_t *m = startup_model(c);
    boot_verified_image_t running;
    boot_diag_t diag;
    boot_diag_context_t context;
    CHECK(c->base == SLOT_BASE && c->image.manifest.slot == SLOT_INDEX);
    CHECK((uintptr_t)&c->watchdog == 0x2000dc00u);
    CHECK(c->watchdog.io.context == c->context && c->clock.context == c->context);
    CHECK((uintptr_t)c->watchdog.io.read_register < 0x8000u);
    CHECK((uintptr_t)c->watchdog.io.write_register < 0x8000u);
    CHECK((uintptr_t)c->clock.read < 0x8000u);
    CHECK(c->observed.msp == 0x20010000u && c->observed.control == 0);
    CHECK(initialized_data == 0x1234abcdu && constructed == 0xfeedbeefu);
    for (unsigned i = 0; i < 64; ++i) CHECK(zero_bss[i] == 0);
    CHECK(c->watchdog.epoch == c->entry_epoch && c->watchdog.owner == BOOT_WATCHDOG_APPLICATION);
    CHECK(c->watchdog.verification == 1 && c->watchdog.completed == c->image.manifest.payload_length);
    CHECK(c->watchdog.total == c->image.manifest.payload_length && c->watchdog.reload == STARTUP_LOAD);
    CHECK(c->watchdog.phase == BOOT_VERIFY_PAYLOAD_PROGRESS);
    CHECK(diagnostic_digest(c->publication.words) == m->digest);
    CHECK(c->publication.associated && !c->publication.busy);
    CHECK(boot_startup_running(c, &running) == 0);
    CHECK(running.reset_vector == c->reset_vector);
    /* Constructor time and bounded app init are included in conserved origin. */
    m->us += 200;
    CHECK(m->us - c->dispatch_us >= 500 && c->entry_us >= c->dispatch_us);
    /* No configure or feed writes across entry, C init and app init. */
    CHECK(m->writes == m->dispatch_writes && m->feeds == m->dispatch_feeds);
#if defined(FOREGROUND_CONTINUITY_TEST)
    CHECK(m->us == c->entry_us + 500 && m->clock_reads == 3);
#endif
    CHECK(boot_startup_c_ready(c));
#if defined(FOREGROUND_CONTINUITY_TEST)
    CHECK(m->us == c->entry_us + 600 && m->clock_reads == 4);
#endif
    CHECK(m->writes == m->dispatch_writes && m->feeds == m->dispatch_feeds);
    CHECK(boot_diag_publication_snapshot(&c->publication, &diag, &context) == BOOT_DIAG_PUBLICATION_OK);
    CHECK(diag.health == 1 && diag.stage == BOOT_DIAG_STAGE_STARTUP);
    #if defined(FOREGROUND_CONTINUITY_TEST)
    foreground_clock_transition();
#endif
    __asm__ volatile("cpsie i\n svc 0" ::: "memory");
#if defined(FOREGROUND_CONTINUITY_TEST)
    /* Bound later fixture assertions: no dependence on host/QEMU cadence. */
    __asm__ volatile("cpsid i" ::: "memory");
#endif
    CHECK(svc_slot == SLOT_INDEX + 1u);
    /* Baselines were captured by boot, not after a possible startup feed. */
    CHECK(m->writes == m->dispatch_writes && m->feeds == m->dispatch_feeds);
    { uint32_t epoch = c->watchdog.epoch, feeds = m->dispatch_feeds;
      uint32_t writes = m->dispatch_writes;
      CHECK(c->watchdog.evidence == 0);
      CHECK(poll_ethernet());
      CHECK(boot_watchdog_application_report(&c->watchdog, epoch, BOOT_WATCHDOG_ETHERNET, 1) == BOOT_WATCHDOG_OK);
      CHECK(boot_watchdog_service(&c->watchdog, BOOT_WATCHDOG_APPLICATION, epoch) == BOOT_WATCHDOG_PROGRESS);
      CHECK(m->feeds == feeds && m->writes == writes);
      CHECK(c->watchdog.epoch == epoch && c->watchdog.evidence == BOOT_WATCHDOG_ETHERNET);
      CHECK(due_timers());
      CHECK(boot_watchdog_application_report(&c->watchdog, epoch, BOOT_WATCHDOG_TIMERS, 1) == BOOT_WATCHDOG_OK);
      CHECK(boot_watchdog_service(&c->watchdog, BOOT_WATCHDOG_APPLICATION, epoch) == BOOT_WATCHDOG_PROGRESS);
      CHECK(m->feeds == feeds && m->writes == writes);
      CHECK(c->watchdog.epoch == epoch && c->watchdog.evidence == (BOOT_WATCHDOG_ETHERNET | BOOT_WATCHDOG_TIMERS));
      CHECK(ftp_idle());
      CHECK(boot_watchdog_application_report(&c->watchdog, epoch, BOOT_WATCHDOG_FTP, 1) == BOOT_WATCHDOG_OK);
      CHECK(m->feeds == feeds && m->writes == writes);
      CHECK(c->watchdog.evidence == (BOOT_WATCHDOG_ETHERNET | BOOT_WATCHDOG_TIMERS | BOOT_WATCHDOG_FTP));
      CHECK(boot_watchdog_service(&c->watchdog, BOOT_WATCHDOG_APPLICATION, epoch) == BOOT_WATCHDOG_OK);
      CHECK(m->feeds == feeds + 1 && m->writes == writes + 3);
      CHECK(c->watchdog.epoch == epoch + 1 && c->entry_epoch == epoch && c->watchdog.evidence == 0);
      CHECK(boot_startup_running(c, &running) == 0);
      { boot_snapshot_t snapshot;
        boot_snapshot_sources_t sources = {{NULL, metadata_read, NULL, NULL}, c, boot_startup_running};
        CHECK(boot_snapshot_capture(&c->publication, &sources, &snapshot) == BOOT_SNAPSHOT_OK);
        CHECK(snapshot.running.manifest.slot == SLOT_INDEX && snapshot.diagnostic.health == 1);
        CHECK(snapshot.running.reset_vector == c->reset_vector);
      }
#if defined(FOREGROUND_CONTINUITY_TEST)
      foreground_continuity_turns();
#endif
      CHECK(boot_watchdog_transfer(&c->watchdog, c->watchdog.epoch) == BOOT_WATCHDOG_STATE);
      CHECK(boot_watchdog_application_report(&c->watchdog, epoch, BOOT_WATCHDOG_FTP, 1) == BOOT_WATCHDOG_STATE);
    }
    /* Origin only: deliberately NOT an enabled deadline or reset actuator. */
    m->us = c->dispatch_us + 59999999;
    CHECK(m->us - c->dispatch_us < 60000000);
    m->us++;
    CHECK(m->us - c->dispatch_us == 60000000);
    semi(4, "STARTUP PASS\n");
    finish(0);
}
