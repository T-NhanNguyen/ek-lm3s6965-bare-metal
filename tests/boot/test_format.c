#include "lm3s6965/boot_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); \
    exit(EXIT_FAILURE); } } while (0)

/* Independently specified wire bytes; CRC cross-checked with Python zlib.
 * These are format fixtures, NOT cryptographically valid signed images. */
static const uint8_t golden_manifest[160] = {
    0x4c,0x4d,0x33,0x53,0x36,0x39,0x36,0x35,0x2d,0x46,0x57,0x2d,0x56,0x31,0,0,
    1,0,0,0, 0xa0,0,0,0, 1,0,0,0, 0,0,0,0,
    0,0x88,0,0, 0xec,0,0,0, 0x78,0x56,0x34,0x12, 0,0,0,0,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x8b,0x8c,0x8d,0x8e,0x8f,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,
    0xa0,0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,
    0xb0,0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf
};
static const uint8_t golden_metadata[168] = {
    0x41,0x42,0x4d,0x31, 1,0,0,0, 0xa8,0,0,0, 1,0,0,0,
    0,0,0,0, 0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 1,0,0,0,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    0x78,0x56,0x34,0x12, 0xec,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0, 0xff,0xff,0xff,0xff, 0,0,0,0, 0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0x93,0xeb,0xf4,0x23, 0,0,0,0
};

static void word(uint8_t *bytes, size_t offset, uint32_t value)
{
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1u] = (uint8_t)(value >> 8);
    bytes[offset + 2u] = (uint8_t)(value >> 16);
    bytes[offset + 3u] = (uint8_t)(value >> 24);
}

static boot_manifest_t manifest_fixture(void)
{
    boot_manifest_t m = {0};
    unsigned i;
    m.slot = BOOT_SLOT_A;
    m.execution_base = BOOT_SLOT_A_BASE;
    m.payload_length = BOOT_PAYLOAD_MIN;
    m.release = UINT32_C(0x12345678);
    for (i = 0; i < 32u; ++i)
        m.payload_hash[i] = (uint8_t)i;
    for (i = 0; i < 64u; ++i)
        m.signature[i] = (uint8_t)(0x80u + i);
    return m;
}

static boot_metadata_t metadata_fixture(void)
{
    boot_metadata_t m = {0};
    boot_manifest_t image = manifest_fixture();
    m.sequence = BOOT_FACTORY_SEQUENCE;
    m.current_slot = BOOT_SLOT_A;
    m.candidate_slot = BOOT_SLOT_NONE;
    m.previous_slot = BOOT_SLOT_NONE;
    m.confirmed_mask = BOOT_CONFIRMED_A;
    memcpy(m.images[0].payload_hash, image.payload_hash, 32);
    m.images[0].release = image.release;
    m.images[0].payload_length = image.payload_length;
    m.intent_slot = BOOT_SLOT_NONE;
    return m;
}

