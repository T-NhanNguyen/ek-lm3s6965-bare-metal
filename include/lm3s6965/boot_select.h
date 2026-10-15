/* HOST selection composition only; no reset, provisioning or physical entry. */
#ifndef LM3S6965_BOOT_SELECT_H
#define LM3S6965_BOOT_SELECT_H

#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

typedef struct
{
    uint8_t wire[2][BOOT_MANIFEST_BYTES]; /* physical A, B manifest prefixes */
    size_t size[2]; /* available prefix bytes; tails beyond 160 are ignored */
} boot_select_images_t;

typedef enum
{
    BOOT_SELECT_OK = 0,
    BOOT_SELECT_ARGUMENT,
    BOOT_SELECT_RECOVERY, /* no executable decision; intervention required */
    BOOT_SELECT_VERIFY_FAILED, /* operational failure, NOT invalid evidence */
    BOOT_SELECT_IDENTITY_FAILED,
    BOOT_SELECT_LIFECYCLE_FAILED,
    BOOT_SELECT_STORE_FAILED,
    BOOT_SELECT_RECONCILE /* UNCERTAIN: explicitly reload; never replay */
} boot_select_status_t;

typedef struct
{
    boot_select_status_t status;
    /* Known verifier evidence, not a complete attempt trace: candidate store
     * failure can occur before OR after verification. Only set entries count. */
    uint32_t evidence_mask;
    boot_verify_result_t verify_result[2];
    boot_lifecycle_result_t lifecycle_result;
    boot_store_result_t store_result;
} boot_select_result_t;

/* ALL boot_launch caller preconditions apply: successful reset accounting THIS
 * boot, established watchdog, truthful callbacks, stable physical images and
 * authority, exclusive/nonreentrant ownership through publication and use.
 * C structs do not prove provenance. This API does not establish those facts.
 * Snapshot adapter, expected and both manifest prefixes before any callback.
 * Inputs may alias valid inputs; out must not overlap any input or opaque
 * context storage. Known spans checked; opaque sizes are caller obligations.
 * Callbacks must not mutate inputs/output. All non-OK results leave out intact.
 *
 * READY/TRIAL candidate first via boot_launch_candidate. Only its IMAGE_INVALID
 * permits independent confirmed CURRENT verification, with NO candidate count,
 * rejection or metadata change. Identity/stale/exhaustion/store/operational
 * failures stop. Without candidate, verify confirmed CURRENT normally, no write.
 * Only image-invalid CURRENT permits independently verified, identity-matching
 * confirmed PREVIOUS promotion via lifecycle + store OK authoritative readback.
 * Candidate is never previous. Missing roles, neither-valid, absent/conflicting
 * metadata => RECOVERY, no guessed factory selection or automatic provisioning.
 * Normal/fallback CURRENT rechecks canonical authority AND selected page before
 * zero-write publication (including maximum sequence). Promotion may exhaust.
 * UNCERTAIN => RECONCILE without reload/retry/publication. Caller must explicitly
 * reload and reconcile authority before a fresh call; never replay cached work.
 * Success returns an owned image and authoritative snapshot, NOT execution or
 * handoff permission after ownership/stability ends.
 */
boot_select_result_t boot_select(const boot_store_adapter_t *store,
    const boot_store_snapshot_t *expected, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_launch_decision_t *out);

#endif
