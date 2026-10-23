#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int overlaps(const void *a, size_t as, const void *b, size_t bs)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < as : x - y < bs;
}

/* STE: Compare canonical bytes and the selected storage representative. */
static int same_snapshot(const boot_store_snapshot_t *a,
                         const boot_store_snapshot_t *b)
{
    uint8_t aw[BOOT_METADATA_BYTES], bw[BOOT_METADATA_BYTES];
    return a->page == b->page &&
        boot_metadata_encode(&a->record, BOOT_COMMIT_VALID, aw, sizeof aw) == BOOT_OK &&
        boot_metadata_encode(&b->record, BOOT_COMMIT_VALID, bw, sizeof bw) == BOOT_OK &&
        memcmp(aw, bw, sizeof aw) == 0;
}

static int image_invalid(boot_verify_result_t r)
{
    return r == BOOT_VERIFY_ERROR_FORMAT || r == BOOT_VERIFY_ERROR_TARGET ||
        r == BOOT_VERIFY_ERROR_SIGNATURE || r == BOOT_VERIFY_ERROR_HASH ||
        r == BOOT_VERIFY_ERROR_MSP || r == BOOT_VERIFY_ERROR_THUMB ||
        r == BOOT_VERIFY_ERROR_VECTOR_RANGE;
}

static int confirmed(const boot_metadata_t *r, uint32_t slot)
{
    return slot <= BOOT_SLOT_B &&
        (r->confirmed_mask & (UINT32_C(1) << slot)) != 0u;
}

static boot_select_status_t store_status(boot_store_result_t r)
{
    if (r == BOOT_STORE_UNCERTAIN) return BOOT_SELECT_RECONCILE;
    if (r == BOOT_STORE_NO_RECORD || r == BOOT_STORE_CONFLICT)
        return BOOT_SELECT_RECOVERY;
    return BOOT_SELECT_STORE_FAILED;
}

static boot_verify_result_t verify(const boot_select_images_t *images,
    uint32_t slot, boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_verified_image_t *out)
{
    size_t size = images->size[slot];
    if (size > BOOT_MANIFEST_BYTES) size = BOOT_MANIFEST_BYTES;
    return boot_verify_stored(images->wire[slot], size, slot, reader,
        reader_context, progress, progress_context, out);
}

