#include "lm3s6965/boot_launch.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}

/* Compare encoded records, not C padding. Include the selected representative. */
static int same_snapshot(const boot_store_snapshot_t *a,
                         const boot_store_snapshot_t *b)
{
    uint8_t aw[BOOT_METADATA_BYTES], bw[BOOT_METADATA_BYTES];
    return a->page == b->page &&
        boot_metadata_encode(&a->record, BOOT_COMMIT_VALID, aw, sizeof aw) == BOOT_OK &&
        boot_metadata_encode(&b->record, BOOT_COMMIT_VALID, bw, sizeof bw) == BOOT_OK &&
        memcmp(aw, bw, sizeof aw) == 0;
}

static int image_invalid(boot_verify_result_t result)
{
    return result == BOOT_VERIFY_ERROR_FORMAT || result == BOOT_VERIFY_ERROR_TARGET ||
        result == BOOT_VERIFY_ERROR_SIGNATURE || result == BOOT_VERIFY_ERROR_HASH ||
        result == BOOT_VERIFY_ERROR_MSP || result == BOOT_VERIFY_ERROR_THUMB ||
        result == BOOT_VERIFY_ERROR_VECTOR_RANGE;
}

boot_launch_result_t boot_launch_candidate(const boot_store_adapter_t *store,
    const boot_store_snapshot_t *expected, const uint8_t *wire, size_t size,
    boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_launch_decision_t *out)
{
    boot_launch_result_t result = {BOOT_LAUNCH_ARGUMENT, BOOT_VERIFY_OK,
        BOOT_LIFECYCLE_NO_CHANGE, BOOT_STORE_OK};
    boot_launch_decision_t decision;
    boot_store_snapshot_t current;
    boot_metadata_t proposal;
    uint32_t slot, state;
    if (store == NULL || expected == NULL || wire == NULL || reader == NULL ||
        out == NULL || store->read == NULL || store->erase_page == NULL ||
        store->program_word == NULL ||
        overlaps(out, sizeof *out, store, sizeof *store) ||
        overlaps(out, sizeof *out, expected, sizeof *expected) ||
        overlaps(out, sizeof *out, wire, size)) return result;
    if (expected->page > 1u || boot_metadata_validate(&expected->record) != BOOT_OK)
    {
        result.status = BOOT_LAUNCH_STORE_FAILED;
        result.store_result = BOOT_STORE_INVALID_RECORD;
        return result;
    }
    result.store_result = boot_store_load(store, &current);
    if (result.store_result != BOOT_STORE_OK) goto store_failed;
    if (!same_snapshot(expected, &current))
    {
        result.store_result = BOOT_STORE_STALE;
        goto store_failed;
    }
    slot = current.record.candidate_slot;
    if (slot > BOOT_SLOT_B)
    {
        result.status = BOOT_LAUNCH_INELIGIBLE;
        return result;
    }
    state = current.record.images[slot].accounting & BOOT_ACCOUNT_STATE_MASK;
    if (state != BOOT_STATE_READY && state != BOOT_STATE_TRIAL)
    {
        result.status = BOOT_LAUNCH_INELIGIBLE;
        return result;
    }
    result.verify_result = boot_verify_stored(wire, size, slot, reader,
        reader_context, progress, progress_context, &decision.image);
    if (result.verify_result != BOOT_VERIFY_OK)
    {
        result.status = image_invalid(result.verify_result) ?
            BOOT_LAUNCH_IMAGE_INVALID : BOOT_LAUNCH_VERIFY_FAILED;
        return result;
    }
    result.lifecycle_result = boot_lifecycle_begin_trial(&current.record,
        &decision.image, &proposal);
    if (result.lifecycle_result == BOOT_LIFECYCLE_CHANGE)
        result.store_result = boot_store_replace(store, &current, &proposal,
                                                 &decision.snapshot);
    else if (result.lifecycle_result == BOOT_LIFECYCLE_NO_CHANGE)
    {
        /* Retry must not publish cached authority, even though it writes none. */
        result.store_result = boot_store_load(store, &decision.snapshot);
        if (result.store_result == BOOT_STORE_OK &&
            !same_snapshot(&current, &decision.snapshot))
            result.store_result = BOOT_STORE_STALE;
    }
    else
    {
        result.status = BOOT_LAUNCH_LIFECYCLE_FAILED;
        return result;
    }
    if (result.store_result != BOOT_STORE_OK) goto store_failed;
    *out = decision;
    result.status = BOOT_LAUNCH_OK;
    return result;

store_failed:
    result.status = result.store_result == BOOT_STORE_UNCERTAIN ?
        BOOT_LAUNCH_RECONCILE : BOOT_LAUNCH_STORE_FAILED;
    return result;
}
