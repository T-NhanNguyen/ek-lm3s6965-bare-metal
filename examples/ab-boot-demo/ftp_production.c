/* Minimal production FTP receive binding for ab-boot-demo slot FTP images.
 *
 * Why this file exists: the shared ftp_firmware_receive binder only links the
 * ftp_boot operation (STOR firmware/confirm, RETR boot-status) onto the FTP
 * core singleton. Its production hook is weak-NULL (RAM-only hello). Each slot
 * FTP image overrides that hook here with a real bundle: MMIO flash
 * controller, store adapter, inherited diagnostic publication, and snapshot
 * sources sharing this foreground owner.
 *
 * Ownership/lifetime: every object below is static to this image and outlives
 * the binding. The receiver starts BOOT_RECEIVE_INIT and is never
 * reinitialized; one admitted receiver lifetime only. No watchdog is exposed,
 * copied, fed, or transferred here; the caller keeps the SAME APPLICATION
 * epoch and genuine ETHERNET/TIMERS/FTP evidence (see foreground_owner.c).
 *
 * Safety: the permitted destination is the INACTIVE peer slot, never the
 * running image (LM3S6965_AB_FTP_SLOT names this image's own link slot:
 * 0 = A, 1 = B). Overwriting the executing slot is refused by construction.
 * Failure (unready continuity, flash init refusal) returns NULL, which keeps
 * RAM-only hello: no fake evidence is ever supplied.
 */

#include "ftp_firmware_receive.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_flash.h"
#include "lm3s6965/boot_receive.h"
#include "lm3s6965/boot_snapshot.h"
#include "lm3s6965/boot_startup.h"
#include "lm3s6965/boot_target_io.h"
#include "lm3s6965/ftp_boot.h"
#include "lm3s6965/ftp_diagnostic.h"
#if LM3S6965_FTP_DIAGNOSTIC
#include "lwip/sys.h"
#endif

#if !defined(LM3S6965_AB_FTP_SLOT)
#error "LM3S6965_AB_FTP_SLOT must be 0 (slot A image) or 1 (slot B image)"
#endif

/* Bounded register polls for flash init; bounds reads, not elapsed time. */
#define AB_FTP_FLASH_POLLS UINT32_C(1000000)

static boot_flash_t flash;
static bool flash_ready;
static boot_receive_t receiver = BOOT_RECEIVE_INIT;
static ftp_boot_config_t config;
static bool configured;
#if LM3S6965_FTP_DIAGNOSTIC
static uint32_t diagnostic_clock(void *context)
{
    (void)context;
    return sys_now();
}
#endif
#if LM3S6965_AB_FTP_SLOT == 0
static const ftp_boot_report_descriptor_t report = {
    "slot-a-breathing", "Breathing LED with FTP service."
};
#elif LM3S6965_AB_FTP_SLOT == 1
static const ftp_boot_report_descriptor_t report = {
    "slot-b-blink", "Double-blink LED with FTP service."
};
#else
#error "LM3S6965_AB_FTP_SLOT must be 0 or 1"
#endif

const ftp_boot_config_t *ftp_firmware_receive_production_config(void)
{
    boot_startup_t *continuity;

    if (configured) {
        return &config;
    }
    continuity = &boot_startup_continuity;
    /* Only bind on genuine inherited application readiness with a real clock.
     * Anything else keeps RAM-only hello (NULL), never fabricated evidence. */
    if (continuity->state != BOOT_STARTUP_C_READY || continuity->hz == 0u) {
        return NULL;
    }
    if (!flash_ready) {
        /* Real controller: target MMIO register/flash callbacks, trusted
         * inherited Hz, bounded polls. Refusal keeps RAM-only hello. */
        if (boot_target_flash_init(&flash, continuity->hz,
                                   AB_FTP_FLASH_POLLS) != BOOT_FLASH_OK) {
            return NULL;
        }
        flash_ready = true;
    }
    config.report = &report;
    config.receiver = &receiver;
    config.receive.flash = &flash;
    config.receive.running_context = continuity;
    config.receive.running = boot_startup_running;
    config.receive.progress = NULL;
    config.receive.progress_context = NULL;
    /* Inactive peer only: a slot-A image may stage slot B and vice versa. */
#if LM3S6965_AB_FTP_SLOT == 0
    config.receive.permitted_slot = 1u;
#else
    config.receive.permitted_slot = 0u;
#endif
    /* Real inherited publication and coherent sources under this owner. */
    config.publication = &continuity->publication;
    config.sources.metadata = boot_flash_store_adapter(&flash);
    config.sources.running_context = continuity;
    config.sources.running = boot_startup_running;
#if LM3S6965_FTP_DIAGNOSTIC
    ftp_diagnostic_clock(diagnostic_clock, NULL);
#endif
    configured = true;
    return &config;
}
