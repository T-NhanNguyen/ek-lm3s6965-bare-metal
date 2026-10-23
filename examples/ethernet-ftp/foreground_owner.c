#include "foreground_owner.h"

#include <stdbool.h>
#include <stdint.h>

#include "lm3s6965/boot_diag.h"
#include "lm3s6965/boot_diag_publication.h"
#include "lm3s6965/boot_startup.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"
#include "lm3s6965/ftp_tcp.h"
#include "lm3s_netif.h"
#include "lwip/err.h"
#include "lwip/timeouts.h"
#include "net_clock.h"

static bool active, failed, transport_started;

static bool running(boot_startup_t *c)
{
    boot_verified_image_t image;
    return c->state == BOOT_STARTUP_C_READY &&
        boot_startup_running(c, &image) == 0;
}

bool foreground_owner_turn(void)
{
    boot_startup_t *const c = &boot_startup_continuity;
    boot_watchdog_t *const watchdog = &c->watchdog;
    if (active) {
        failed = true;
        /* A suspended callback may still feed: never let it resume. */
        boot_startup_stop();
    }
    if (failed) return false;
    active = true;
    const uint32_t epoch = watchdog->epoch;
    /* No evidence may be banked from another/partial turn, even in the same
     * epoch. Running identity uses immutable entry history, not entry_epoch
     * equality with the legitimately evolving live controller epoch. */
    if (!running(c) || watchdog->evidence != 0u ||
        !lm3s_netif_service(sys_now()) || failed)
        goto failure;

    if (!lm3s_netif_ready()) {
        transport_started = false;
    } else if (!transport_started) {
        const err_t result = ftp_tcp_start(lm3s_netif_address());
        /* Listener allocation pressure is handled backpressure, not owner
         * failure. The FTP service still executes once, including while down. */
        if (result != ERR_OK && result != ERR_MEM) goto failure;
        transport_started = result == ERR_OK;
    }

    /* Pinned NO_SYS lwIP 2.2.1 samples sys_now at entry and completes that
     * due-timer snapshot before normal return. Readiness is not timer work.
     * This asserts completion, not a measured bound on arbitrary callbacks. */
    sys_check_timeouts();
    if (failed || !ftp_tcp_service(sys_now())) goto failure;

    /* Observe application SysTick after all work; the startup GPTM is already
     * released. No reports at all until all three services complete. */
    (void)sys_now();
    /* Complete the watchdog turn before publishing readiness. A failed report,
     * feed, or service is a failed owner turn and must not publish health. */
    if (failed || !running(c) || watchdog->epoch != epoch ||
        watchdog->evidence != 0u ||
        boot_watchdog_application_report(watchdog, epoch,
            BOOT_WATCHDOG_ETHERNET, 1u) != BOOT_WATCHDOG_OK ||
        boot_watchdog_application_report(watchdog, epoch,
            BOOT_WATCHDOG_TIMERS, 1u) != BOOT_WATCHDOG_OK ||
        boot_watchdog_application_report(watchdog, epoch,
            BOOT_WATCHDOG_FTP, 1u) != BOOT_WATCHDOG_OK ||
        boot_watchdog_service(watchdog, BOOT_WATCHDOG_APPLICATION,
            epoch) != BOOT_WATCHDOG_OK)
        goto failure;
    /* Publish current diagnostic health only after the complete successful
     * turn. Health is current, not sticky: APP always; ETHERNET only while
     * netif is ready; FTP only while the listener is started. The report guard
     * requires health 7, so a stale 7 after readiness loss would misstate boot
     * success. */
    {
        uint32_t health = BOOT_DIAG_HEALTH_APP;
        if (lm3s_netif_ready()) {
            health |= BOOT_DIAG_HEALTH_ETHERNET;
            if (transport_started) health |= BOOT_DIAG_HEALTH_FTP;
        }
        boot_diag_publication_result_t published =
            boot_diag_publication_progress(&c->publication,
                BOOT_DIAG_STAGE_STARTUP, health);
        /* Publication is a non-retry API. A non-OK result (BUSY, UNASSOCIATED,
         * UNCERTAIN) means readiness was not safely recorded; stop this owner
         * rather than pretend success. Do not retry or reconcile. */
        if (published != BOOT_DIAG_PUBLICATION_OK) {
            failed = true;
            active = false;
            return false;
        }
    }
    active = false;
    return true;

failure:
    /* A stopped or failed owner cannot vouch for Ethernet or FTP readiness.
     * Publish the safe conservative APP-only health so diagnostics never
     * retain a false full-health 7 after an init/listener/watchdog failure.
     * Publication is a non-retry API; ignore its result because the owner is
     * stopping and must not retry or reconcile. */
    {
        (void)boot_diag_publication_progress(&c->publication,
            BOOT_DIAG_STAGE_STARTUP, BOOT_DIAG_HEALTH_APP);
    }
    /* Never retry/reconcile, reuse partial evidence, or feed after failure.
     * Watchdog API failures also retain their own controller quarantine. */
    failed = true;
    active = false;
    return false;
}