static void test_golden(void)
{
    uint8_t wire[1025];
    boot_manifest_t image = manifest_fixture();
    boot_manifest_t decoded;
    boot_metadata_t metadata = metadata_fixture();
    boot_metadata_t decoded_metadata;
    memset(wire, 0xa5, sizeof wire);
    CHECK(boot_manifest_encode(&image, wire + 1, 1024) == BOOT_OK);
    CHECK(memcmp(wire + 1, golden_manifest, 160) == 0);
    CHECK(wire[0] == 0xa5 && wire[161] == 0xa5);
    CHECK(boot_manifest_decode(wire + 1, 1024, &decoded) == BOOT_OK);
    CHECK(decoded.release == UINT32_C(0x12345678));
    CHECK(memcmp(decoded.signature, image.signature, 64) == 0);
    CHECK(boot_manifest_encode(&decoded, wire + 1, 160) == BOOT_OK);
    CHECK(memcmp(wire + 1, golden_manifest, 160) == 0);
    CHECK(boot_metadata_encode(&metadata, BOOT_COMMIT_VALID, wire + 1, 1024) == BOOT_OK);
    CHECK(memcmp(wire + 1, golden_metadata, 168) == 0);
    CHECK(wire[0] == 0xa5 && wire[169] == 0xa5);
    CHECK(boot_metadata_decode(wire + 1, 1024, &decoded_metadata) == BOOT_OK);
    CHECK(boot_metadata_encode(&decoded_metadata, BOOT_COMMIT_VALID, wire + 1, 168) == BOOT_OK);
    CHECK(memcmp(wire + 1, golden_metadata, 168) == 0);
    CHECK(boot_crc32((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    CHECK(boot_crc32(NULL, 0) == 0u);
    word(wire, 164, BOOT_COMMIT_ERASED);
    CHECK(boot_metadata_encode(&metadata, BOOT_COMMIT_ERASED, wire, 168) == BOOT_OK);
    CHECK(boot_metadata_decode(wire, 168, &decoded_metadata) == BOOT_ERROR_COMMIT);
    CHECK(boot_crc32(golden_metadata, 160) == UINT32_C(0x23f4eb93));
    CHECK(boot_metadata_image_matches(&metadata, &image) == BOOT_OK);
    image.release++;
    CHECK(boot_metadata_image_matches(&metadata, &image) == BOOT_ERROR_IDENTITY);
    image = manifest_fixture();
    image.payload_hash[0] ^= 1u;
    CHECK(boot_metadata_image_matches(&metadata, &image) == BOOT_ERROR_IDENTITY);
    image = manifest_fixture();
    image.payload_length += 4u;
    CHECK(boot_metadata_image_matches(&metadata, &image) == BOOT_ERROR_IDENTITY);
    /* Independent B offsets: 0x24000 base, full payload 0x1b400. */
    image = manifest_fixture();
    image.slot = BOOT_SLOT_B;
    image.execution_base = BOOT_SLOT_B_BASE;
    image.payload_length = BOOT_PAYLOAD_MAX;
    CHECK(boot_manifest_encode(&image, wire, 160) == BOOT_OK);
    CHECK(wire[28] == 1u && wire[29] == 0u);
    CHECK(wire[32] == 0u && wire[33] == 0x40u && wire[34] == 2u && wire[35] == 0u);
    CHECK(wire[36] == 0u && wire[37] == 0xb4u && wire[38] == 1u && wire[39] == 0u);
    CHECK(boot_manifest_decode(wire, 160, &decoded) == BOOT_OK);
    CHECK(decoded.slot == 1u && decoded.payload_length == 111616u);
}

static void test_manifest_rejections(void)
{
    static const struct { size_t offset; uint32_t value; boot_result_t error; } bad[] = {
        {16, 2, BOOT_ERROR_VERSION}, {20, 159, BOOT_ERROR_LENGTH},
        {24, 2, BOOT_ERROR_PRODUCT}, {28, 2, BOOT_ERROR_SLOT},
        {28, UINT32_MAX, BOOT_ERROR_SLOT}, {32, BOOT_SLOT_B_BASE, BOOT_ERROR_BASE},
        {32, UINT32_MAX, BOOT_ERROR_BASE}, {36, 0, BOOT_ERROR_BOUNDS},
        {36, 232, BOOT_ERROR_BOUNDS}, {36, 237, BOOT_ERROR_BOUNDS},
        {36, BOOT_PAYLOAD_MAX + 4u, BOOT_ERROR_BOUNDS},
        {36, UINT32_MAX, BOOT_ERROR_BOUNDS}, {44, 1, BOOT_ERROR_RESERVED}
    };
    uint8_t wire[160];
    boot_manifest_t out;
    boot_manifest_t sentinel;
    boot_manifest_t image = manifest_fixture();
    size_t i;
    memset(&sentinel, 0xa5, sizeof sentinel);
    for (i = 0; i < 160u; ++i)
    {
        /* Exact-sized stack span: sanitizer catches any premature reads. */
        uint8_t short_wire[i == 0u ? 1u : i];
        memcpy(short_wire, golden_manifest, i);
        out = sentinel;
        CHECK(boot_manifest_decode(short_wire, i, &out) == BOOT_ERROR_SIZE);
        CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
        memset(wire, 0xa5, sizeof wire);
        CHECK(boot_manifest_encode(&image, wire, i) == BOOT_ERROR_SIZE);
        CHECK(wire[0] == 0xa5 && wire[159] == 0xa5);
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; ++i)
    {
        memcpy(wire, golden_manifest, sizeof wire);
        word(wire, bad[i].offset, bad[i].value);
        out = sentinel;
        CHECK(boot_manifest_decode(wire, sizeof wire, &out) == bad[i].error);
        CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
    }
    for (i = 0; i < 16u; ++i)
    {
        memcpy(wire, golden_manifest, sizeof wire);
        wire[i] ^= 1u;
        CHECK(boot_manifest_decode(wire, sizeof wire, &out) == BOOT_ERROR_DOMAIN);
        memcpy(wire, golden_manifest, sizeof wire);
        wire[80u + i] = 1;
        CHECK(boot_manifest_decode(wire, sizeof wire, &out) == BOOT_ERROR_RESERVED);
    }
    CHECK(boot_manifest_decode(NULL, 160, &out) == BOOT_ERROR_ARGUMENT);
    CHECK(boot_manifest_decode(wire, 160, NULL) == BOOT_ERROR_ARGUMENT);
    image.payload_length = UINT32_MAX;
    memset(wire, 0xa5, sizeof wire);
    CHECK(boot_manifest_encode(&image, wire, sizeof wire) == BOOT_ERROR_BOUNDS);
    CHECK(wire[0] == 0xa5 && wire[159] == 0xa5);
}

static void test_spans_vectors(void)
{
    uint8_t payload[BOOT_PAYLOAD_MAX];
    uint32_t slot;
    uint32_t end = 42;
    boot_manifest_t image = manifest_fixture();
    memset(payload, 0xff, sizeof payload);
    for (slot = 0; slot < 2u; ++slot)
    {
        uint32_t base = slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
        uint32_t limit = slot == 0u ? BOOT_SLOT_A_MANIFEST : BOOT_SLOT_B_MANIFEST;
        uint32_t length;
        image.slot = slot;
        image.execution_base = base;
        for (length = BOOT_PAYLOAD_MIN; length <= BOOT_PAYLOAD_MAX;
             length += BOOT_PAYLOAD_MAX - BOOT_PAYLOAD_MIN)
        {
            uint32_t target;
            image.payload_length = length;
            CHECK(boot_image_span(slot, base, length, &end) == BOOT_OK);
            CHECK(end == base + length && end <= limit);
            word(payload, 0, BOOT_INITIAL_MSP);
            for (target = base; target < end; target += 2u)
            {
                word(payload, 4, target | 1u);
                CHECK(boot_vectors_validate(&image, payload, length) == BOOT_OK);
            }
            word(payload, 4, end | 1u);
            CHECK(boot_vectors_validate(&image, payload, length) == BOOT_ERROR_VECTOR_RANGE);
            word(payload, 4, (base - 2u) | 1u);
            CHECK(boot_vectors_validate(&image, payload, length) == BOOT_ERROR_VECTOR_RANGE);
            word(payload, 4, UINT32_MAX);
            CHECK(boot_vectors_validate(&image, payload, length) == BOOT_ERROR_VECTOR_RANGE);
            word(payload, 4, base);
            CHECK(boot_vectors_validate(&image, payload, length) == BOOT_ERROR_THUMB);
            word(payload, 4, base | 1u);
            word(payload, 0, BOOT_INITIAL_MSP - 8u);
            CHECK(boot_vectors_validate(&image, payload, length) == BOOT_ERROR_MSP);
            word(payload, 0, BOOT_STACK_BASE);
            CHECK(boot_vectors_validate(&image, payload, length) == BOOT_ERROR_MSP);
            word(payload, 0, BOOT_INITIAL_MSP);
            CHECK(boot_vectors_validate(&image, payload, length - 1u) == BOOT_ERROR_SIZE);
        }
        CHECK(boot_image_span(slot, base, 235, &end) == BOOT_ERROR_BOUNDS);
        CHECK(boot_image_span(slot, base, 238, &end) == BOOT_ERROR_BOUNDS);
        CHECK(boot_image_span(slot, base, UINT32_MAX - 3u, &end) == BOOT_ERROR_BOUNDS);
    }
    end = 42;
    CHECK(boot_image_span(2, 0, UINT32_MAX, &end) == BOOT_ERROR_SLOT);
    CHECK(end == 42u);
    CHECK(boot_image_span(0, UINT32_MAX, 236, &end) == BOOT_ERROR_BASE);
    CHECK(boot_image_span(0, BOOT_SLOT_A_BASE, 236, NULL) == BOOT_ERROR_ARGUMENT);
    image.payload_length = UINT32_MAX;
    CHECK(boot_vectors_validate(&image, payload, 0) == BOOT_ERROR_BOUNDS);
    CHECK(boot_vectors_validate(NULL, payload, 0) == BOOT_ERROR_ARGUMENT);
}

static void metadata_patch_error(size_t offset, uint32_t value, boot_result_t error)
{
    uint8_t wire[168];
    boot_metadata_t out;
    boot_metadata_t sentinel;
    memcpy(wire, golden_metadata, sizeof wire);
    word(wire, offset, value);
    word(wire, 160, boot_crc32(wire, 160));
    memset(&sentinel, 0xa5, sizeof sentinel);
    out = sentinel;
    CHECK(boot_metadata_decode(wire, sizeof wire, &out) == error);
    CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
}

static void test_metadata_rejections(void)
{
    uint8_t wire[168];
    boot_metadata_t out;
    boot_metadata_t sentinel;
    boot_metadata_t metadata = metadata_fixture();
    size_t i;
    memset(&sentinel, 0xa5, sizeof sentinel);
    for (i = 0; i < 168u; ++i)
    {
        uint8_t short_wire[i == 0u ? 1u : i];
        memcpy(short_wire, golden_metadata, i);
        out = sentinel;
        CHECK(boot_metadata_decode(short_wire, i, &out) == BOOT_ERROR_SIZE);
        CHECK(memcmp(&out, &sentinel, sizeof out) == 0);
        memset(wire, 0xa5, sizeof wire);
        CHECK(boot_metadata_encode(&metadata, 0, wire, i) == BOOT_ERROR_SIZE);
        CHECK(wire[0] == 0xa5 && wire[167] == 0xa5);
    }
    metadata_patch_error(0, 0, BOOT_ERROR_MAGIC);
    metadata_patch_error(4, 2, BOOT_ERROR_VERSION);
    metadata_patch_error(8, 164, BOOT_ERROR_LENGTH);
    metadata_patch_error(164, UINT32_MAX, BOOT_ERROR_COMMIT);
    metadata_patch_error(164, UINT32_C(0xfffffffe), BOOT_ERROR_COMMIT);
    metadata_patch_error(164, 1, BOOT_ERROR_COMMIT);
    for (i = 0; i < 32u; ++i)
        metadata_patch_error(164, UINT32_C(1) << i, BOOT_ERROR_COMMIT);
    metadata_patch_error(16, 2, BOOT_ERROR_SLOT);
    metadata_patch_error(20, 1, BOOT_ERROR_ROLE); /* empty candidate */
    metadata_patch_error(24, 0, BOOT_ERROR_ROLE); /* duplicate current */
    metadata_patch_error(28, 0, BOOT_ERROR_ROLE); /* unconfirmed current */
    metadata_patch_error(28, 3, BOOT_ERROR_ROLE); /* confirmed empty B */
    metadata_patch_error(28, 4, BOOT_ERROR_RESERVED);
    metadata_patch_error(68, 0, BOOT_ERROR_EMPTY);
    metadata_patch_error(68, 235, BOOT_ERROR_BOUNDS);
    metadata_patch_error(68, UINT32_MAX, BOOT_ERROR_BOUNDS);
    metadata_patch_error(80, 1, BOOT_ERROR_EMPTY);
    metadata_patch_error(112, 1, BOOT_ERROR_EMPTY);
    metadata_patch_error(120, 1, BOOT_ERROR_EMPTY);
    metadata_patch_error(124, 1, BOOT_ERROR_EMPTY);
    metadata_patch_error(128, 2, BOOT_ERROR_INTENT);
    metadata_patch_error(128, 1, BOOT_ERROR_INTENT);
    metadata_patch_error(132, 0, BOOT_ERROR_INTENT);
    metadata_patch_error(136, 1, BOOT_ERROR_INTENT);
    for (i = 140; i < 160u; i += 4u)
        metadata_patch_error(i, 1, BOOT_ERROR_RESERVED);
    for (i = 14; i < 32u; ++i)
        metadata_patch_error(72, UINT32_C(1) << i, BOOT_ERROR_RESERVED);
    for (i = 0; i < 160u; ++i)
    {
        memcpy(wire, golden_metadata, sizeof wire);
        wire[i] ^= 1u;
        CHECK(boot_metadata_decode(wire, sizeof wire, &out) != BOOT_OK);
    }
    memcpy(wire, golden_metadata, sizeof wire);
    wire[160] ^= 1u;
    CHECK(boot_metadata_decode(wire, sizeof wire, &out) == BOOT_ERROR_CRC);
    CHECK(boot_metadata_decode(NULL, 168, &out) == BOOT_ERROR_ARGUMENT);
    CHECK(boot_metadata_decode(wire, 168, NULL) == BOOT_ERROR_ARGUMENT);
    memset(wire, 0xa5, sizeof wire);
    metadata.confirmed_mask = 4;
    CHECK(boot_metadata_encode(&metadata, 0, wire, sizeof wire) == BOOT_ERROR_RESERVED);
    CHECK(wire[0] == 0xa5 && wire[167] == 0xa5);
}

static boot_metadata_t trial_fixture(void)
{
    boot_metadata_t m = metadata_fixture();
    m.sequence = 7;
    m.images[1] = m.images[0];
    m.images[1].accounting = BOOT_STATE_TRIAL;
    m.images[1].trial_id = 6;
    m.candidate_slot = BOOT_SLOT_B;
    return m;
}

static void test_accounting_roles_intent(void)
{
    boot_metadata_t m;
    unsigned count;
    unsigned position;
    unsigned reason;
    uint8_t wire[168];
    boot_metadata_t decoded;
    for (count = 0; count < 4u; ++count)
    {
        m = trial_fixture();
        m.images[1].accounting = count == 3u ? BOOT_STATE_REJECTED | BOOT_ACCOUNT_ROLLBACK : BOOT_STATE_TRIAL;
        if (count == 3u)
            m.candidate_slot = BOOT_SLOT_NONE;
        m.images[1].accounting |= count << BOOT_ACCOUNT_COUNT_SHIFT;
        for (position = 0; position < count; ++position)
            m.images[1].accounting |= BOOT_REASON_WATCHDOG << (5u + 3u * position);
        CHECK(boot_metadata_validate(&m) == BOOT_OK);
        CHECK(boot_metadata_encode(&m, 0, wire, sizeof wire) == BOOT_OK);
        CHECK(boot_metadata_decode(wire, sizeof wire, &decoded) == BOOT_OK);
        for (position = 0; position < 3u; ++position)
        {
            for (reason = 0; reason < 8u; ++reason)
            {
                boot_metadata_t altered = m;
                unsigned shift = 5u + 3u * position;
                int valid = position < count ? reason >= 1u && reason <= 5u : reason == 0u;
                altered.images[1].accounting &= ~(UINT32_C(7) << shift);
                altered.images[1].accounting |= reason << shift;
                CHECK(boot_metadata_validate(&altered) == (valid ? BOOT_OK : BOOT_ERROR_ACCOUNTING));
            }
        }
    }
    m = trial_fixture();
    m.images[1].accounting = BOOT_STATE_READY;
    CHECK(boot_metadata_validate(&m) == BOOT_OK);
    m.images[1].accounting |= 4u | (BOOT_REASON_EXTERNAL << 5);
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_ACCOUNTING);
    m = trial_fixture();
    m.intent_kind = BOOT_INTENT_DEADLINE;
    m.intent_slot = BOOT_SLOT_B;
    m.intent_trial_id = 6;
    CHECK(boot_metadata_validate(&m) == BOOT_OK);
    CHECK(boot_metadata_encode(&m, 0, wire, sizeof wire) == BOOT_OK);
    CHECK(boot_metadata_decode(wire, sizeof wire, &decoded) == BOOT_OK);
    CHECK(decoded.intent_trial_id == 6u);
    m.intent_trial_id = 5;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_INTENT);
    m.intent_trial_id = 6;
    m.intent_slot = BOOT_SLOT_A;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_INTENT);
    m.intent_slot = UINT32_MAX;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_INTENT);
    m.intent_slot = BOOT_SLOT_B;
    m.images[1].accounting = BOOT_STATE_READY;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_INTENT);
    m = trial_fixture();
    m.confirmed_mask = 3;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_ROLE);
    m = trial_fixture();
    m.candidate_slot = BOOT_SLOT_NONE;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_ROLE);
    m = trial_fixture();
    m.images[1].accounting = BOOT_STATE_NONE;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_ROLE);
    /* Confirmed NONE retains count/reasons/trial; A/B roles can swap. */
    m.current_slot = BOOT_SLOT_B;
    m.previous_slot = BOOT_SLOT_A;
    m.candidate_slot = BOOT_SLOT_NONE;
    m.confirmed_mask = 3;
    m.images[1].accounting = (2u << 2) | (2u << 5) | (5u << 8);
    CHECK(boot_metadata_validate(&m) == BOOT_OK);
    CHECK(boot_metadata_encode(&m, 0, wire, sizeof wire) == BOOT_OK);
    CHECK(boot_metadata_decode(wire, sizeof wire, &decoded) == BOOT_OK);
    CHECK(decoded.images[1].trial_id == 6u && decoded.images[1].accounting == m.images[1].accounting);
    m.previous_slot = BOOT_SLOT_B;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_ROLE);
    m = trial_fixture();
    m.images[1].accounting = BOOT_STATE_REJECTED | (3u << 2) | (1u << 5) | (2u << 8) | (3u << 11);
    m.candidate_slot = BOOT_SLOT_NONE;
    CHECK(boot_metadata_validate(&m) == BOOT_ERROR_ACCOUNTING); /* rollback missing */
}

