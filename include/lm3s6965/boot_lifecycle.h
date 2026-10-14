/* Pure lifecycle metadata proposals; no flash, MMIO or reset classification. */
#ifndef LM3S6965_BOOT_LIFECYCLE_H
#define LM3S6965_BOOT_LIFECYCLE_H
#include "lm3s6965/boot_verify.h"

/* Evidence is explicit, but NOT an authentication token. Caller provenance:
 * incoming is the successful output of boot_verify_manifest for permitted_slot;
 * running is the independently boot_verify_stored-validated running image.
 * Caller guarantees these images/evidence have not changed, running really is
 * executing, and permitted_slot is the authorized inactive physical slot.
 * These C structs can be forged: structural/identity/vector checks below do NOT
 * verify signatures or payload hashes and cannot establish that provenance.
 * Retain the same incoming evidence across invalidation and acceptance. */
typedef struct
{
    boot_manifest_t incoming;
    boot_verified_image_t running;
    uint32_t permitted_slot;
} boot_upload_evidence_t;

typedef enum
{
    BOOT_LIFECYCLE_CHANGE = 0, /* proposal only; NO write has occurred */
    BOOT_LIFECYCLE_ERROR_ARGUMENT,
    BOOT_LIFECYCLE_ERROR_RECORD,
    BOOT_LIFECYCLE_ERROR_ADMISSION,
    BOOT_LIFECYCLE_ERROR_IDENTITY,
    BOOT_LIFECYCLE_ERROR_IMAGE,
    BOOT_LIFECYCLE_ERROR_EXHAUSTED,
    BOOT_LIFECYCLE_NO_CHANGE, /* out is an exact copy of record; DO NOT write */
    BOOT_LIFECYCLE_ERROR_HEALTH
} boot_lifecycle_result_t;

/* Inputs must cover their objects and be stable for the entire call. Inputs
 * may alias each other; output must not overlap ANY input (rejected, including
 * exact in-place use). All errors leave output unchanged. CHANGE always means
 * a codec-valid exactly-next-sequence proposal, never durability/entitlement.
 * record must be current authoritative metadata, not guessed roles.
 * No READY/TRIAL candidate, running=current confirmed, inactive target only.
 * Clear only destination's 48-byte entry/roles/confirmation/eligibility; retain
 * all other identity/history. Even an already-empty destination needs a commit.
 * Commit via boot_store_replace with the loaded expected snapshot and require
 * BOOT_STORE_OK before ANY destination erase (old manifest FIRST). On UNCERTAIN
 * reload authority; never erase on the basis of this RAM proposal. */
boot_lifecycle_result_t boot_lifecycle_invalidate(const boot_metadata_t *record,
    const boot_upload_evidence_t *evidence, boot_metadata_t *out);

/* record must be the durably committed invalidation snapshot; destination must
 * still be canonical empty, not a retained entry. Caller has received EXACTLY
 * the package (no short/trailing bytes), and stored is boot_verify_stored's
 * successful output for that destination, under exclusive image ownership.
 * Require exact admitted manifest match (including signature), admissible
 * vectors, and running current still confirmed. No new cryptographic proof is
 * made here. Install READY, unconfirmed, zero counters/reasons/rollback and
 * trial_id=acceptance sequence; never mutate the other entry.
 * Commit/readback via boot_store_replace before reporting acceptance/requesting
 * reset. A stale invalidated snapshot can pass this pure function: replace's
 * authoritative re-read MUST reject it before destruction. No pure record API
 * can detect external flash changes or prove an earlier commit. */
boot_lifecycle_result_t boot_lifecycle_accept_ready(const boot_metadata_t *record,
    const boot_upload_evidence_t *evidence, const boot_verified_image_t *stored,
    boot_metadata_t *out);
/* Execution evidence must be successful boot_verify_stored output, stable
 * under exclusive image ownership. It is NOT a cryptographic token: caller
 * establishes provenance and (confirm/deadline) that this image really runs.
 * The same no-output/input-overlap rule applies to ALL functions, including
 * NO_CHANGE. Errors leave out untouched; NO_CHANGE copies record exactly.
 * Only CHANGE advances sequence. Never pass NO_CHANGE to boot_store_replace.
 * CHANGE requires BOOT_STORE_OK committed readback before first execution,
 * confirmation success, or reset respectively. UNCERTAIN requires reload. */
typedef struct
{
    uint32_t slot;
    uint8_t payload_hash[32];
    uint32_t trial_id;
} boot_confirm_request_t;

#define BOOT_HEALTH_APP_INIT UINT32_C(1)
#define BOOT_HEALTH_ETHERNET_INIT UINT32_C(2)
#define BOOT_HEALTH_FTP_LISTENING UINT32_C(4)
#define BOOT_HEALTH_REQUIRED UINT32_C(7)

/* Validate exact candidate, READY->TRIAL before first execute; preserve ID and
 * history. Independently revalidated TRIAL retry is NO_CHANGE even at maxseq. */
boot_lifecycle_result_t boot_lifecycle_begin_trial(const boot_metadata_t *record,
    const boot_verified_image_t *candidate, boot_metadata_t *out);

