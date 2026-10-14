/* Boot MVP v1 byte contracts. No hardware, allocation, hashing or signatures. */
#ifndef LM3S6965_BOOT_FORMAT_H
#define LM3S6965_BOOT_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define BOOT_FLASH_BASE UINT32_C(0)
#define BOOT_FLASH_END UINT32_C(0x40000)
#define BOOT_LOADER_END UINT32_C(0x08000)
#define BOOT_METADATA_PAGE0 UINT32_C(0x08000)
#define BOOT_METADATA_PAGE1 UINT32_C(0x08400)
#define BOOT_METADATA_END UINT32_C(0x08800)
#define BOOT_SLOT_A_BASE UINT32_C(0x08800)
#define BOOT_SLOT_A_MANIFEST UINT32_C(0x23c00)
#define BOOT_SLOT_A_END UINT32_C(0x24000)
#define BOOT_SLOT_B_BASE UINT32_C(0x24000)
#define BOOT_SLOT_B_MANIFEST UINT32_C(0x3f400)
#define BOOT_SLOT_B_END UINT32_C(0x3f800)
#define BOOT_SPARE_BASE BOOT_SLOT_B_END
#define BOOT_ERASE_BYTES 1024u
#define BOOT_PROGRAM_BYTES 4u
#define BOOT_PROTECTION_BYTES 2048u
#define BOOT_VECTOR_ALIGNMENT 256u
#define BOOT_SLOT_BYTES 112640u
#define BOOT_MANIFEST_RESERVE_BYTES 1024u
#define BOOT_PAYLOAD_MIN 236u
#define BOOT_PAYLOAD_MAX 111616u
#define BOOT_PACKAGE_MAX 111776u
#define BOOT_SRAM_BASE UINT32_C(0x20000000)
#define BOOT_SRAM_END UINT32_C(0x20010000)
#define BOOT_DIAGNOSTIC_BASE UINT32_C(0x2000dfc0)
#define BOOT_DIAGNOSTIC_BYTES 64u
#define BOOT_STACK_BASE UINT32_C(0x2000e000)
#define BOOT_STACK_BYTES 8192u
#define BOOT_INITIAL_MSP BOOT_SRAM_END

#define BOOT_FORMAT_VERSION 1u
#define BOOT_PRODUCT_ID 1u
#define BOOT_MANIFEST_BYTES 160u
#define BOOT_MANIFEST_SIGNED_BYTES 96u
#define BOOT_METADATA_BYTES 168u
#define BOOT_METADATA_CRC_BYTES 160u
#define BOOT_METADATA_MAGIC UINT32_C(0x314d4241)
#define BOOT_FACTORY_SEQUENCE 1u
#define BOOT_COMMIT_ERASED UINT32_MAX
#define BOOT_COMMIT_VALID UINT32_C(0)
#define BOOT_SLOT_A UINT32_C(0)
#define BOOT_SLOT_B UINT32_C(1)
#define BOOT_SLOT_NONE UINT32_MAX
#define BOOT_CONFIRMED_A UINT32_C(1)
#define BOOT_CONFIRMED_B UINT32_C(2)
#define BOOT_INTENT_NONE UINT32_C(0)
#define BOOT_INTENT_DEADLINE UINT32_C(1)
#define BOOT_STATE_NONE UINT32_C(0)
#define BOOT_STATE_READY UINT32_C(1)
#define BOOT_STATE_TRIAL UINT32_C(2)
#define BOOT_STATE_REJECTED UINT32_C(3)
#define BOOT_REASON_UNUSED 0u
#define BOOT_REASON_WATCHDOG 1u
#define BOOT_REASON_EXTERNAL 2u
#define BOOT_REASON_BROWNOUT 3u
#define BOOT_REASON_DEADLINE 4u
#define BOOT_REASON_COMBINED 5u
#define BOOT_ACCOUNT_STATE_MASK UINT32_C(3)
#define BOOT_ACCOUNT_COUNT_SHIFT 2u
#define BOOT_ACCOUNT_ROLLBACK UINT32_C(0x10)
#define BOOT_ACCOUNT_REASON1_SHIFT 5u
#define BOOT_ACCOUNT_REASON2_SHIFT 8u
#define BOOT_ACCOUNT_REASON3_SHIFT 11u
#define BOOT_ACCOUNT_RESERVED_MASK UINT32_C(0xffffc000)

/* Typed failures; selection returns recovery rather than an invalid page's
 * sequence. Numeric enum values are API-only, never wire encodings. */