/* Build committed wire independently so invalid semantic cases reach decode
 * with a correct CRC, without depending on encode accepting them. */
static void check_metadata_semantics(const boot_metadata_t *m, boot_result_t expected)
{
    uint8_t wire[168];
    uint8_t encoded[168];
    uint8_t untouched[168];
    boot_metadata_t decoded;
    boot_metadata_t sentinel;
    unsigned slot;
    memcpy(wire, golden_metadata, sizeof wire);
    word(wire, 12, m->sequence);
    word(wire, 16, m->current_slot);
    word(wire, 20, m->candidate_slot);
    word(wire, 24, m->previous_slot);
    word(wire, 28, m->confirmed_mask);
    for (slot = 0; slot < 2u; ++slot)
    {
        size_t offset = 32u + 48u * slot;
        memcpy(wire + offset, m->images[slot].payload_hash, 32);
        word(wire, offset + 32u, m->images[slot].release);
        word(wire, offset + 36u, m->images[slot].payload_length);
        word(wire, offset + 40u, m->images[slot].accounting);
        word(wire, offset + 44u, m->images[slot].trial_id);
    }
    word(wire, 128, m->intent_kind);
    word(wire, 132, m->intent_slot);
    word(wire, 136, m->intent_trial_id);
    word(wire, 160, boot_crc32(wire, 160));
    memset(untouched, 0xa5, sizeof untouched);
    memcpy(encoded, untouched, sizeof encoded);
    memset(&sentinel, 0xa5, sizeof sentinel);
    decoded = sentinel;
    CHECK(boot_metadata_validate(m) == expected);
    CHECK(boot_metadata_encode(m, BOOT_COMMIT_VALID, encoded, sizeof encoded) == expected);
    CHECK(boot_metadata_decode(wire, sizeof wire, &decoded) == expected);
    if (expected == BOOT_OK)
    {
        CHECK(memcmp(encoded, wire, sizeof wire) == 0);
        CHECK(boot_metadata_encode(&decoded, BOOT_COMMIT_VALID, encoded, sizeof encoded) == BOOT_OK);
        CHECK(memcmp(encoded, wire, sizeof wire) == 0);
    }
    else
    {
        CHECK(memcmp(encoded, untouched, sizeof encoded) == 0);
        CHECK(memcmp(&decoded, &sentinel, sizeof decoded) == 0);
    }
}

