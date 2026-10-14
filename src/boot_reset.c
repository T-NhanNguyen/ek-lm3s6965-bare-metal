#include "lm3s6965/boot_reset.h"
#include <string.h>

static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}
boot_lifecycle_result_t boot_reset_propose(const boot_metadata_t *record,
    uint32_t captured_flags, boot_metadata_t *out, boot_reset_decision_t *decision)
{
    boot_reset_decision_t d = {0};
    boot_metadata_t proposal;
    boot_counted_failure_t failure = {0};
    boot_lifecycle_result_t result;
    uint32_t slot, categories = 0u, matching;
    const boot_image_entry_t *entry;
    if (record == NULL || out == NULL || decision == NULL ||
        overlaps(out, sizeof *out, record, sizeof *record) ||
        overlaps(decision, sizeof *decision, record, sizeof *record) ||
        overlaps(out, sizeof *out, decision, sizeof *decision))
        return BOOT_LIFECYCLE_ERROR_ARGUMENT;
    if (boot_metadata_validate(record) != BOOT_OK)
        return BOOT_LIFECYCLE_ERROR_RECORD;
    d.supported_flags = captured_flags & BOOT_RESET_SUPPORTED;
    slot = record->candidate_slot;
    d.trial_applicable = slot <= BOOT_SLOT_B &&
        (record->confirmed_mask & (UINT32_C(1) << slot)) == 0u &&
        (record->images[slot].accounting & BOOT_ACCOUNT_STATE_MASK) == BOOT_STATE_TRIAL;
    if (d.trial_applicable != 0u)
    {
        entry = &record->images[slot];
        matching = record->intent_kind == BOOT_INTENT_DEADLINE &&
            record->intent_slot == slot && record->intent_trial_id == entry->trial_id;
        if ((d.supported_flags & BOOT_RESET_WATCHDOG) != 0u)
        { ++categories; d.reason = BOOT_REASON_WATCHDOG; }
        if ((d.supported_flags & BOOT_RESET_EXTERNAL) != 0u)
        { ++categories; d.reason = BOOT_REASON_EXTERNAL; }
        if ((d.supported_flags & BOOT_RESET_BROWNOUT) != 0u)
        { ++categories; d.reason = BOOT_REASON_BROWNOUT; }
        if ((d.supported_flags & BOOT_RESET_SW) != 0u && matching != 0u)
        { ++categories; d.reason = BOOT_REASON_DEADLINE; }
        if (categories != 0u)
        {
            d.counted = 1u;
            if (categories > 1u) d.reason = BOOT_REASON_COMBINED;
            d.consume_intent = matching;
            failure.identity.slot = slot;
            memcpy(failure.identity.payload_hash, entry->payload_hash, 32u);
            failure.identity.release = entry->release;
            failure.identity.payload_length = entry->payload_length;
            failure.identity.trial_id = entry->trial_id;
            failure.reason = d.reason;
            failure.consume_intent = d.consume_intent;
            result = boot_lifecycle_apply_counted_failure(record, &failure, &proposal);
            if (result != BOOT_LIFECYCLE_CHANGE) return result;
            *out = proposal;
            *decision = d;
            return result;
        }
    }
    *out = *record;
    *decision = d;
    return BOOT_LIFECYCLE_NO_CHANGE;
}
