/* Private bridge seam. Caller MUST hold watchdog->entered throughout the
 * transaction. These functions never release it; no external consumer API. */
#ifndef BOOT_WATCHDOG_INTERNAL_H
#define BOOT_WATCHDOG_INTERNAL_H

#include <stdint.h>
#include "lm3s6965/boot_watchdog.h"
#include "lm3s6965/boot_verify.h"

boot_watchdog_result_t boot_watchdog_service_held(boot_watchdog_t *w,
    boot_watchdog_owner_t owner, uint32_t epoch);
boot_watchdog_result_t boot_watchdog_report_held(boot_watchdog_t *w,
    uint32_t epoch, uint32_t verification, boot_verify_phase_t phase,
    uint32_t completed, uint32_t total);

#endif
