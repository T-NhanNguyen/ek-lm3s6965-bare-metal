/* Codec/policy only; synthetic manifests are NOT signature acceptance proof.
 * Full selected-root package acceptance is a separate native CLI regression. */
#include <assert.h>
#include <string.h>
#include "fixture_metadata.h"

int main(void)
{
    boot_verified_image_t images[2] = {0};
    uint8_t page0[BOOT_ERASE_BYTES], page1[BOOT_ERASE_BYTES], staged[BOOT_ERASE_BYTES];
    for (unsigned i = 0; i < 2u; ++i) {
        images[i].manifest.slot = i;
        images[i].manifest.execution_base = i ? BOOT_SLOT_B_BASE : BOOT_SLOT_A_BASE;
        images[i].manifest.payload_length = BOOT_PAYLOAD_MIN + i * 4u;
        images[i].manifest.release = 100u + i;
        memset(images[i].manifest.payload_hash, (int)(i + 1u), 32u);
    }
    assert(fixture_metadata_pages(images, page0, page1));
    boot_metadata_t decoded, selected;
    unsigned page = 99u;
    assert(boot_metadata_decode(page0, sizeof page0, &decoded) == BOOT_OK);
    assert(boot_metadata_select(page0, sizeof page0, page1, sizeof page1,
                                &selected, &page) == BOOT_OK && page == 0u);
    assert(decoded.sequence == 1u && decoded.current_slot == BOOT_SLOT_A);
    assert(decoded.previous_slot == BOOT_SLOT_B && decoded.candidate_slot == BOOT_SLOT_NONE);
    assert(decoded.confirmed_mask == 3u && decoded.intent_kind == BOOT_INTENT_NONE);
    assert(decoded.intent_slot == BOOT_SLOT_NONE && decoded.intent_trial_id == 0u);
    for (unsigned i = 0; i < 2u; ++i) {
        assert(decoded.images[i].accounting == 0u && decoded.images[i].trial_id == 0u);
        assert(boot_metadata_image_matches(&decoded, &images[i].manifest) == BOOT_OK);
        boot_manifest_t stale = images[i].manifest;
        ++stale.release;
        assert(boot_metadata_image_matches(&decoded, &stale) == BOOT_ERROR_IDENTITY);
        stale = images[i].manifest;
        stale.payload_hash[0] ^= 1u;
        assert(boot_metadata_image_matches(&decoded, &stale) == BOOT_ERROR_IDENTITY);
    }
    for (unsigned i = 0; i < BOOT_ERASE_BYTES; ++i) {
        assert(page1[i] == 0xffu);
        if (i >= BOOT_METADATA_BYTES) assert(page0[i] == 0xffu);
    }
    /* Real staging encoder: exact first 164 bytes unchanged; only commit last. */
    memset(staged, 0xff, sizeof staged);
    assert(boot_metadata_encode(&decoded, BOOT_COMMIT_ERASED, staged, sizeof staged) == BOOT_OK);
    assert(memcmp(staged, page0, BOOT_METADATA_BYTES - BOOT_PROGRAM_BYTES) == 0);
    assert(boot_metadata_decode(staged, sizeof staged, &selected) == BOOT_ERROR_COMMIT);
    memcpy(staged + BOOT_METADATA_BYTES - BOOT_PROGRAM_BYTES,
           page0 + BOOT_METADATA_BYTES - BOOT_PROGRAM_BYTES, BOOT_PROGRAM_BYTES);
    assert(boot_metadata_decode(staged, sizeof staged, &selected) == BOOT_OK);
    page0[40] ^= 1u;
    assert(boot_metadata_decode(page0, sizeof page0, &selected) == BOOT_ERROR_CRC);
    assert(boot_metadata_select(page0, sizeof page0, page1, sizeof page1,
                                &selected, &page) == BOOT_ERROR_NO_RECORD);
    return 0;
}
