/* Only the signed foreground variant includes this fixture. The actual owner,
 * clock, startup and WDT run on Cortex-M3; services are explicit idle doubles. */
#include <stddef.h> /* NULL is used below; do not rely on includer order. */
/* net_clock.c supplies SysTick_Handler; startup.c has only a weak alias. */
#define FOREGROUND_STRING_(x) #x
#define FOREGROUND_STRING(x) FOREGROUND_STRING_(x)
#undef CHECK
#define CHECK(x) do { if (!(x)) { semi(4, "FOREGROUND_ASSERT " __FILE__ ":" \
    FOREGROUND_STRING(__LINE__) " " #x "\n"); finish(1); } } while (0)
#include "foreground_owner.h"
#include "net_clock.h"
#include "lm3s6965/boot_startup_gptm.h"
void SysTick_Handler(void);
#include "lm3s_netif.h"
#include "lm3s6965/ftp_tcp.h"
#include "lwip/timeouts.h"
static unsigned foreground_order;
static uint32_t foreground_feeds;
bool lm3s_netif_service(uint32_t ms)
{
    startup_model_t *m = startup_model(&boot_startup_continuity);
    CHECK(ms == sys_now() && foreground_order == 0);
    CHECK(boot_startup_continuity.watchdog.evidence == 0 && m->feeds == foreground_feeds);
    foreground_order = 1; return true;
}
bool lm3s_netif_ready(void) { return false; }
const ip4_addr_t *lm3s_netif_address(void) { CHECK(0); return NULL; }
err_t ftp_tcp_start(const ip4_addr_t *ip) { (void)ip; CHECK(0); return ERR_IF; }
void sys_check_timeouts(void)
{
    CHECK(foreground_order == 1 && boot_startup_continuity.watchdog.evidence == 0);
    foreground_order = 2;
}
bool ftp_tcp_service(uint32_t ms)
{
    startup_model_t *m = startup_model(&boot_startup_continuity);
    CHECK(ms == sys_now() && foreground_order == 2);
    CHECK(boot_startup_continuity.watchdog.evidence == 0 && m->feeds == foreground_feeds);
    foreground_order = 3; return true;
}
static uint32_t released_reads, released_writes, released_tar;
static void foreground_clock_transition(void)
{
    boot_startup_t *c = &boot_startup_continuity;
    startup_model_t *m = startup_model(c);
    foreground_model_t *f = foreground_model(c);
    uint32_t mask, offset;
    const uint32_t feeds = m->feeds, writes = m->writes, tar = f->tar_reads;
    __asm__ volatile("mrs %0, primask" : "=r"(mask)); CHECK(mask == 1);
    CHECK(f->timer == SLOT_INDEX + 2u && f->timer_ctl == 1u);
    CHECK(net_clock_initialize(c->hz));
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(mask) :: "memory");
    CHECK(mask == 0); /* production port actually exposed SysTick */
    CHECK(f->saw_start == 1 && f->tar_reads == tar + 1);
    CHECK(f->final_us == c->entry_us + 700 && m->clock_reads == 5);
    CHECK(net_clock_startup_elapsed(&offset));
    CHECK(offset == (uint32_t)(((uint64_t)f->final_us - c->entry_us) / 1000u + 1u));
    CHECK((*(volatile uint32_t *)0xe000e010u & 7u) == 7u);
    CHECK(*(volatile uint32_t *)0xe000e014u == 49999u);
    CHECK(m->writes == writes && m->feeds == feeds);
    /* Modeled idle network-init boundary; actual main order is separate static evidence. */
    CHECK(boot_startup_gptm_release(c));
    CHECK(f->timer_ctl == 0u);
    released_reads = f->timer_reads; released_writes = f->timer_writes; released_tar = f->tar_reads;
    const uint32_t now = sys_now();
    SysTick_Handler(); SysTick_Handler(); SysTick_Handler();
    CHECK(sys_now() == now + 3u);
    CHECK(m->writes == writes && m->feeds == feeds);
    CHECK(f->timer_reads == released_reads && f->timer_writes == released_writes && f->tar_reads == released_tar);
}
static void foreground_continuity_turns(void)
{
    boot_startup_t *c = &boot_startup_continuity;
    startup_model_t *m = startup_model(c);
    foreground_model_t *f = foreground_model(c);
    const uint32_t epoch = c->watchdog.epoch, feeds = m->feeds, writes = m->writes;
    const uint32_t entry_epoch = c->entry_epoch, verification = c->watchdog.verification;
    const uint32_t total = c->watchdog.total, completed = c->watchdog.completed;
    const unsigned phase = c->watchdog.phase;
    const uint64_t entry_us = c->entry_us;
    boot_watchdog_io_t io = c->watchdog.io;
    boot_startup_clock_t clock = c->clock;
    CHECK(epoch == entry_epoch + 1); /* original predicate turn already fed */
    CHECK(c->hz == STARTUP_HZ && c->watchdog.reload == STARTUP_LOAD);
    CHECK(c->watchdog.io.context == c->context && c->clock.context == c->context);
    boot_startup_gptm_t *g = (void *)c->context;
    CHECK(g->registers.context == (void *)c->reserve);
    CHECK((uintptr_t)g->registers.read_register < 0x8000u && (uintptr_t)g->registers.write_register < 0x8000u);
    CHECK((uintptr_t)g->gate_delay < 0x8000u && (uintptr_t)g->reader_identity < 0x8000u);
    for (unsigned i = 0; i < 2; ++i) {
        foreground_order = 0; foreground_feeds = m->feeds;
        SysTick_Handler();
        CHECK(foreground_owner_turn());
        CHECK(foreground_order == 3 && c->watchdog.epoch == epoch + i + 1);
        CHECK(m->feeds == feeds + i + 1 && m->writes == writes + 3 * (i + 1));
        CHECK(c->watchdog.evidence == 0 && c->entry_epoch == entry_epoch && c->entry_us == entry_us);
        CHECK((uintptr_t)&c->watchdog == 0x2000dc00u && c->watchdog.reload == STARTUP_LOAD);
        CHECK(c->watchdog.io.context == io.context && c->watchdog.io.read_register == io.read_register && c->watchdog.io.write_register == io.write_register);
        CHECK(c->clock.context == clock.context && c->clock.read == clock.read);
        CHECK(c->watchdog.verification == verification && c->watchdog.total == total && c->watchdog.completed == completed && c->watchdog.phase == phase);
        uint32_t mask; __asm__ volatile("mrs %0, primask" : "=r"(mask)); CHECK(mask == 1);
        CHECK((*(volatile uint32_t *)0xe000e010u & 7u) == 7u);
        CHECK(f->timer_reads == released_reads && f->timer_writes == released_writes && f->tar_reads == released_tar);
        boot_verified_image_t running;
        CHECK(boot_startup_running(c, &running) == 0);
    }
}
