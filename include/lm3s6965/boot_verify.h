/* Offline signed-image verification; no flash, lifecycle or watchdog policy. */
#ifndef LM3S6965_BOOT_VERIFY_H
#define LM3S6965_BOOT_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_format.h"

#define BOOT_VERIFY_CHUNK_BYTES 256u

typedef enum
{
    BOOT_VERIFY_OK = 0,
    BOOT_VERIFY_ERROR_ARGUMENT,
    BOOT_VERIFY_ERROR_FORMAT,
    BOOT_VERIFY_ERROR_TARGET,
    BOOT_VERIFY_ERROR_SIGNATURE,
    BOOT_VERIFY_ERROR_CRYPTO,
    BOOT_VERIFY_ERROR_READ,
    BOOT_VERIFY_ERROR_ABORTED,
    BOOT_VERIFY_ERROR_HASH,
    BOOT_VERIFY_ERROR_MSP,
    BOOT_VERIFY_ERROR_THUMB,
    BOOT_VERIFY_ERROR_VECTOR_RANGE
} boot_verify_result_t;

typedef enum
{
    BOOT_VERIFY_MANIFEST_BEGIN,
    BOOT_VERIFY_MANIFEST_AUTHENTICATED,
    BOOT_VERIFY_PAYLOAD_PROGRESS
} boot_verify_phase_t;

/* Return nonzero to continue, zero to cancel. Optional (NULL).
 * BEGIN follows structural/physical-target checks, before crypto.
 * AUTHENTICATED follows signature verification, before any payload read.
 * PAYLOAD_PROGRESS follows each successfully read+hashed chunk (including last).
 * completed is 0 for manifest phases, otherwise exact bytes hashed; total is
 * the checked declared length. No callback inside the non-restartable ECDSA
 * operation. These events enable later foreground scheduling, NOT permission
 * to blindly feed a watchdog or a target latency guarantee. */
typedef int (*boot_verify_progress_fn)(void *context, boot_verify_phase_t phase,
                                       uint32_t completed, uint32_t total);

/* Return bytes copied (exactly requested for success). Short/zero/oversize
 * results fail immediately; no retries. Requests are absolute physical payload
 * addresses, sequential, <=256 bytes, confined to the authenticated interval.
 * The reader must not write beyond requested or mutate the stored image.
 * Caller must prevent concurrent storage changes throughout verification and
 * between success and execution/use. No library can establish that lock. */
typedef size_t (*boot_verify_read_fn)(void *context, uint32_t address,
                                     uint8_t *buffer, size_t requested);

typedef struct
{
    boot_manifest_t manifest;
    uint32_t initial_msp;
    uint32_t reset_vector; /* Original Thumb vector, not cleared target. */
} boot_verified_image_t;

/* All inputs must cover stated spans; output must not overlap inputs/contexts.
 * Only first 160 bytes are inspected; reserve tails are deliberately ignored.
 * expected_slot is trusted physical slot/permitted destination, A or B.
 * One immutable build-selected SEC1 P-256 PUBLIC trust root; no runtime keys.
 * Default builds use explicitly NONPRODUCTION demo trust. Production requires
 * LM3S6965_BOOT_PHYSICAL_PRODUCTION=ON and an explicit
 * LM3S6965_BOOT_PUBLIC_KEY_FILE (exactly 65 raw uncompressed SEC1 PUBLIC bytes).
 * Production requires an out-of-source build directory; generated PUBLIC
 * material stays there. CMake rejects missing/malformed/off-curve/demo roots
 * and prints the selected public SHA256 fingerprint; provenance and
 * authorization remain external.
 * Consumers link boot_crypto (which exports lm3s6965_boot_trust_root), or apply
 * that root target's usage requirements when supplying crypto separately.
 * Production compilation without its generated public header fails closed;
 * it never includes/falls back to demo trust. Root selection does not enable
 * physical startup or establish deployment permission, protected boot closure,
 * board/clock identity or authoritative metadata: those remain prerequisites.
 * Outputs unchanged on any failure (including cancellation); callbacks must
 * not modify output.
 * FORMAT includes decoder domain/version/size/geometry/reserved errors.
 * CRYPTO means library/resource failure, not a valid signature mismatch.
 * No anti-downgrade, metadata trust, execution, or durability claim. */
boot_verify_result_t boot_verify_manifest(const uint8_t *wire, size_t size,
    uint32_t expected_slot, boot_verify_progress_fn progress, void *context,
    boot_manifest_t *out);

/* Always re-authenticates the manifest; NEVER accepts a decoded/admitted struct
 * as proof. Admission alone says nothing about stored payload/hash/vectors.
 * Hashes every exact declared byte, captures vectors from that same stream,
 * compares payload SHA256, then checks exact MSP and authenticated Thumb range.
 * Reader is never invoked for failed structure/target/signature admission. */
boot_verify_result_t boot_verify_stored(const uint8_t *wire, size_t size,
    uint32_t expected_slot, boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_verified_image_t *out);

#endif
