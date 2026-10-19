/* Real owner, checked inherited clock, startup validator and WDT. Bounded
 * service doubles expose ordering/fault cutpoints; no stack-composition claim. */
#include <setjmp.h>
#include "continuity.h"
#include "foreground_owner.h"
#include "net_clock.h"
#include "lm3s_netif.h"
#include "lm3s6965/ftp_tcp.h"
#include "lwip/timeouts.h"
volatile uint32_t test_systick[3];
static char trace[128];
static size_t count;
static unsigned reports, services, starts;
static bool link_ready, recurse_timer, busy_report;
static err_t start_result = ERR_OK;
static uint32_t expected_epoch, expected_feeds;
static ip4_addr_t address;
static jmp_buf stop_env;
static bool expect_stop;
static unsigned stops, timer_resumes, read_entries, read_resumes, outer_resumes;
static unsigned sticky_samples;
static void event(char e) { CHECK(count + 1 < sizeof trace); trace[count++] = e; trace[count] = 0; }
static void before_work(void)
{
    CHECK(reports == 0 && services == 0 && model()->feeds == expected_feeds);
    CHECK(boot_startup_continuity.watchdog.evidence == 0);
}
bool lm3s_netif_service(uint32_t ms)
{ CHECK(ms == sys_now()); before_work(); event('E'); return true; }
bool lm3s_netif_ready(void) { return link_ready; }
const ip4_addr_t *lm3s_netif_address(void) { return &address; }
err_t ftp_tcp_start(const ip4_addr_t *a)
{ CHECK(a == &address); before_work(); ++starts; event('L'); return start_result; }
void sys_check_timeouts(void)
{
    before_work(); event('T');
    if (recurse_timer) {
        (void)foreground_owner_turn();
        ++timer_resumes;
    }
}
bool ftp_tcp_service(uint32_t ms)
{ CHECK(ms == sys_now()); before_work(); event('F'); return true; }
static boot_watchdog_result_t observed_report(boot_watchdog_t *w,
    uint32_t epoch, uint32_t predicate, unsigned complete)
{
    static const uint32_t order[] = {BOOT_WATCHDOG_ETHERNET,
        BOOT_WATCHDOG_TIMERS, BOOT_WATCHDOG_FTP};
    CHECK(w == &boot_startup_continuity.watchdog && epoch == expected_epoch);
    CHECK(count && trace[count-1] == (reports == 0 ? 'F' : 'R'));
    CHECK(reports < 3 && predicate == order[reports] && complete == 1);
    ++reports; event('R');
    if (busy_report && reports == 2) return BOOT_WATCHDOG_BUSY;
    return boot_watchdog_application_report(w, epoch, predicate, complete);
}
static boot_watchdog_result_t observed_service(boot_watchdog_t *w,
    boot_watchdog_owner_t owner, uint32_t epoch)
{
    CHECK(w == &boot_startup_continuity.watchdog && epoch == expected_epoch);
    CHECK(owner == BOOT_WATCHDOG_APPLICATION && reports == 3 && services == 0);
    ++services; event('W');
    return boot_watchdog_service(w, owner, epoch);
}
#define boot_watchdog_application_report observed_report
#define boot_watchdog_service observed_service
#include "../../../examples/ethernet-ftp/foreground_owner.c"
#undef boot_watchdog_application_report
#undef boot_watchdog_service
_Noreturn void boot_startup_stop(void)
{
    CHECK(expect_stop && active && failed);
    ++stops;
    longjmp(stop_env, 1);
}
/* Delegate to the ordinary retained MMIO model, then reenter at the first
 * real watchdog-service read. No fake service or modeled feed cutpoint. */
