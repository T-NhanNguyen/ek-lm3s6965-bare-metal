/* Private launch seam. Caller holds both object guards through publication. */
#ifndef BOOT_HANDOFF_INTERNAL_H
#define BOOT_HANDOFF_INTERNAL_H

#include "lm3s6965/boot_handoff.h"
#include "lm3s6965/boot_watchdog.h"

boot_handoff_result_t boot_handoff_run_held(boot_handoff_t *h,
    boot_watchdog_t *w, const boot_handoff_request_t *request,
    boot_handoff_intent_t *out);

#endif
