/* Verified candidate decision only; no reset composition or physical handoff. */
#ifndef LM3S6965_BOOT_LAUNCH_H
#define LM3S6965_BOOT_LAUNCH_H

#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

typedef enum
{
    BOOT_LAUNCH_OK = 0,
    BOOT_LAUNCH_ARGUMENT,
    BOOT_LAUNCH_INELIGIBLE,
    BOOT_LAUNCH_IMAGE_INVALID,
    BOOT_LAUNCH_VERIFY_FAILED, /* operational failure, not invalid-image proof */
    BOOT_LAUNCH_LIFECYCLE_FAILED,
    BOOT_LAUNCH_STORE_FAILED,
    BOOT_LAUNCH_RECONCILE /* exact BOOT_STORE_UNCERTAIN; no launch decision */
} boot_launch_status_t;

typedef struct
{
    boot_launch_status_t status;
    /* Consult only for IMAGE_INVALID/VERIFY_FAILED, LIFECYCLE_FAILED, or
     * STORE_FAILED/RECONCILE respectively. Other values are neutral defaults. */
    boot_verify_result_t verify_result;
    boot_lifecycle_result_t lifecycle_result;
    boot_store_result_t store_result;
} boot_launch_result_t;

typedef struct
{
    boot_verified_image_t image;
    boot_store_snapshot_t snapshot; /* exact authoritative TRIAL readback */
} boot_launch_decision_t;

/* Caller prerequisite: reset handler returned BOOT_RESET_HANDLER_OK in this
 * boot. expected is that final authoritative snapshot, or an explicitly loaded
 * and reconciled authority after a failed launch transaction following that
 * successful reset pass (not after a failed reset pass). NEVER accept a snapshot
 * field from a failed reset report, even if its valid flag is set. This wrapper
 * does not capture/clear/account resets, nor prove provenance from a C struct.
 *
 * Caller has established existing watchdog protection before this call.
 * Progress and reader callbacks are passed unchanged to boot_verify_stored;
 * no blind feed, scheduler or timing guarantee is supplied here.
 * wire names the physical candidate manifest and expected names both pages.
 * Exclusively own both metadata pages, image storage, adapters, arguments and
 * contexts throughout this synchronous non-reentrant call. No ISR/concurrent
 * writer/reentry or callback mutation of inputs/output/images/metadata. Only
 * adapter-requested metadata changes are permitted. Callback return/effect
 * contracts are those of boot_verify and boot_store. A callback must not call
 * this API again. There is no runtime lock or global reentry detector.
 * Keep the verified image and authority stable from success through decision
 * use; publication is NOT a jump or entitlement after ownership is released.
 *
 * Inputs may alias each other if they still cover valid stable objects. out
 * must not overlap any input, wire span, or opaque context storage. Known
 * spans are checked; opaque context sizes remain caller obligations. All
 * failures leave out unchanged. Callbacks must not write out.
 *
 * Reload/check expected before verification. READY: verify exact candidate,
 * check resident identity, propose TRIAL, then store_replace. Only STORE_OK
 * committed readback publishes. TRIAL: independently verify again, propose
 * NO_CHANGE, reload/check authority again, publish with ZERO erase/program.
 * No new trial ID/count or fallback/confirmed/recovery selection is made.
 * UNCERTAIN returns RECONCILE without reload/retry/publication. Caller must
 * stop, load authority and reconcile old/new/recovery BEFORE any later call;
 * do not replay the cached READY transaction or treat the failure as rollback.
 */
boot_launch_result_t boot_launch_candidate(const boot_store_adapter_t *store,
    const boot_store_snapshot_t *expected, const uint8_t *wire, size_t size,
    boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_launch_decision_t *out);

#endif
