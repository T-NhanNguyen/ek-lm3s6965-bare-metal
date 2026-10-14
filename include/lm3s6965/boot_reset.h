/* Pure captured-reset classification and atomic metadata proposal, no MMIO. */
#ifndef LM3S6965_BOOT_RESET_H
#define LM3S6965_BOOT_RESET_H

#include <stdint.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_lifecycle.h"

#define BOOT_RESET_SW UINT32_C(0x10)
#define BOOT_RESET_WATCHDOG UINT32_C(0x08)
#define BOOT_RESET_BROWNOUT UINT32_C(0x04)
#define BOOT_RESET_POR UINT32_C(0x02)
#define BOOT_RESET_EXTERNAL UINT32_C(0x01)
#define BOOT_RESET_SUPPORTED UINT32_C(0x1f)

typedef struct
{
    uint32_t supported_flags; /* captured flags masked to exact-part support */
    uint32_t trial_applicable;
    uint32_t counted; /* 0 or 1, regardless of number of observed causes */
    uint32_t reason; /* UNUSED on no change, otherwise WATCHDOG..COMBINED */
    uint32_t consume_intent; /* exact applicable intent consumed with count */
} boot_reset_decision_t;

/* record MUST be loaded authoritative durable metadata; these forgeable C
 * structs cannot prove authority or execution. Only unconfirmed durable TRIAL
 * is applicable, including resets before candidate execution (policy, NOT
 * causal proof). Classify the captured snapshot, ignoring reserved flags.
 * WDT/EXT/BOR and SW with matching kind/slot/trial deadline intent are counted
 * categories; >=2 categories means COMBINED. POR is never a category, hence
 * POR+BOR alone is BROWNOUT. Count once, never at deadline expiry.
 *
 * USER POLICY: preserve existing schema/identity/history. On a counted startup
 * consume ONLY the matching active intent in the SAME count/history proposal,
 * even when no SW flag is present. On UNCOUNTED startup KEEP pending valid
 * intent: exact metadata copy, no sequence change, no journal and NO write.
 * Mismatched/noncanonical intent is a codec error, never silently repaired.
 * Third count uses the existing lifecycle rejection behavior unchanged.
 *
 * Inputs stable complete objects; out and decision must not overlap record or
 * each other (including exact in-place use). Every error leaves BOTH outputs
 * unchanged. NO_CHANGE copies record exactly, including at maximum sequence;
 * counted exhaustion is an error. CHANGE is not durable: commit once with
 * boot_store_replace and require OK readback; UNCERTAIN requires reload.
 * Never pass NO_CHANGE to the store. No capture/clear, flash or reset here.
 *
 * Accepted limitation: retained temporally stale but matching intent plus a
 * later SW reset can be misclassified as deadline. No freshness mechanism or
 * exactly-once guarantee. Raw cause clearing belongs to the next harness;
 * replaying a WDT/EXT/BOR snapshot can count again, while consumed intent alone
 * cannot make a subsequent SW-only snapshot count twice. */
boot_lifecycle_result_t boot_reset_propose(const boot_metadata_t *record,
    uint32_t captured_flags, boot_metadata_t *out, boot_reset_decision_t *decision);
#endif