static void test_accounting_lifecycle(void)
{
    unsigned slot;
    uint32_t state;
    uint32_t count;
    uint32_t rollback;
    for (slot = 0; slot < 2u; ++slot)
    {
        for (state = BOOT_STATE_NONE; state <= BOOT_STATE_REJECTED; ++state)
        {
            for (count = 0; count < 4u; ++count)
            {
                for (rollback = 0; rollback < 2u; ++rollback)
                {
                    boot_metadata_t m = trial_fixture();
                    unsigned position;
                    int valid = state == BOOT_STATE_REJECTED ? count == 3u && rollback == 1u :
                        rollback == 0u && count < 3u && (state != BOOT_STATE_READY || count == 0u);
                    m.current_slot = 1u - slot;
                    m.candidate_slot = slot;
                    m.images[0].accounting = BOOT_STATE_NONE;
                    m.images[1].accounting = BOOT_STATE_NONE;
                    m.images[slot].trial_id = 6;
                    m.confirmed_mask = UINT32_C(1) << m.current_slot;
                    m.images[slot].accounting = state | (count << BOOT_ACCOUNT_COUNT_SHIFT) |
                        (rollback != 0u ? BOOT_ACCOUNT_ROLLBACK : 0u);
                    for (position = 0; position < count; ++position)
                        m.images[slot].accounting |= (position + 1u) << (5u + 3u * position);
                    if (state == BOOT_STATE_NONE)
                    {
                        m.previous_slot = m.current_slot;
                        m.current_slot = slot;
                        m.confirmed_mask = BOOT_CONFIRMED_A | BOOT_CONFIRMED_B;
                        m.candidate_slot = BOOT_SLOT_NONE;
                    }
                    else if (state == BOOT_STATE_REJECTED)
                        m.candidate_slot = BOOT_SLOT_NONE;
                    check_metadata_semantics(&m, valid ? BOOT_OK : BOOT_ERROR_ACCOUNTING);
                }
            }
        }
    }
}

