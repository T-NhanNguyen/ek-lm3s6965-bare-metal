#ifndef BOOT_FIXTURE_METADATA_H
#define BOOT_FIXTURE_METADATA_H
#include "lm3s6965/boot_verify.h"

/* Immutable exact transport package; real selected-root verification, including
 * full payload hash/vectors. No outputs on failure. Release is caller policy. */
int fixture_verify_package(const uint8_t *package, size_t length,
    uint32_t slot, uint32_t release, boot_verified_image_t *image);
/* Fixed factory policy: A current, B previous, both confirmed, sequence 1.
 * Inputs must be results of fixture_verify_package in this process. */
int fixture_metadata_pages(const boot_verified_image_t images[2],
    uint8_t page0[BOOT_ERASE_BYTES], uint8_t page1[BOOT_ERASE_BYTES]);
#endif