/* Explicit parsed PC request plus independent running validation. New confirm
 * requires running=candidate TRIAL (READY cannot already legitimately run),
 * exact slot/hash/retained ID, and all three health bits (no reserved bits).
 * Atomically promote roles/mask/state, retaining both histories/IDs; canonical
 * clear intent as candidate ceases to be active. Matching already-confirmed
 * running identity/retained ID is NO_CHANGE regardless of current/previous
 * role, without renewed health gating, including
 * factory ID zero and max sequence/ID. No FTP parser is implemented here. */
boot_lifecycle_result_t boot_lifecycle_confirm(const boot_metadata_t *record,
    const boot_confirm_request_t *request, const boot_verified_image_t *running,
    uint32_t health, boot_metadata_t *out);

typedef struct
{
    uint32_t slot;
    uint32_t trial_id;
    uint32_t reason; /* exactly BOOT_REASON_DEADLINE */
} boot_deadline_request_t;

/* CALLER PRECONDITION: 60 seconds elapsed since APPLICATION STARTUP entry,
 * still unconfirmed, coherent/exclusive running evidence. No clock/reset here.
 * Exact running unconfirmed TRIAL only; install slot/trial DEADLINE intent,
 * never increment counters. Exact applicable duplicate is NO_CHANGE (maxseq
 * allowed), not intent consumption/classification. Require durable authoritative
 * readback of applicable intent before reset, even for a duplicate; a RAM
 * proposal or an uncertain store result is never reset permission. */
boot_lifecycle_result_t boot_lifecycle_deadline_intent(const boot_metadata_t *record,
    const boot_deadline_request_t *request, const boot_verified_image_t *running,
    boot_metadata_t *out);
/* Exact resident identity, NOT evidence that an image executed or is valid. */
typedef struct
{
    uint32_t slot;
    uint8_t payload_hash[32];
    uint32_t release;
    uint32_t payload_length;
    uint32_t trial_id;
} boot_resident_identity_t;

typedef struct
{
    boot_resident_identity_t identity;
    uint32_t reason; /* one preclassified WATCHDOG..COMBINED, never raw RESC */
    uint32_t consume_intent; /* exactly 0 (retain) or 1 (canonical clear) */
} boot_counted_failure_t;

/* Authoritative durable unconfirmed TRIAL only; append one reason/count.
 * Third count -> REJECTED/rollback, clear candidate and its exact intent (codec
 * requires this), retain resident identity/history and all other roles.
 * Below threshold, intent is retained unless caller explicitly requests clear.
 * No image verification/execution evidence is needed for trial-wide policy.
 * This is NOT reset accounting: caller captures/clears/classifies hardware,
 * supplies the reason and applicable consumption decision. The reset package
 * MUST combine count and applicable consumption in this SAME proposal before
 * its one commit; never commit a retained intent and consume it in a later
 * transaction. Retain mode alone is an intermediate transformation, not a
 * completed reset handler. In particular reason does NOT infer SW/consumption:
 * COMBINED may or may not include a deadline. No uncounted/confirmed failures
 * are written by this helper (inapplicable requests fail, out unchanged).
 * No exactly-once guarantee: interrupted clear-before-commit can lose events
 * or retain stale intent; reload authority on UNCERTAIN, never blindly retry.
 * All common alias/error/next-sequence rules above apply. */
boot_lifecycle_result_t boot_lifecycle_apply_counted_failure(
    const boot_metadata_t *record, const boot_counted_failure_t *failure,
    boot_metadata_t *out);

typedef struct
{
    boot_resident_identity_t identity;
    boot_verify_result_t failure;
} boot_invalid_current_t;

/* CALLER provenance: independently boot_verify_stored current and observed an
 * image-invalid failure (FORMAT/TARGET/SIGNATURE/HASH/MSP/THUMB/VECTOR_RANGE),
 * not OK or an operational ARGUMENT/CRYPTO/READ/ABORTED failure. Identity names
 * the exact metadata resident checked even when stored manifest is corrupt.
 * previous is successful independent boot_verify_stored output, still stable
 * under exclusive image ownership. Forged C evidence cannot prove invalidity
 * or authenticate previous; this function only checks structure/identity.
 * Require current present + eligible confirmed previous; previous -> current,
 * previous absent, invalid confirmation/roles clear, both histories unchanged,
 * no new trial. In two slots current+previous leaves no candidate slot; current
 * invalid with READY/TRIAL candidate has no eligible previous: return error,
 * never invent candidate promotion. Neither valid/no previous returns error
 * for caller recovery/intervention, no guessed-role mutation or execution.
 * CHANGE requires separate boot_store_replace BOOT_STORE_OK full committed
 * readback before fallback execution; UNCERTAIN requires authority reload and
 * revalidation. This proposal performs no flash write or actual jump. */
boot_lifecycle_result_t boot_lifecycle_promote_previous(
    const boot_metadata_t *record, const boot_invalid_current_t *invalid,
    const boot_verified_image_t *previous, boot_metadata_t *out);
#endif
