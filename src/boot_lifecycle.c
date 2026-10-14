#include "lm3s6965/boot_lifecycle.h"
#include <string.h>

/* Subtraction avoids pointer-end overflow; no relational comparison of
 * unrelated C pointers. Valid complete objects are required by the API. */
static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}
static int manifest_equal(const boot_manifest_t *a, const boot_manifest_t *b)
{
    return a->slot == b->slot && a->execution_base == b->execution_base &&
        a->payload_length == b->payload_length && a->release == b->release &&
        memcmp(a->payload_hash, b->payload_hash, 32u) == 0 &&
        memcmp(a->signature, b->signature, 64u) == 0;
}
static int image_admissible(const boot_verified_image_t *image)
{
    uint32_t end;
    return boot_manifest_validate(&image->manifest) == BOOT_OK &&
        boot_image_span(image->manifest.slot, image->manifest.execution_base,
                        image->manifest.payload_length, &end) == BOOT_OK &&
        image->initial_msp == BOOT_INITIAL_MSP &&
        (image->reset_vector & 1u) != 0u &&
        (image->reset_vector & ~UINT32_C(1)) >= image->manifest.execution_base &&
        (image->reset_vector & ~UINT32_C(1)) < end;
}
static boot_lifecycle_result_t admission(const boot_metadata_t *r,
    const boot_upload_evidence_t *e, boot_metadata_t *out)
{
    if (r == NULL || e == NULL || out == NULL ||
        overlaps(out, sizeof *out, r, sizeof *r) ||
        overlaps(out, sizeof *out, e, sizeof *e))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    if (boot_metadata_validate(r) != BOOT_OK)
        return BOOT_LIFECYCLE_ERROR_RECORD;
    if (boot_manifest_validate(&e->incoming) != BOOT_OK ||
        !image_admissible(&e->running))
        return BOOT_LIFECYCLE_ERROR_IMAGE;
    if (e->permitted_slot > BOOT_SLOT_B ||
        e->incoming.slot != e->permitted_slot ||
        r->current_slot != e->running.manifest.slot ||
        e->permitted_slot == r->current_slot ||
        r->candidate_slot != BOOT_SLOT_NONE ||
        (r->confirmed_mask & (UINT32_C(1) << e->running.manifest.slot)) == 0u)
        return BOOT_LIFECYCLE_ERROR_ADMISSION;
    if (boot_metadata_image_matches(r, &e->running.manifest) != BOOT_OK)
        return BOOT_LIFECYCLE_ERROR_IDENTITY;
    return BOOT_LIFECYCLE_CHANGE;
}
static boot_lifecycle_result_t finish(const boot_metadata_t *r,
    boot_metadata_t *proposal, boot_metadata_t *out)
{
    boot_result_t result = boot_metadata_next_sequence(r, &proposal->sequence);
    if (result == BOOT_ERROR_EXHAUSTED)
        return BOOT_LIFECYCLE_ERROR_EXHAUSTED;
    if (result != BOOT_OK || boot_metadata_validate(proposal) != BOOT_OK)
        return BOOT_LIFECYCLE_ERROR_RECORD;
    *out = *proposal;
    return BOOT_LIFECYCLE_CHANGE;
}
boot_lifecycle_result_t boot_lifecycle_invalidate(const boot_metadata_t *record,
    const boot_upload_evidence_t *evidence, boot_metadata_t *out)
{
    boot_metadata_t proposal;
    uint32_t slot;
    boot_lifecycle_result_t result = admission(record, evidence, out);
    if (result != BOOT_LIFECYCLE_CHANGE)
        return result;
    slot = evidence->permitted_slot;
    proposal = *record;
    memset(&proposal.images[slot], 0, sizeof proposal.images[slot]);
    if (proposal.previous_slot == slot) proposal.previous_slot = BOOT_SLOT_NONE;
    /* Admission rules exclude destination current/candidate, but spell out
     * removal of destination roles rather than assigning any new roles. */
    if (proposal.current_slot == slot) proposal.current_slot = BOOT_SLOT_NONE;
    if (proposal.candidate_slot == slot) proposal.candidate_slot = BOOT_SLOT_NONE;
    proposal.confirmed_mask &= ~(UINT32_C(1) << slot);
    return finish(record, &proposal, out);
}
boot_lifecycle_result_t boot_lifecycle_accept_ready(const boot_metadata_t *record,
    const boot_upload_evidence_t *evidence, const boot_verified_image_t *stored,
    boot_metadata_t *out)
{
    boot_metadata_t proposal;
    boot_image_entry_t *entry;
    uint32_t slot, next;
    boot_lifecycle_result_t result;
    if (stored == NULL || out == NULL ||
        overlaps(out, sizeof *out, stored, sizeof *stored))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    result = admission(record, evidence, out);
    if (result != BOOT_LIFECYCLE_CHANGE) return result;
    slot = evidence->permitted_slot;
    /* Codec validation already proves all 48 entry bytes are canonical zero
     * and there are no role/confirmation references when length is zero. */
    if (record->images[slot].payload_length != 0u)
        return BOOT_LIFECYCLE_ERROR_ADMISSION;
    if (!image_admissible(stored)) return BOOT_LIFECYCLE_ERROR_IMAGE;
    if (!manifest_equal(&stored->manifest, &evidence->incoming))
        return BOOT_LIFECYCLE_ERROR_IDENTITY;
    if (boot_metadata_next_sequence(record, &next) == BOOT_ERROR_EXHAUSTED)
        return BOOT_LIFECYCLE_ERROR_EXHAUSTED;
    proposal = *record;
    proposal.candidate_slot = slot;
    entry = &proposal.images[slot];
    memcpy(entry->payload_hash, stored->manifest.payload_hash, 32u);
    entry->release = stored->manifest.release;
    entry->payload_length = stored->manifest.payload_length;
    entry->accounting = BOOT_STATE_READY;
    entry->trial_id = next;
    return finish(record, &proposal, out);
}

