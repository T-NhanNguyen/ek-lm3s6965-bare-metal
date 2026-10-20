#include "fixture_metadata.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *payload;
    size_t length;
    uint32_t base;
} package_reader_t;

static size_t read_package(void *context, uint32_t address,
                           uint8_t *buffer, size_t requested)
{
    const package_reader_t *r = context;
    if (address < r->base) return 0;
    size_t offset = address - r->base;
    if (offset > r->length || requested > r->length - offset) return 0;
    memcpy(buffer, r->payload + offset, requested);
    return requested;
}

int fixture_verify_package(const uint8_t *package, size_t length,
    uint32_t slot, uint32_t release, boot_verified_image_t *image)
{
    if (package == NULL || image == NULL || length < BOOT_MANIFEST_BYTES ||
        length > BOOT_PACKAGE_MAX || slot > BOOT_SLOT_B) return 0;
    package_reader_t reader = {package + BOOT_MANIFEST_BYTES,
        length - BOOT_MANIFEST_BYTES,
        slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE};
    boot_verified_image_t verified;
    boot_verify_result_t status = boot_verify_stored(package, BOOT_MANIFEST_BYTES,
        slot, read_package, &reader, NULL, NULL, &verified);
    if (status != BOOT_VERIFY_OK) {
        fprintf(stderr, "slot %c verification rejected: %u\n", slot ? 'B' : 'A',
                (unsigned)status);
        return 0;
    }
    if (verified.manifest.release != release ||
        verified.manifest.payload_length != reader.length) {
        fprintf(stderr, "slot %c release/length mismatch (expected release %lu)\n",
                slot ? 'B' : 'A', (unsigned long)release);
        return 0;
    }
    *image = verified;
    return 1;
}

int fixture_metadata_pages(const boot_verified_image_t images[2],
    uint8_t page0[BOOT_ERASE_BYTES], uint8_t page1[BOOT_ERASE_BYTES])
{
    boot_metadata_t metadata = {0}, decoded, selected;
    unsigned selected_page = 99u;
    metadata.sequence = BOOT_FACTORY_SEQUENCE;
    metadata.current_slot = BOOT_SLOT_A;
    metadata.previous_slot = BOOT_SLOT_B;
    metadata.candidate_slot = BOOT_SLOT_NONE;
    metadata.confirmed_mask = BOOT_CONFIRMED_A | BOOT_CONFIRMED_B;
    metadata.intent_slot = BOOT_SLOT_NONE;
    for (unsigned i = 0; i < 2u; ++i) {
        const boot_manifest_t *m = &images[i].manifest;
        if (m->slot != i || boot_manifest_validate(m) != BOOT_OK) return 0;
        memcpy(metadata.images[i].payload_hash, m->payload_hash, 32u);
        metadata.images[i].release = m->release;
        metadata.images[i].payload_length = m->payload_length;
    }
    memset(page0, 0xff, BOOT_ERASE_BYTES);
    memset(page1, 0xff, BOOT_ERASE_BYTES);
    if (boot_metadata_encode(&metadata, BOOT_COMMIT_VALID, page0,
                             BOOT_ERASE_BYTES) != BOOT_OK ||
        boot_metadata_decode(page0, BOOT_ERASE_BYTES, &decoded) != BOOT_OK ||
        boot_metadata_select(page0, BOOT_ERASE_BYTES, page1, BOOT_ERASE_BYTES,
                             &selected, &selected_page) != BOOT_OK ||
        selected_page != 0u || selected.sequence != BOOT_FACTORY_SEQUENCE)
        return 0;
    for (unsigned i = 0; i < 2u; ++i) {
        if (boot_metadata_image_matches(&decoded, &images[i].manifest) != BOOT_OK ||
            boot_metadata_image_matches(&selected, &images[i].manifest) != BOOT_OK)
            return 0;
    }
    return 1;
}