static void test_trial_sequence_bounds(void)
{
    unsigned slot;
    uint32_t state;
    for (slot = 0; slot < 2u; ++slot)
    {
        for (state = BOOT_STATE_NONE; state <= BOOT_STATE_REJECTED; ++state)
        {
            boot_metadata_t m = trial_fixture();
            m.current_slot = 1u - slot;
            m.candidate_slot = slot;
            m.confirmed_mask = UINT32_C(1) << m.current_slot;
            m.images[0].accounting = BOOT_STATE_NONE;
            m.images[1].accounting = BOOT_STATE_NONE;
            m.images[slot].accounting = state;
            if (state == BOOT_STATE_NONE)
            {
                m.candidate_slot = BOOT_SLOT_NONE;
                m.confirmed_mask |= UINT32_C(1) << slot;
                m.previous_slot = slot;
                m.images[slot].accounting = (1u << 2) | (BOOT_REASON_EXTERNAL << 5);
            }
            else if (state == BOOT_STATE_REJECTED)
            {
                m.candidate_slot = BOOT_SLOT_NONE;
                m.images[slot].accounting |= BOOT_ACCOUNT_ROLLBACK | (3u << 2) |
                    (1u << 5) | (2u << 8) | (3u << 11);
            }
            m.images[slot].trial_id = 8; /* Future acceptance in sequence 7. */
            check_metadata_semantics(&m, BOOT_ERROR_ACCOUNTING);
            m.images[slot].trial_id = 7;
            check_metadata_semantics(&m, BOOT_OK);
            m.images[slot].trial_id = 6;
            check_metadata_semantics(&m, BOOT_OK);
            if (state == BOOT_STATE_REJECTED)
            {
                boot_metadata_t confirmed = m;
                confirmed.confirmed_mask |= UINT32_C(1) << slot;
                check_metadata_semantics(&confirmed, BOOT_ERROR_ROLE);
            }
            m.images[slot].trial_id = UINT32_MAX;
            m.sequence = UINT32_MAX - 1u;
            check_metadata_semantics(&m, BOOT_ERROR_ACCOUNTING);
            m.sequence = UINT32_MAX;
            check_metadata_semantics(&m, BOOT_OK);
        }
    }
}