static boot_lifecycle_result_t execution(const boot_metadata_t *r,
    const boot_verified_image_t *image, boot_metadata_t *out)
{
    if (r == NULL || image == NULL || out == NULL ||
        overlaps(out, sizeof *out, r, sizeof *r) ||
        overlaps(out, sizeof *out, image, sizeof *image))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    if (boot_metadata_validate(r) != BOOT_OK)
        return BOOT_LIFECYCLE_ERROR_RECORD;
    if (!image_admissible(image)) return BOOT_LIFECYCLE_ERROR_IMAGE;
    if (boot_metadata_image_matches(r, &image->manifest) != BOOT_OK)
        return BOOT_LIFECYCLE_ERROR_IDENTITY;
    return BOOT_LIFECYCLE_CHANGE;
}
static boot_lifecycle_result_t unchanged(const boot_metadata_t *r,
    boot_metadata_t *out)
{
    *out = *r;
    return BOOT_LIFECYCLE_NO_CHANGE;
}
boot_lifecycle_result_t boot_lifecycle_begin_trial(const boot_metadata_t *record,
    const boot_verified_image_t *candidate, boot_metadata_t *out)
{
    boot_metadata_t proposal;
    uint32_t slot;
    boot_lifecycle_result_t result = execution(record, candidate, out);
    if (result != BOOT_LIFECYCLE_CHANGE) return result;
    slot = candidate->manifest.slot;
    if (record->candidate_slot != slot) return BOOT_LIFECYCLE_ERROR_ADMISSION;
    if ((record->images[slot].accounting & BOOT_ACCOUNT_STATE_MASK) == BOOT_STATE_TRIAL)
        return unchanged(record, out);
    proposal = *record;
    proposal.images[slot].accounting =
        (proposal.images[slot].accounting & ~BOOT_ACCOUNT_STATE_MASK) | BOOT_STATE_TRIAL;
    return finish(record, &proposal, out);
}
boot_lifecycle_result_t boot_lifecycle_confirm(const boot_metadata_t *record,
    const boot_confirm_request_t *request, const boot_verified_image_t *running,
    uint32_t health, boot_metadata_t *out)
{
    boot_metadata_t proposal;
    uint32_t slot;
    boot_lifecycle_result_t result;
    if (request == NULL || out == NULL ||
        overlaps(out, sizeof *out, request, sizeof *request))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    result = execution(record, running, out);
    if (result != BOOT_LIFECYCLE_CHANGE) return result;
    slot = running->manifest.slot;
    if (request->slot != slot || request->trial_id != record->images[slot].trial_id ||
        memcmp(request->payload_hash, running->manifest.payload_hash, 32u) != 0)
        return BOOT_LIFECYCLE_ERROR_IDENTITY;
    if ((record->confirmed_mask & (UINT32_C(1) << slot)) != 0u)
        return unchanged(record, out);
    if (record->candidate_slot != slot ||
        (record->images[slot].accounting & BOOT_ACCOUNT_STATE_MASK) != BOOT_STATE_TRIAL)
        return BOOT_LIFECYCLE_ERROR_ADMISSION;
    if (health != BOOT_HEALTH_REQUIRED) return BOOT_LIFECYCLE_ERROR_HEALTH;
    proposal = *record;
    proposal.previous_slot = record->current_slot;
    proposal.current_slot = slot;
    proposal.candidate_slot = BOOT_SLOT_NONE;
    proposal.confirmed_mask |= UINT32_C(1) << slot;
    proposal.images[slot].accounting &= ~BOOT_ACCOUNT_STATE_MASK;
    proposal.intent_kind = BOOT_INTENT_NONE;
    proposal.intent_slot = BOOT_SLOT_NONE;
    proposal.intent_trial_id = 0u;
    return finish(record, &proposal, out);
}
static void clear_intent(boot_metadata_t *r)
{
    r->intent_kind = BOOT_INTENT_NONE;
    r->intent_slot = BOOT_SLOT_NONE;
    r->intent_trial_id = 0u;
}
static int resident_matches(const boot_metadata_t *r,
    const boot_resident_identity_t *id)
{
    const boot_image_entry_t *entry;
    if (id->slot > BOOT_SLOT_B) return 0;
    entry = &r->images[id->slot];
    return entry->payload_length != 0u &&
        entry->payload_length == id->payload_length && entry->release == id->release &&
        entry->trial_id == id->trial_id &&
        memcmp(entry->payload_hash, id->payload_hash, 32u) == 0;
}
boot_lifecycle_result_t boot_lifecycle_apply_counted_failure(
    const boot_metadata_t *record, const boot_counted_failure_t *failure,
    boot_metadata_t *out)
{
    boot_metadata_t proposal;
    uint32_t slot, count, accounting;
    if (record == NULL || failure == NULL || out == NULL ||
        overlaps(out, sizeof *out, record, sizeof *record) ||
        overlaps(out, sizeof *out, failure, sizeof *failure))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    if (boot_metadata_validate(record) != BOOT_OK)
        return BOOT_LIFECYCLE_ERROR_RECORD;
    if (failure->reason < BOOT_REASON_WATCHDOG ||
        failure->reason > BOOT_REASON_COMBINED || failure->consume_intent > 1u)
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    if (!resident_matches(record, &failure->identity))
        return BOOT_LIFECYCLE_ERROR_IDENTITY;
    slot = failure->identity.slot;
    accounting = record->images[slot].accounting;
    if (record->candidate_slot != slot ||
        (record->confirmed_mask & (UINT32_C(1) << slot)) != 0u ||
        (accounting & BOOT_ACCOUNT_STATE_MASK) != BOOT_STATE_TRIAL)
        return BOOT_LIFECYCLE_ERROR_ADMISSION;
    if (record->sequence == UINT32_MAX)
        return BOOT_LIFECYCLE_ERROR_EXHAUSTED;
    count = (accounting >> BOOT_ACCOUNT_COUNT_SHIFT) & 3u;
    proposal = *record;
    accounting = (accounting & ~(UINT32_C(3) << BOOT_ACCOUNT_COUNT_SHIFT)) |
        ((count + 1u) << BOOT_ACCOUNT_COUNT_SHIFT) |
        (failure->reason << (BOOT_ACCOUNT_REASON1_SHIFT + 3u * count));
    if (count == 2u)
    {
        accounting = (accounting & ~BOOT_ACCOUNT_STATE_MASK) |
            BOOT_STATE_REJECTED | BOOT_ACCOUNT_ROLLBACK;
        proposal.candidate_slot = BOOT_SLOT_NONE;
        clear_intent(&proposal);
    }
    else if (failure->consume_intent != 0u) clear_intent(&proposal);
    proposal.images[slot].accounting = accounting;
    return finish(record, &proposal, out);
}
static int invalid_image_failure(boot_verify_result_t failure)
{
    switch (failure)
    {
    case BOOT_VERIFY_ERROR_FORMAT:
    case BOOT_VERIFY_ERROR_TARGET:
    case BOOT_VERIFY_ERROR_SIGNATURE:
    case BOOT_VERIFY_ERROR_HASH:
    case BOOT_VERIFY_ERROR_MSP:
    case BOOT_VERIFY_ERROR_THUMB:
    case BOOT_VERIFY_ERROR_VECTOR_RANGE: return 1;
    default: return 0;
    }
}
boot_lifecycle_result_t boot_lifecycle_promote_previous(
    const boot_metadata_t *record, const boot_invalid_current_t *invalid,
    const boot_verified_image_t *previous, boot_metadata_t *out)
{
    boot_metadata_t proposal;
    boot_lifecycle_result_t result;
    if (invalid == NULL || out == NULL ||
        overlaps(out, sizeof *out, invalid, sizeof *invalid))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    result = execution(record, previous, out);
    if (result != BOOT_LIFECYCLE_CHANGE) return result;
    if (!invalid_image_failure(invalid->failure))
        return BOOT_LIFECYCLE_ERROR_IMAGE;
    if (!resident_matches(record, &invalid->identity) ||
        record->current_slot != invalid->identity.slot)
        return BOOT_LIFECYCLE_ERROR_IDENTITY;
    if (record->previous_slot != previous->manifest.slot ||
        record->candidate_slot != BOOT_SLOT_NONE)
        return BOOT_LIFECYCLE_ERROR_ADMISSION;
    if (record->sequence == UINT32_MAX)
        return BOOT_LIFECYCLE_ERROR_EXHAUSTED;
    proposal = *record;
    proposal.current_slot = record->previous_slot;
    proposal.previous_slot = BOOT_SLOT_NONE;
    proposal.confirmed_mask &= ~(UINT32_C(1) << invalid->identity.slot);
    return finish(record, &proposal, out);
}
boot_lifecycle_result_t boot_lifecycle_deadline_intent(const boot_metadata_t *record,
    const boot_deadline_request_t *request, const boot_verified_image_t *running,
    boot_metadata_t *out)
{
    boot_metadata_t proposal;
    uint32_t slot;
    boot_lifecycle_result_t result;
    if (request == NULL || out == NULL ||
        overlaps(out, sizeof *out, request, sizeof *request))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    result = execution(record, running, out);
    if (result != BOOT_LIFECYCLE_CHANGE) return result;
    slot = running->manifest.slot;
    if (request->slot != slot || request->trial_id != record->images[slot].trial_id ||
        request->reason != BOOT_REASON_DEADLINE)
        return BOOT_LIFECYCLE_ERROR_IDENTITY;
    if (record->candidate_slot != slot ||
        (record->images[slot].accounting & BOOT_ACCOUNT_STATE_MASK) != BOOT_STATE_TRIAL)
        return BOOT_LIFECYCLE_ERROR_ADMISSION;
    if (record->intent_kind == BOOT_INTENT_DEADLINE && record->intent_slot == slot &&
        record->intent_trial_id == request->trial_id)
        return unchanged(record, out);
    proposal = *record;
    proposal.intent_kind = BOOT_INTENT_DEADLINE;
    proposal.intent_slot = slot;
    proposal.intent_trial_id = request->trial_id;
    return finish(record, &proposal, out);
}
