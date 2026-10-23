#include "check.h"
#define main platform_legacy_main
#include "../platform/ftp_platform_test.c"
#undef main
#include "lwip/timeouts.h"
/* Same real lwIP instance as the legacy platform fixture; no owner double.
 * Keep the clock fixed during each dispatcher call. This checks actual due
 * work and a future child, not owner/report ordering or callbacks that advance
 * time during dispatch. All times precede the fixture's background deadlines. */
static unsigned due_callbacks, child_callbacks;
static void future_child(void *context)
{
    CHECK(context == &due_callbacks && sys_now() == 12);
    CHECK(due_callbacks == 1 && child_callbacks == 0);
    ++child_callbacks;
}
static void due_at_entry(void *context)
{
    CHECK(context == &due_callbacks && sys_now() == 5);
    CHECK(due_callbacks == 0 && child_callbacks == 0);
    ++due_callbacks;
    sys_timeout(7, future_child, context);
}
static void real_due_timer_checks(void)
{
    /* Legacy clock assertions end at zero; do not reinitialize lwIP or time. */
    CHECK(sys_now() == 0 && due_callbacks == 0 && child_callbacks == 0);
    CHECK(sys_timeouts_sleeptime() > 12); /* no background timeout in window */
    sys_timeout(5, due_at_entry, &due_callbacks);
    CHECK(sys_timeouts_sleeptime() == 5);
    milliseconds = 5;
    sys_check_timeouts();
    CHECK(due_callbacks == 1 && child_callbacks == 0);
    CHECK(sys_timeouts_sleeptime() == 7); /* child remains pending */
    sys_check_timeouts(); /* idle call at the same time must do no work */
    CHECK(due_callbacks == 1 && child_callbacks == 0);
    CHECK(sys_timeouts_sleeptime() == 7);
    milliseconds = 11;
    sys_check_timeouts();
    CHECK(due_callbacks == 1 && child_callbacks == 0);
    CHECK(sys_timeouts_sleeptime() == 1);
    milliseconds = 12;
    sys_check_timeouts();
    CHECK(due_callbacks == 1 && child_callbacks == 1);
    milliseconds = 13;
    sys_check_timeouts(); /* completed callbacks are not dispatched again */
    CHECK(due_callbacks == 1 && child_callbacks == 1);
}
static int run_case(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "netif_completed_work_returns_true")) {
        CHECK(platform_legacy_main() == 0);
        /* Additional consumed pbuf_take failure, real allocation/free. */
        poll_result = ETHERNET_OK; fake_status.mac_ready = true;
        CHECK(lm3s_netif_service(54)); CHECK(ready);
        const unsigned before = inputs;
        const mem_size_t used = lwip_stats.mem.used;
        const unsigned pool_used = lwip_stats.memp[MEMP_PBUF_POOL]->used;
        pending = 1; take_fail = true;
        CHECK(lm3s_netif_service(55));
        CHECK(pending == 0 && inputs == before);
        CHECK(lwip_stats.mem.used == used && lwip_stats.memp[MEMP_PBUF_POOL]->used == pool_used);
        real_due_timer_checks();
        return 0;
    }
    if (!strcmp(argv[1], "netif_uninitialized_returns_false")) {
        CHECK(!lm3s_netif_service(0));
        CHECK(polls == 0 && reads == 0 && inputs == 0 && !initialized && !ready);
        CHECK(netif_default == NULL); return 0;
    }
    lwip_init();
    CHECK(lm3s_netif_initialize(50000000, 0) == ERR_OK);
    fake_status.mac_ready = true;
    CHECK(lm3s_netif_service(0) && ready);
    reads = 0; pending = 2;
    if (!strcmp(argv[1], "netif_phy_timeout_returns_false")) {
        poll_result = ETHERNET_TIMEOUT;
        CHECK(!lm3s_netif_service(10));
        CHECK(!ready && teardowns == 1 && reads == 0 && pending == 2);
    } else if (!strcmp(argv[1], "netif_rx_error_returns_false")) {
        rx_result = ETHERNET_NOT_READY;
        CHECK(!lm3s_netif_service(1));
        CHECK(reads == 1 && inputs == 0 && pending == 2 && ready);
        CHECK(lwip_stats.memp[MEMP_PBUF_POOL]->used == 0);
    } else CHECK(0);
    return 0;
}

int main(int argc, char **argv)
{
    int result = run_case(argc, argv);
    if (result == 0) printf("FOREGROUND_CASE_OK %s\n", argv[1]);
    return result;
}