static void test_selection_exhaustion(void)
{
    uint8_t pages[2][1024];
    boot_metadata_t m = metadata_fixture();
    boot_metadata_t out;
    boot_metadata_t sentinel;
    unsigned page = 99;
    uint32_t next = 99;
    memset(pages, 0xff, sizeof pages);
    memset(&sentinel, 0xa5, sizeof sentinel);
    out = sentinel;
    CHECK(boot_metadata_select(pages[0], 1024, pages[1], 1024, &out, &page) == BOOT_ERROR_NO_RECORD);
    CHECK(page == 99u && memcmp(&out, &sentinel, sizeof out) == 0);
    CHECK(boot_metadata_encode(&m, 0, pages[0], 1024) == BOOT_OK);
    CHECK(boot_metadata_select(pages[0], 1024, pages[1], 1024, &out, &page) == BOOT_OK);
    CHECK(page == 0u && out.sequence == 1u);
    m.sequence = 2;
    CHECK(boot_metadata_encode(&m, 0, pages[1], 1024) == BOOT_OK);
    CHECK(boot_metadata_select(pages[0], 1024, pages[1], 1024, &out, &page) == BOOT_OK);
    CHECK(page == 1u && out.sequence == 2u);
    CHECK(boot_metadata_select(pages[1], 1024, pages[0], 1024, &out, &page) == BOOT_OK);
    CHECK(page == 0u && out.sequence == 2u);
    memcpy(pages[0], pages[1], 168);
    pages[0][168] = 0;
    CHECK(boot_metadata_select(pages[0], 1024, pages[1], 1024, &out, &page) == BOOT_OK);
    CHECK(page == 0u); /* identical prefixes, unequal ignored tails */
    m.images[0].release++;
    CHECK(boot_metadata_encode(&m, 0, pages[1], 1024) == BOOT_OK);
    page = 99;
    out = sentinel;
    CHECK(boot_metadata_select(pages[0], 1024, pages[1], 1024, &out, &page) == BOOT_ERROR_CONFLICT);
    CHECK(page == 99u && memcmp(&out, &sentinel, sizeof out) == 0);
    m.sequence = UINT32_MAX;
    CHECK(boot_metadata_encode(&m, 0, pages[1], 1024) == BOOT_OK);
    CHECK(boot_metadata_select(pages[0], 1024, pages[1], 1024, &out, &page) == BOOT_OK);
    CHECK(page == 1u && out.sequence == UINT32_MAX);
    CHECK(boot_metadata_next_sequence(&out, &next) == BOOT_ERROR_EXHAUSTED);
    CHECK(next == 99u);
    /* Invalid raw max cannot outrank valid smaller page. */
    pages[1][160] ^= 1u;
    CHECK(boot_metadata_select(pages[0], 1024, pages[1], 1024, &out, &page) == BOOT_OK);
    CHECK(page == 0u && out.sequence == 2u);
    CHECK(boot_metadata_select(pages[1], 1024, pages[0], 1024, &out, &page) == BOOT_OK);
    CHECK(page == 1u);
    CHECK(boot_metadata_select(NULL, 0, pages[0], 168, &out, &page) == BOOT_OK);
    CHECK(page == 1u);
    CHECK(boot_metadata_select(pages[0], 167, pages[1], 0, &out, &page) == BOOT_ERROR_NO_RECORD);
    CHECK(boot_metadata_select(pages[0], 168, pages[1], 168, NULL, &page) == BOOT_ERROR_ARGUMENT);
    m = trial_fixture();
    m.sequence = UINT32_MAX;
    m.images[1].trial_id = UINT32_MAX;
    m.intent_kind = BOOT_INTENT_DEADLINE;
    m.intent_slot = 1;
    m.intent_trial_id = UINT32_MAX;
    CHECK(boot_metadata_encode(&m, 0, pages[0], 1024) == BOOT_OK);
    CHECK(boot_metadata_decode(pages[0], 168, &out) == BOOT_OK);
    CHECK(out.images[1].trial_id == UINT32_MAX && out.intent_trial_id == UINT32_MAX);
    CHECK(boot_metadata_next_sequence(&m, &next) == BOOT_ERROR_EXHAUSTED);
    m.sequence = UINT32_MAX - 1u;
    m.images[1].trial_id = m.sequence;
    m.intent_trial_id = m.sequence;
    check_metadata_semantics(&m, BOOT_OK);
    CHECK(boot_metadata_next_sequence(&m, &next) == BOOT_OK);
    CHECK(next == UINT32_MAX);
}

