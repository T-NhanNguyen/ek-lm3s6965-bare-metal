/* Bounded HOST firmware transaction; NOT an ftp_storage_ops_t backend. */
#ifndef LM3S6965_BOOT_RECEIVE_H
#define LM3S6965_BOOT_RECEIVE_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_flash.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

typedef enum {
    BOOT_RECEIVE_OK, BOOT_RECEIVE_WAIT, BOOT_RECEIVE_ACCEPTED,
    BOOT_RECEIVE_ARGUMENT, BOOT_RECEIVE_BUSY, BOOT_RECEIVE_CONSUMED,
    BOOT_RECEIVE_AUTHORITY, BOOT_RECEIVE_RUNNING, BOOT_RECEIVE_EXHAUSTED,
    BOOT_RECEIVE_ADMISSION, BOOT_RECEIVE_MANIFEST, BOOT_RECEIVE_STORE,
    BOOT_RECEIVE_RECONCILE, BOOT_RECEIVE_FLASH, BOOT_RECEIVE_LENGTH,
    BOOT_RECEIVE_STORED, BOOT_RECEIVE_ABORTED
} boot_receive_result_t;
typedef enum {
    BOOT_RECEIVE_NEW, BOOT_RECEIVE_HEADER, BOOT_RECEIVE_AUTHENTICATE,
    BOOT_RECEIVE_INVALIDATE, BOOT_RECEIVE_ERASE, BOOT_RECEIVE_PAYLOAD,
    BOOT_RECEIVE_WRITE_MANIFEST, BOOT_RECEIVE_VERIFY, BOOT_RECEIVE_READY,
    BOOT_RECEIVE_TERMINAL
} boot_receive_phase_t;
typedef struct {
    boot_flash_t *flash;
    void *running_context;
    /* Zero only for stable, actually executing boot_verify_stored evidence.
     * Not a role/selection guess. Writes only out; no image/source mutation.
     * Read-only boot_flash_read on the configured controller is allowed;
     * blocked authentication routes to RECONCILE with its first flash cause. */
    int (*running)(void *, boot_verified_image_t *out);
    /* Optional cancellation/scheduling observation, NOT watchdog progress.
     * Same semantics as boot_verify; no IO or metadata mutation by observer. */
    boot_verify_progress_fn progress;
    void *progress_context;
    uint32_t permitted_slot;
} boot_receive_config_t;
typedef struct {
    boot_store_snapshot_t metadata;
    boot_verified_image_t image;
    unsigned reset_requested; /* Intent only: no reset or execution is performed. */
} boot_receive_acceptance_t;
typedef struct {
    atomic_flag entered;
    boot_receive_phase_t phase, failed_phase;
    boot_receive_result_t failure;
    boot_store_result_t store_result;
    boot_flash_result_t flash_result;
    boot_verify_result_t verify_result;
    boot_lifecycle_result_t lifecycle_result;
    boot_receive_config_t config;
    boot_store_adapter_t adapter;
    boot_store_snapshot_t authority;
    boot_upload_evidence_t evidence;
    boot_metadata_t proposal;
    boot_flash_erase_plan_t erase;
    boot_verified_image_t stored;
    uint8_t manifest[BOOT_MANIFEST_BYTES], word[4];
    size_t manifest_used, word_used;
    uint32_t received, programmed, manifest_programmed;
    unsigned eof;
} boot_receive_t;
#define BOOT_RECEIVE_INIT { .entered = ATOMIC_FLAG_INIT }

/* Initialize ONCE, unpublished, with BOOT_RECEIVE_INIT. Never copy/reinitialize
 * an admitted object. All APIs foreground-only. Exclusive ownership of receiver,
 * controller, metadata, both images and evidence spans begin through terminal;
 * no independent users, ISR, debugger/DMA writers or callback mutation. The
 * receiver's guard spans EVERY callback, including publication; reentry BUSY.
 * C structs cannot authenticate provenance or enforce the external lifetime lock.
 * Config is copied; no caller buffers retained. Callback contexts and controller
 * must outlive transaction, disjoint from receiver and all output/input spans.
 * Pre-admission ARGUMENT can be corrected. Every admitted begin is one-shot,
 * including failure. Fresh authority is loaded, two sequence advances reserved.
 * No reset accounting, automatic reconciliation, cached retry or reset IO.
 * Watchdog is deliberately NOT exposed/copied/reinitialized/transferred/fed.
 * Caller continues the SAME APPLICATION watchdog with genuine foreground
 * Ethernet/timer/FTP evidence between calls. Verifier events cannot replace it.
 * ECDSA, store and stored verification remain synchronous opaque service gaps;
 * bounded work/bytes/polls do NOT establish elapsed time or production fit. */
boot_receive_result_t boot_receive_begin(boot_receive_t *, const boot_receive_config_t *);
/* Copy at most manifest remainder or one partial word. accepted is exact even
 * on WAIT (zero); resubmit ONLY unaccepted suffix. No IO. Manifest+payload in
 * one input stops at 160, then step/admit/erase before more bytes. Nonempty
 * input beyond declared payload is terminal LENGTH; no implicit EOF. Arguments
 * and accepted must be disjoint from each other, receiver, controller and known
 * contexts. Rejected arguments leave accepted unchanged; otherwise set to zero
 * before work. Zero-size feed is harmless, not EOF. */
boot_receive_result_t boot_receive_feed(boot_receive_t *, const uint8_t *, size_t, size_t *accepted);
/* One action: authenticate, invalidate+committed readback+erase-plan admission,
 * one erase page (manifest first), one program word, full stored verification,
 * or READY+committed readback. WAIT means supply input/EOF. out is unchanged
 * through callbacks/errors; owned acceptance copied ONLY on ACCEPTED. Terminal
 * duplicates return CONSUMED with zero IO, unchanged out. out must be disjoint
 * from receiver/controller/contexts. Subordinate results + failed_phase retain
 * exact first failure for receiver-owned operations (including metadata IO).
 * A running callback's own verifier result is not exposed; if it blocks the
 * configured controller, flash_result retains that controller's first cause.
 * Any store uncertainty or blocked controller -> RECONCILE; all errors stop,
 * with no cleanup IO/retry. Reconcile externally before a new transaction. */
boot_receive_result_t boot_receive_step(boot_receive_t *, boot_receive_acceptance_t *out);
boot_receive_result_t boot_receive_eof(boot_receive_t *);
/* Disconnect/cancel: terminal, no cleanup IO. Cannot revoke already accepted
 * READY; a later abort returns CONSUMED, not a false rollback claim. */
boot_receive_result_t boot_receive_abort(boot_receive_t *);
#endif