static int reentry_read(void *p, uint32_t a, uint32_t *v)
{
    int result = model_read(p, a, v);
    CHECK(a == BOOT_WATCHDOG_RCGC0 && result == 0);
    CHECK(reports == 3 && services == 1 && read_entries == 0);
    CHECK(boot_startup_continuity.watchdog.evidence == 0);
    CHECK(model()->writes == 0 && model()->feeds == 0);
    CHECK(boot_startup_continuity.watchdog.epoch == expected_epoch);
    ++read_entries;
    (void)foreground_owner_turn();
    ++read_resumes;
    return result;
}
static void terminal_turn(void)
{
    CHECK(!expect_stop && stops == 0);
    expect_stop = true;
    if (setjmp(stop_env) == 0) {
        (void)foreground_owner_turn();
        ++outer_resumes;
        CHECK(0); /* Neither true nor false is a terminal-stop outcome. */
    }
    expect_stop = false;
    CHECK(stops == 1 && active && failed);
    CHECK(timer_resumes == 0 && read_resumes == 0 && outer_resumes == 0);
}
static int sticky_sample(void *p, boot_startup_sample_t *s)
{
    ++sticky_samples;
    return sample(p, s);
}
static void clear_turn(void)
{
    count = reports = services = 0; trace[0] = 0;
    expected_epoch = boot_startup_continuity.watchdog.epoch;
    expected_feeds = model()->feeds;
}
static void sticky(void)
{
    size_t n = count;
    owner_model_t saved = *model();
    uint32_t evidence = boot_startup_continuity.watchdog.evidence;
    unsigned r = reports, w = services, l = starts;
    uint32_t epoch = boot_startup_continuity.watchdog.epoch;
    boot_startup_clock_t clock = boot_startup_continuity.clock;
    CHECK(failed && !active && stops == 0 && sticky_samples == 0);
    boot_startup_continuity.clock.read = sticky_sample;
    for (unsigned retry = 0; retry < 2; ++retry) {
        CHECK(!foreground_owner_turn());
        CHECK(failed && !active && stops == 0 && sticky_samples == 0);
        CHECK(count == n && memcmp(&saved, model(), sizeof saved) == 0);
        CHECK(reports == r && services == w && starts == l);
        CHECK(boot_startup_continuity.watchdog.evidence == evidence);
        CHECK(boot_startup_continuity.watchdog.epoch == epoch);
    }
    boot_startup_continuity.clock = clock;
}
static int run_case(int argc, char **argv)
{
    CHECK(argc == 2);
    continuity_ready(1234567);
    /* Standalone application tick: the owner has no retained clock dependency. */
    CHECK(net_clock_initialize(1000));
    boot_startup_t *c = &boot_startup_continuity;
    const uint64_t origin = c->entry_us;
    boot_watchdog_io_t io = c->watchdog.io;
    boot_startup_clock_t clock = c->clock;
    if (!strcmp(argv[1], "owner_completed_turn_sequence")) {
        /* Genuine successful feeds advance the live epoch beyond entry_epoch. */
        for (unsigned turn = 0; turn < 4; ++turn) {
            clear_turn(); link_ready = turn != 0; start_result = turn == 1 ? ERR_MEM : ERR_OK;
            model()->us += 1000;
            CHECK(foreground_owner_turn());
            CHECK(!strcmp(trace, turn == 1 || turn == 2 ? "ELTFRRRW" : "ETFRRRW"));
            CHECK(c->watchdog.epoch == 10 + turn && c->entry_epoch == 9);
            CHECK(c->watchdog.evidence == 0 && model()->feeds == turn + 1);
            CHECK(model()->writes == 3 * (turn + 1));
            CHECK(c->entry_us == origin && c->watchdog.reload == 500);
            CHECK(c->watchdog.verification == 7 && c->watchdog.total == 4096 && c->watchdog.completed == 4096);
            CHECK(c->watchdog.phase == BOOT_VERIFY_PAYLOAD_PROGRESS);
            CHECK(c->watchdog.io.context == io.context && c->watchdog.io.read_register == io.read_register && c->watchdog.io.write_register == io.write_register);
            CHECK(c->clock.context == clock.context && c->clock.read == clock.read);
        }
        CHECK(starts == 2);
    } else {
        clear_turn();
        if (!strcmp(argv[1], "owner_timer_reentry_quarantines")) {
            recurse_timer = true; terminal_turn();
            CHECK(!strcmp(trace, "ET") && reports == 0 && services == 0);
            CHECK(model()->writes == 0 && model()->feeds == 0);
            CHECK(c->watchdog.epoch == 9 && c->watchdog.evidence == 0);
            return 0; /* A terminal turn remains active; never resume/retry it. */
        } else if (!strcmp(argv[1], "owner_watchdog_read_reentry_stops_before_icr")) {
            c->watchdog.io.read_register = reentry_read;
            terminal_turn();
            CHECK(!strcmp(trace, "ETFRRRW") && reports == 3 && services == 1);
            CHECK(read_entries == 1 && model()->reads == 1);
            CHECK(model()->writes == 0 && model()->feeds == 0);
            CHECK(c->watchdog.epoch == 9 && c->watchdog.evidence == 0);
            return 0; /* longjmp models terminal stop, not watchdog recovery. */
        } else if (!strcmp(argv[1], "owner_partial_report_failure_no_retry")) {
            busy_report = true; CHECK(!foreground_owner_turn());
            CHECK(!strcmp(trace, "ETFRR") && reports == 2 && services == 0);
            CHECK(c->watchdog.evidence == BOOT_WATCHDOG_ETHERNET);
            CHECK(model()->feeds == 0 && c->watchdog.epoch == 9 && !c->watchdog.blocked);
        } else if (!strcmp(argv[1], "owner_ambiguous_feed_no_retry")) {
            model()->fail_after_feed = 1; CHECK(!foreground_owner_turn());
            CHECK(!strcmp(trace, "ETFRRRW") && reports == 3 && services == 1);
            CHECK(model()->feeds == 1 && c->watchdog.epoch == 9);
            CHECK(c->watchdog.blocked && c->watchdog.evidence == 0);
        } else CHECK(0);
        sticky();
    }
    return 0;
}

int main(int argc, char **argv)
{
    int result = run_case(argc, argv);
    if (result == 0) printf("FOREGROUND_CASE_OK %s\n", argv[1]);
    return result;
}