static void test_commit_cuts(void)
{
    uint8_t old_page[1024];
    uint8_t new_page[1024];
    uint8_t staged[168];
    boot_metadata_t m = metadata_fixture();
    boot_metadata_t out;
    unsigned page;
    size_t cut;
    CHECK(boot_metadata_encode(&m, BOOT_COMMIT_VALID, old_page, sizeof old_page) == BOOT_OK);
    m.sequence = 2;
    CHECK(boot_metadata_encode(&m, BOOT_COMMIT_ERASED, staged, sizeof staged) == BOOT_OK);
    /* Every byte-prefix interruption before final marker: old remains valid.
     * This is only a deterministic codec/selection model, not a flash driver. */
    for (cut = 0; cut <= 168u; ++cut)
    {
        memset(new_page, 0xff, sizeof new_page);
        memcpy(new_page, staged, cut);
        CHECK(boot_metadata_select(old_page, sizeof old_page, new_page, sizeof new_page,
                                   &out, &page) == BOOT_OK);
        CHECK(page == 0u && out.sequence == 1u);
    }
    word(new_page, 164, BOOT_COMMIT_VALID);
    CHECK(boot_metadata_select(old_page, sizeof old_page, new_page, sizeof new_page,
                               &out, &page) == BOOT_OK);
    CHECK(page == 1u && out.sequence == 2u);
}