typedef enum
{
    BOOT_OK = 0,
    BOOT_ERROR_ARGUMENT,
    BOOT_ERROR_SIZE,
    BOOT_ERROR_DOMAIN,
    BOOT_ERROR_VERSION,
    BOOT_ERROR_LENGTH,
    BOOT_ERROR_PRODUCT,
    BOOT_ERROR_SLOT,
    BOOT_ERROR_BASE,
    BOOT_ERROR_BOUNDS,
    BOOT_ERROR_RESERVED,
    BOOT_ERROR_MAGIC,
    BOOT_ERROR_COMMIT,
    BOOT_ERROR_CRC,
    BOOT_ERROR_ACCOUNTING,
    BOOT_ERROR_EMPTY,
    BOOT_ERROR_ROLE,
    BOOT_ERROR_INTENT,
    BOOT_ERROR_IDENTITY,
    BOOT_ERROR_MSP,
    BOOT_ERROR_THUMB,
    BOOT_ERROR_VECTOR_RANGE,
    BOOT_ERROR_NO_RECORD,
    BOOT_ERROR_CONFLICT,
    BOOT_ERROR_EXHAUSTED
} boot_result_t;

/* In-memory values only: NEVER memcpy these structs to/from the wire. */
typedef struct
{
    uint32_t slot;
    uint32_t execution_base;
    uint32_t payload_length;
    uint32_t release;
    uint8_t payload_hash[32];
    uint8_t signature[64]; /* Opaque; validity is a later crypto layer. */
} boot_manifest_t;

typedef struct
{
    uint8_t payload_hash[32];
    uint32_t release;
    uint32_t payload_length;
    uint32_t accounting;
    uint32_t trial_id;
} boot_image_entry_t;

typedef struct
{
    uint32_t sequence;
    uint32_t current_slot;
    uint32_t candidate_slot;
    uint32_t previous_slot;
    uint32_t confirmed_mask;
    boot_image_entry_t images[2];
    uint32_t intent_kind;
    uint32_t intent_slot;
    uint32_t intent_trial_id;
} boot_metadata_t;

/* All spans must cover their stated size; inputs/outputs must not overlap.
 * Decoders inspect only the fixed prefix (tails deliberately ignored), reject
 * unsupported versions before version-dependent fields, and publish output
 * only on success. Encoders validate first, write only the fixed prefix,
 * canonicalize fixed/reserved fields, and leave output unchanged on failure.
 * No function establishes signature/hash authenticity or reads flash. */
boot_result_t boot_manifest_decode(const uint8_t *wire, size_t size,
                                   boot_manifest_t *out);
boot_result_t boot_manifest_encode(const boot_manifest_t *manifest,
                                   uint8_t *wire, size_t capacity);
boot_result_t boot_manifest_validate(const boot_manifest_t *manifest);
/* Overflow-safe geometry; end is exclusive, published only on success. */
boot_result_t boot_image_span(uint32_t slot, uint32_t base, uint32_t length,
                              uint32_t *end);
/* Geometry is checked BEFORE reading vectors. Supply at least payload_length
 * readable bytes; only first eight are inspected. Crypto must precede use. */
boot_result_t boot_vectors_validate(const boot_manifest_t *manifest,
                                    const uint8_t *payload, size_t size);

/* ISO-HDLC one-shot; NULL allowed only for size 0. */
uint32_t boot_crc32(const uint8_t *bytes, size_t size);
boot_result_t boot_metadata_validate(const boot_metadata_t *metadata);
boot_result_t boot_metadata_decode(const uint8_t *wire, size_t size,
                                   boot_metadata_t *out);
/* commit must be ERASED (staging) or VALID (fixture/readback); flash layer must
 * program VALID last. Decode accepts ONLY VALID. CRC excludes commit. */
boot_result_t boot_metadata_encode(const boot_metadata_t *metadata,
                                   uint32_t commit, uint8_t *wire,
                                   size_t capacity);
/* Each page is validated independently. Equal identical prefixes select page
 * 0 as a storage representative, NOT an image-role decision. Invalid pages
 * never participate in ranking. out/page are unchanged on recovery/failure. */
boot_result_t boot_metadata_select(const uint8_t *page0, size_t size0,
                                   const uint8_t *page1, size_t size1,
                                   boot_metadata_t *out, unsigned *page);
boot_result_t boot_metadata_next_sequence(const boot_metadata_t *metadata,
                                          uint32_t *next);
/* Separate identity gate after manifest AND stored payload authentication. */
boot_result_t boot_metadata_image_matches(const boot_metadata_t *metadata,
                                          const boot_manifest_t *manifest);

#endif
