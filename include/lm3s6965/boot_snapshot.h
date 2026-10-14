/* Coherent foreground sampling, not diagnostic source attribution or status formatting. */
#ifndef LM3S6965_BOOT_SNAPSHOT_H
#define LM3S6965_BOOT_SNAPSHOT_H
#include "lm3s6965/boot_diag_publication.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

/* Truthful, bounded read-only sources under the publication owner's exclusive
 * session. read follows boot_store's contract; erase/program are never used.
 * running returns zero only for actually executing, previously authenticated
 * stored-image evidence (including vectors), NOT a selection or role guess.
 * No verification is performed here. All participants must use this owner and
 * prevent source/image changes throughout capture and between verification and
 * use. Interrupt masking is not protection against DMA, debugger, another
 * owner, or a lying/stale callback. Such callbacks violate this API contract.
 * No allocation, blocking, formatting, crypto, programming or erase in either
 * callback. No mutation/reentry except attempted publisher calls (BUSY).
 * Source descriptor and output must be disjoint from each other, owner and
 * diagnostic port. Callback-managed storage must not alias output/port; it
 * may reference, but must not mutate, the owner. All are exclusively owned;
 * callbacks may write only their requested output spans (host trace counters
 * may be separate callback-local state). */
typedef struct {
    boot_store_adapter_t metadata;
    void *running_context;
    int (*running)(void *, boot_verified_image_t *);
} boot_snapshot_sources_t;

typedef struct {
    boot_diag_t diagnostic;
    boot_diag_context_t diagnostic_context;
    boot_store_snapshot_t metadata;
    boot_verified_image_t running;
} boot_snapshot_t;

typedef enum {
    BOOT_SNAPSHOT_OK, BOOT_SNAPSHOT_ARGUMENT, BOOT_SNAPSHOT_PUBLICATION,
    BOOT_SNAPSHOT_METADATA, BOOT_SNAPSHOT_RUNNING, BOOT_SNAPSHOT_IDENTITY
} boot_snapshot_result_t;

/* Captures ALL values before owner unlock; owned fixed-size output only on
 * success. No writes to the diagnostic port. Diagnostic numbers/context are
 * copied verbatim: no equality to live metadata/roles/running is implied.
 * In particular latched failed-image evidence can precede running fallback.
 * Current root-status trial/state sourcing and FTP rendering are separate. */
boot_snapshot_result_t boot_snapshot_capture(boot_diag_publication_t *,
    const boot_snapshot_sources_t *, boot_snapshot_t *);
#endif