static void test_memory_map(void)
{
    CHECK(BOOT_FLASH_BASE == 0u && BOOT_FLASH_END == 262144u);
    CHECK(BOOT_LOADER_END == BOOT_METADATA_PAGE0);
    CHECK(BOOT_METADATA_PAGE1 - BOOT_METADATA_PAGE0 == BOOT_ERASE_BYTES);
    CHECK(BOOT_METADATA_END - BOOT_METADATA_PAGE1 == BOOT_ERASE_BYTES);
    CHECK(BOOT_METADATA_END == BOOT_SLOT_A_BASE);
    CHECK(BOOT_SLOT_A_END - BOOT_SLOT_A_BASE == BOOT_SLOT_BYTES);
    CHECK(BOOT_SLOT_B_END - BOOT_SLOT_B_BASE == BOOT_SLOT_BYTES);
    CHECK(BOOT_SLOT_A_END == BOOT_SLOT_B_BASE);
    CHECK(BOOT_FLASH_END - BOOT_SPARE_BASE == BOOT_PROTECTION_BYTES);
    CHECK(BOOT_SLOT_A_MANIFEST - BOOT_SLOT_A_BASE == BOOT_PAYLOAD_MAX);
    CHECK(BOOT_SLOT_B_MANIFEST - BOOT_SLOT_B_BASE == BOOT_PAYLOAD_MAX);
    CHECK(BOOT_SLOT_A_END - BOOT_SLOT_A_MANIFEST == BOOT_MANIFEST_RESERVE_BYTES);
    CHECK(BOOT_SLOT_B_END - BOOT_SLOT_B_MANIFEST == BOOT_MANIFEST_RESERVE_BYTES);
    CHECK(BOOT_PAYLOAD_MAX + BOOT_MANIFEST_BYTES == BOOT_PACKAGE_MAX);
    CHECK(BOOT_PAYLOAD_MIN == 59u * BOOT_PROGRAM_BYTES);
    CHECK(BOOT_SLOT_A_BASE % BOOT_VECTOR_ALIGNMENT == 0u);
    CHECK(BOOT_SLOT_B_BASE % BOOT_VECTOR_ALIGNMENT == 0u);
    CHECK(BOOT_DIAGNOSTIC_BASE + BOOT_DIAGNOSTIC_BYTES == BOOT_STACK_BASE);
    CHECK(BOOT_STACK_BASE + BOOT_STACK_BYTES == BOOT_INITIAL_MSP);
    CHECK(BOOT_SRAM_END - BOOT_SRAM_BASE == 65536u);
}

int main(void)
{
    test_golden();
    test_manifest_rejections();
    test_spans_vectors();
    test_metadata_rejections();
    test_accounting_roles_intent();
    test_accounting_lifecycle();
    test_trial_sequence_bounds();
    test_selection_exhaustion();
    test_memory_map();
    test_commit_cuts();
    printf("boot format: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}
