/* Logical final-handoff host gate. No MMIO, MSP switch or application call. */
#ifndef LM3S6965_BOOT_HANDOFF_H
#define LM3S6965_BOOT_HANDOFF_H

#include <stdint.h>
#include <stdatomic.h>

#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"

typedef struct
{
    uint32_t ipsr, control, primask, basepri, faultmask;
} boot_handoff_context_t;

typedef struct
{
    boot_verified_image_t image;
    uint32_t selected_slot, watchdog_epoch;
    boot_handoff_context_t observed;
} boot_handoff_request_t;

typedef struct
{
    uint32_t vtor, msp, reset_vector;
    uint32_t control, primask, basepri, faultmask;
    uint32_t application_epoch;
} boot_handoff_intent_t;

/* Bank events describe logical IRQ 0..31 and 32..42, NOT register masks.
 * Barrier events express ordering intent, NOT a target instruction contract. */
typedef enum
{
    BOOT_HANDOFF_EXCLUDE,
    BOOT_HANDOFF_STOP_SYSTICK,
    BOOT_HANDOFF_DISABLE_BANK0,
    BOOT_HANDOFF_DISABLE_BANK1,
    BOOT_HANDOFF_QUIESCE_SOURCES,
    BOOT_HANDOFF_CLEAR_CORE_PENDING,
    BOOT_HANDOFF_CLEAR_BANK0_PENDING,
    BOOT_HANDOFF_CLEAR_BANK1_PENDING,
    BOOT_HANDOFF_BARRIER_BEFORE_VTOR,
    BOOT_HANDOFF_SET_VTOR,
    BOOT_HANDOFF_BARRIER_AFTER_VTOR
} boot_handoff_operation_t;

typedef struct
{
    void *context;
    int (*operation)(void *context, boot_handoff_operation_t operation,
                     uint32_t value);
    int (*terminal_intent)(void *context, const boot_handoff_intent_t *intent);
} boot_handoff_io_t;

typedef enum
{
    BOOT_HANDOFF_OK = 0,
    BOOT_HANDOFF_ARGUMENT,
    BOOT_HANDOFF_BUSY,
    BOOT_HANDOFF_BLOCKED,
    BOOT_HANDOFF_STATE,
    BOOT_HANDOFF_IO
} boot_handoff_result_t;

typedef struct
{
    boot_handoff_io_t io;
    atomic_flag entered;
    unsigned blocked, finished;
} boot_handoff_t;

/* Fresh unpublished storage ONCE only; no reinit escape after admission. */
boot_handoff_result_t boot_handoff_init(boot_handoff_t *handoff,
                                       const boot_handoff_io_t *io);
/* Pure structural admission, not signature or eligibility verification.
 * Caller supplies a successful unchanged boot_verify_stored result, selected
 * image authority and a truthful current observation of privileged Thread mode.
 * No ISR, nPRIV or unsupported CONTROL bits; SPSEL may be 0 or 1. Inherited
 * masks must be representable, not reset values: PRIMASK/FAULTMASK 0 or 1,
 * BASEPRI only bits 7:5 (the part has three priority bits).
 * Input/output may not overlap. Failure leaves output unchanged. */
boot_handoff_result_t boot_handoff_admit(const boot_handoff_request_t *request,
                                        boot_handoff_intent_t *out);
/* Caller exclusively owns the WHOLE transaction: handoff/watchdog objects,
 * controller, selected storage, context and all sources. No ISR/other users,
 * watchdog calls or clock transitions during callbacks. Preserve the same
 * configured enabled watchdog object into application. Guards reject reentry,
 * not external hardware access. Inputs are copied before the first callback.
 * Operations return 0 only after completion; nonzero may already have effects.
 * QUIESCE must stop work and repending sources while preserving WDT + clock.
 * Every admitted callback failure permanently quarantines: no cleanup/release,
 * retry, rollback or further operations. Caller must not resume boot work or
 * feed through another API after failure; stop and retain watchdog protection.
 * EXCLUDE stays logically held even on
 * success; terminal callback records intent ONLY and must not execute an app
 * or restore physical masks. Transfer occurs once immediately before it, using
 * boot_watchdog_transfer without IO/feed; terminal failure does NOT undo it.
 * A terminal error can mean intent was recorded: caller must not infer absence
 * of effects. OK is host intent success, not physical execution. Output changes
 * only on OK. Output storage must be disjoint from the request, callback
 * context storage and both handoff/watchdog objects; request snapshotting does
 * not relax this requirement. Callbacks must not mutate either object, IO or
 * output. */
boot_handoff_result_t boot_handoff_run(boot_handoff_t *handoff,
    boot_watchdog_t *watchdog, const boot_handoff_request_t *request,
    boot_handoff_intent_t *out);

#endif