boot_select_result_t boot_select(const boot_store_adapter_t *store,
    const boot_store_snapshot_t *expected, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_launch_decision_t *out)
{
    boot_select_result_t result = {BOOT_SELECT_ARGUMENT, 0u,
        {BOOT_VERIFY_OK, BOOT_VERIFY_OK}, BOOT_LIFECYCLE_NO_CHANGE, BOOT_STORE_OK};
    boot_store_adapter_t adapter;
    boot_store_snapshot_t wanted, current;
    boot_select_images_t wires;
    boot_launch_decision_t decision;
    boot_metadata_t proposal;
    boot_invalid_current_t invalid;
    uint32_t slot, previous;
    if (store == NULL || expected == NULL || images == NULL || reader == NULL ||
        out == NULL || store->read == NULL || store->erase_page == NULL ||
        store->program_word == NULL ||
        overlaps(out, sizeof *out, store, sizeof *store) ||
        overlaps(out, sizeof *out, expected, sizeof *expected) ||
        overlaps(out, sizeof *out, images, sizeof *images)) return result;
    /* STE: No callback observes unsnapshotted policy/authority inputs. */
    adapter = *store;
    wanted = *expected;
    wires = *images;
    if (wanted.page > 1u || boot_metadata_validate(&wanted.record) != BOOT_OK)
    {
        result.store_result = BOOT_STORE_INVALID_RECORD;
        goto store_failed;
    }
    result.store_result = boot_store_load(&adapter, &current);
    if (result.store_result != BOOT_STORE_OK) goto store_failed;
    if (!same_snapshot(&wanted, &current))
    {
        result.store_result = BOOT_STORE_STALE;
        goto store_failed;
    }
    slot = current.record.candidate_slot;
    if (slot <= BOOT_SLOT_B)
    {
        boot_launch_result_t candidate;
        size_t size = wires.size[slot];
        if (size > BOOT_MANIFEST_BYTES) size = BOOT_MANIFEST_BYTES;
        candidate = boot_launch_candidate(&adapter, &current, wires.wire[slot],
            size, reader, reader_context, progress, progress_context, &decision);
        result.verify_result[slot] = candidate.verify_result;
        result.lifecycle_result = candidate.lifecycle_result;
        result.store_result = candidate.store_result;
        if (candidate.status == BOOT_LAUNCH_OK ||
            candidate.status == BOOT_LAUNCH_IMAGE_INVALID ||
            candidate.status == BOOT_LAUNCH_VERIFY_FAILED ||
            candidate.status == BOOT_LAUNCH_LIFECYCLE_FAILED)
            result.evidence_mask |= UINT32_C(1) << slot;
        if (candidate.status == BOOT_LAUNCH_OK) goto publish;
        if (candidate.status == BOOT_LAUNCH_STORE_FAILED ||
            candidate.status == BOOT_LAUNCH_RECONCILE) goto store_failed;
        if (candidate.status != BOOT_LAUNCH_IMAGE_INVALID)
        {
            result.status = candidate.status == BOOT_LAUNCH_VERIFY_FAILED ?
                BOOT_SELECT_VERIFY_FAILED : BOOT_SELECT_LIFECYCLE_FAILED;
            return result;
        }
    }
    slot = current.record.current_slot;
    if (!confirmed(&current.record, slot)) goto recovery;
    result.evidence_mask |= UINT32_C(1) << slot;
    result.verify_result[slot] = verify(&wires, slot, reader, reader_context,
        progress, progress_context, &decision.image);
    if (result.verify_result[slot] == BOOT_VERIFY_OK)
    {
        if (boot_metadata_image_matches(&current.record,
                &decision.image.manifest) != BOOT_OK)
        {
            result.status = BOOT_SELECT_IDENTITY_FAILED;
            return result;
        }
        /* STE: Zero-write selection still requires publication-time authority. */
        result.store_result = boot_store_load(&adapter, &decision.snapshot);
        if (result.store_result == BOOT_STORE_OK &&
            !same_snapshot(&current, &decision.snapshot))
            result.store_result = BOOT_STORE_STALE;
        if (result.store_result != BOOT_STORE_OK) goto store_failed;
        goto publish;
    }
    if (!image_invalid(result.verify_result[slot]))
    {
        result.status = BOOT_SELECT_VERIFY_FAILED;
        return result;
    }
    previous = current.record.previous_slot;
    if (!confirmed(&current.record, previous) || previous == slot ||
        previous == current.record.candidate_slot) goto recovery;
    result.evidence_mask |= UINT32_C(1) << previous;
    result.verify_result[previous] = verify(&wires, previous, reader, reader_context,
        progress, progress_context, &decision.image);
    if (result.verify_result[previous] != BOOT_VERIFY_OK)
    {
        if (image_invalid(result.verify_result[previous])) goto recovery;
        result.status = BOOT_SELECT_VERIFY_FAILED;
        return result;
    }
    if (boot_metadata_image_matches(&current.record,
            &decision.image.manifest) != BOOT_OK)
    {
        result.status = BOOT_SELECT_IDENTITY_FAILED;
        return result;
    }
    invalid.identity.slot = slot;
    memcpy(invalid.identity.payload_hash, current.record.images[slot].payload_hash,
        sizeof invalid.identity.payload_hash);
    invalid.identity.release = current.record.images[slot].release;
    invalid.identity.payload_length = current.record.images[slot].payload_length;
    invalid.identity.trial_id = current.record.images[slot].trial_id;
    invalid.failure = result.verify_result[slot];
    result.lifecycle_result = boot_lifecycle_promote_previous(&current.record,
        &invalid, &decision.image, &proposal);
    if (result.lifecycle_result != BOOT_LIFECYCLE_CHANGE)
    {
        result.status = BOOT_SELECT_LIFECYCLE_FAILED;
        return result;
    }
    result.store_result = boot_store_replace(&adapter, &current, &proposal,
        &decision.snapshot);
    if (result.store_result != BOOT_STORE_OK) goto store_failed;
publish:
    *out = decision;
    result.status = BOOT_SELECT_OK;
    return result;
recovery:
    result.status = BOOT_SELECT_RECOVERY;
    return result;
store_failed:
    result.status = store_status(result.store_result);
    return result;
}
