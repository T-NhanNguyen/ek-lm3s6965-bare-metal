#include "lm3s6965/boot_format.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t manifest_domain[16] = "LM3S6965-FW-V1";

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int zero_bytes(const uint8_t *p, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i)
    {
        if (p[i] != 0u)
            return 0;
    }
    return 1;
}

static int slot_valid(uint32_t slot)
{
    return slot == BOOT_SLOT_A || slot == BOOT_SLOT_B;
}

static int role_valid(uint32_t slot)
{
    return slot_valid(slot) || slot == BOOT_SLOT_NONE;
}

boot_result_t boot_image_span(uint32_t slot, uint32_t base, uint32_t length,
                              uint32_t *end)
{
    uint32_t expected;
    uint32_t limit;
    if (end == NULL)
        return BOOT_ERROR_ARGUMENT;
    if (!slot_valid(slot))
        return BOOT_ERROR_SLOT;
    expected = slot == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    limit = slot == BOOT_SLOT_A ? BOOT_SLOT_A_MANIFEST : BOOT_SLOT_B_MANIFEST;
    if (base != expected)
        return BOOT_ERROR_BASE;
    /* Subtraction precedes addition, so attacker-supplied length cannot wrap. */
    if (length < BOOT_PAYLOAD_MIN || length > limit - base || (length & 3u) != 0u)
        return BOOT_ERROR_BOUNDS;
    *end = base + length;
    return BOOT_OK;
}

boot_result_t boot_manifest_validate(const boot_manifest_t *manifest)
{
    uint32_t end;
    if (manifest == NULL)
        return BOOT_ERROR_ARGUMENT;
    return boot_image_span(manifest->slot, manifest->execution_base,
                           manifest->payload_length, &end);
}

boot_result_t boot_manifest_decode(const uint8_t *wire, size_t size,
                                   boot_manifest_t *out)
{
    boot_manifest_t value;
    boot_result_t result;
    if (wire == NULL || out == NULL)
        return BOOT_ERROR_ARGUMENT;
    if (size < BOOT_MANIFEST_BYTES)
        return BOOT_ERROR_SIZE;
    if (memcmp(wire, manifest_domain, sizeof manifest_domain) != 0)
        return BOOT_ERROR_DOMAIN;
    if (get32(wire + 16) != BOOT_FORMAT_VERSION)
        return BOOT_ERROR_VERSION;
    if (get32(wire + 20) != BOOT_MANIFEST_BYTES)
        return BOOT_ERROR_LENGTH;
    if (get32(wire + 24) != BOOT_PRODUCT_ID)
        return BOOT_ERROR_PRODUCT;
    if (get32(wire + 44) != 0u || !zero_bytes(wire + 80, 16))
        return BOOT_ERROR_RESERVED;
    value.slot = get32(wire + 28);
    value.execution_base = get32(wire + 32);
    value.payload_length = get32(wire + 36);
    value.release = get32(wire + 40);
    result = boot_manifest_validate(&value);
    if (result != BOOT_OK)
        return result;
    memcpy(value.payload_hash, wire + 48, 32);
    memcpy(value.signature, wire + 96, 64);
    *out = value;
    return BOOT_OK;
}

boot_result_t boot_manifest_encode(const boot_manifest_t *manifest,
                                   uint8_t *wire, size_t capacity)
{
    boot_result_t result;
    if (wire == NULL || manifest == NULL)
        return BOOT_ERROR_ARGUMENT;
    if (capacity < BOOT_MANIFEST_BYTES)
        return BOOT_ERROR_SIZE;
    result = boot_manifest_validate(manifest);
    if (result != BOOT_OK)
        return result;
    memset(wire, 0, BOOT_MANIFEST_BYTES);
    memcpy(wire, manifest_domain, sizeof manifest_domain);
    put32(wire + 16, BOOT_FORMAT_VERSION);
    put32(wire + 20, BOOT_MANIFEST_BYTES);
    put32(wire + 24, BOOT_PRODUCT_ID);
    put32(wire + 28, manifest->slot);
    put32(wire + 32, manifest->execution_base);
    put32(wire + 36, manifest->payload_length);
    put32(wire + 40, manifest->release);
    memcpy(wire + 48, manifest->payload_hash, 32);
    memcpy(wire + 96, manifest->signature, 64);
    return BOOT_OK;
}

boot_result_t boot_vectors_validate(const boot_manifest_t *manifest,
                                    const uint8_t *payload, size_t size)
{
    uint32_t end;
    uint32_t reset;
    boot_result_t result;
    if (manifest == NULL || payload == NULL)
        return BOOT_ERROR_ARGUMENT;
    result = boot_image_span(manifest->slot, manifest->execution_base,
                             manifest->payload_length, &end);
    if (result != BOOT_OK)
        return result;
    if (size < manifest->payload_length)
        return BOOT_ERROR_SIZE;
    if (get32(payload) != BOOT_INITIAL_MSP)
        return BOOT_ERROR_MSP;
    reset = get32(payload + 4);
    if ((reset & 1u) == 0u)
        return BOOT_ERROR_THUMB;
    reset &= ~UINT32_C(1);
    if (reset < manifest->execution_base || reset >= end)
        return BOOT_ERROR_VECTOR_RANGE;
    return BOOT_OK;
}

uint32_t boot_crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned bit;
    for (i = 0; i < size; ++i)
    {
        crc ^= bytes[i];
        for (bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) != 0u ? UINT32_C(0xedb88320) : 0u);
    }
    return crc ^ UINT32_MAX;
}

static boot_result_t entry_validate(const boot_image_entry_t *entry)
{
    unsigned i;
    uint32_t state = entry->accounting & BOOT_ACCOUNT_STATE_MASK;
    uint32_t count = (entry->accounting >> BOOT_ACCOUNT_COUNT_SHIFT) & 3u;
    if (entry->payload_length == 0u)
    {
        if (!zero_bytes(entry->payload_hash, 32) || entry->release != 0u ||
            entry->accounting != 0u || entry->trial_id != 0u)
            return BOOT_ERROR_EMPTY;
        return BOOT_OK;
    }
    if (entry->payload_length < BOOT_PAYLOAD_MIN ||
        entry->payload_length > BOOT_PAYLOAD_MAX || (entry->payload_length & 3u) != 0u)
        return BOOT_ERROR_BOUNDS;
    if ((entry->accounting & BOOT_ACCOUNT_RESERVED_MASK) != 0u)
        return BOOT_ERROR_RESERVED;
    for (i = 0; i < 3u; ++i)
    {
        uint32_t reason = (entry->accounting >> (5u + 3u * i)) & 7u;
        if ((i < count && (reason == 0u || reason > BOOT_REASON_COMBINED)) ||
            (i >= count && reason != 0u))
            return BOOT_ERROR_ACCOUNTING;
    }
    if ((state == BOOT_STATE_READY &&
         (entry->accounting != BOOT_STATE_READY)) ||
        (state != BOOT_STATE_REJECTED &&
         (count == 3u || (entry->accounting & BOOT_ACCOUNT_ROLLBACK) != 0u)) ||
        (state == BOOT_STATE_REJECTED &&
         (count != 3u || (entry->accounting & BOOT_ACCOUNT_ROLLBACK) == 0u)))
        return BOOT_ERROR_ACCOUNTING;
    return BOOT_OK;
}

boot_result_t boot_metadata_validate(const boot_metadata_t *metadata)
{
    unsigned i;
    boot_result_t result;
    if (metadata == NULL)
        return BOOT_ERROR_ARGUMENT;
    if (!role_valid(metadata->current_slot) || !role_valid(metadata->candidate_slot) ||
        !role_valid(metadata->previous_slot))
        return BOOT_ERROR_SLOT;
    if ((metadata->confirmed_mask & ~UINT32_C(3)) != 0u)
        return BOOT_ERROR_RESERVED;
    for (i = 0; i < 2u; ++i)
    {
        const boot_image_entry_t *entry = &metadata->images[i];
        uint32_t confirmed = metadata->confirmed_mask & (UINT32_C(1) << i);
        uint32_t state = entry->accounting & BOOT_ACCOUNT_STATE_MASK;
        result = entry_validate(entry);
        if (result != BOOT_OK)
            return result;
        /* IDs are acceptance sequences retained in later records; no wrap. */
        if (entry->trial_id > metadata->sequence)
            return BOOT_ERROR_ACCOUNTING;
        if (entry->payload_length == 0u &&
            (confirmed != 0u || metadata->current_slot == i ||
             metadata->previous_slot == i || metadata->candidate_slot == i))
            return BOOT_ERROR_ROLE;
        if (confirmed != 0u && state != BOOT_STATE_NONE)
            return BOOT_ERROR_ROLE;
        if ((state == BOOT_STATE_READY || state == BOOT_STATE_TRIAL) &&
            metadata->candidate_slot != i)
            return BOOT_ERROR_ROLE;
    }
    if (metadata->current_slot != BOOT_SLOT_NONE &&
        (metadata->confirmed_mask & (UINT32_C(1) << metadata->current_slot)) == 0u)
        return BOOT_ERROR_ROLE;
    if (metadata->previous_slot != BOOT_SLOT_NONE &&
        ((metadata->confirmed_mask & (UINT32_C(1) << metadata->previous_slot)) == 0u ||
         metadata->previous_slot == metadata->current_slot))
        return BOOT_ERROR_ROLE;
    if (metadata->candidate_slot != BOOT_SLOT_NONE)
    {
        uint32_t slot = metadata->candidate_slot;
        uint32_t state = metadata->images[slot].accounting & BOOT_ACCOUNT_STATE_MASK;
        if ((metadata->confirmed_mask & (UINT32_C(1) << slot)) != 0u ||
            slot == metadata->current_slot || slot == metadata->previous_slot ||
            (state != BOOT_STATE_READY && state != BOOT_STATE_TRIAL))
            return BOOT_ERROR_ROLE;
    }
    if (metadata->intent_kind == BOOT_INTENT_NONE)
    {
        if (metadata->intent_slot != BOOT_SLOT_NONE || metadata->intent_trial_id != 0u)
            return BOOT_ERROR_INTENT;
    }
    else if (metadata->intent_kind == BOOT_INTENT_DEADLINE)
    {
        uint32_t slot = metadata->intent_slot;
        if (!slot_valid(slot) || slot != metadata->candidate_slot ||
            (metadata->images[slot].accounting & BOOT_ACCOUNT_STATE_MASK) != BOOT_STATE_TRIAL ||
            metadata->images[slot].trial_id != metadata->intent_trial_id)
            return BOOT_ERROR_INTENT;
    }
    else
        return BOOT_ERROR_INTENT;
    return BOOT_OK;
}

boot_result_t boot_metadata_decode(const uint8_t *wire, size_t size,
                                   boot_metadata_t *out)
{
    boot_metadata_t value;
    boot_result_t result;
    unsigned i;
    if (wire == NULL || out == NULL)
        return BOOT_ERROR_ARGUMENT;
    if (size < BOOT_METADATA_BYTES)
        return BOOT_ERROR_SIZE;
    if (get32(wire) != BOOT_METADATA_MAGIC)
        return BOOT_ERROR_MAGIC;
    if (get32(wire + 4) != BOOT_FORMAT_VERSION)
        return BOOT_ERROR_VERSION;
    if (get32(wire + 8) != BOOT_METADATA_BYTES)
        return BOOT_ERROR_LENGTH;
    if (get32(wire + 164) != BOOT_COMMIT_VALID)
        return BOOT_ERROR_COMMIT;
    if (get32(wire + 160) != boot_crc32(wire, BOOT_METADATA_CRC_BYTES))
        return BOOT_ERROR_CRC;
    if (get32(wire + 140) != 0u || !zero_bytes(wire + 144, 16))
        return BOOT_ERROR_RESERVED;
    value.sequence = get32(wire + 12);
    value.current_slot = get32(wire + 16);
    value.candidate_slot = get32(wire + 20);
    value.previous_slot = get32(wire + 24);
    value.confirmed_mask = get32(wire + 28);
    for (i = 0; i < 2u; ++i)
    {
        const uint8_t *p = wire + 32u + 48u * i;
        memcpy(value.images[i].payload_hash, p, 32);
        value.images[i].release = get32(p + 32);
        value.images[i].payload_length = get32(p + 36);
        value.images[i].accounting = get32(p + 40);
        value.images[i].trial_id = get32(p + 44);
    }
    value.intent_kind = get32(wire + 128);
    value.intent_slot = get32(wire + 132);
    value.intent_trial_id = get32(wire + 136);
    result = boot_metadata_validate(&value);
    if (result != BOOT_OK)
        return result;
    *out = value;
    return BOOT_OK;
}

boot_result_t boot_metadata_encode(const boot_metadata_t *metadata,
                                   uint32_t commit, uint8_t *wire, size_t capacity)
{
    unsigned i;
    boot_result_t result;
    if (metadata == NULL || wire == NULL)
        return BOOT_ERROR_ARGUMENT;
    if (capacity < BOOT_METADATA_BYTES)
        return BOOT_ERROR_SIZE;
    if (commit != BOOT_COMMIT_ERASED && commit != BOOT_COMMIT_VALID)
        return BOOT_ERROR_COMMIT;
    result = boot_metadata_validate(metadata);
    if (result != BOOT_OK)
        return result;
    memset(wire, 0, BOOT_METADATA_BYTES);
    put32(wire, BOOT_METADATA_MAGIC);
    put32(wire + 4, BOOT_FORMAT_VERSION);
    put32(wire + 8, BOOT_METADATA_BYTES);
    put32(wire + 12, metadata->sequence);
    put32(wire + 16, metadata->current_slot);
    put32(wire + 20, metadata->candidate_slot);
    put32(wire + 24, metadata->previous_slot);
    put32(wire + 28, metadata->confirmed_mask);
    for (i = 0; i < 2u; ++i)
    {
        uint8_t *p = wire + 32u + 48u * i;
        memcpy(p, metadata->images[i].payload_hash, 32);
        put32(p + 32, metadata->images[i].release);
        put32(p + 36, metadata->images[i].payload_length);
        put32(p + 40, metadata->images[i].accounting);
        put32(p + 44, metadata->images[i].trial_id);
    }
    put32(wire + 128, metadata->intent_kind);
    put32(wire + 132, metadata->intent_slot);
    put32(wire + 136, metadata->intent_trial_id);
    put32(wire + 160, boot_crc32(wire, BOOT_METADATA_CRC_BYTES));
    put32(wire + 164, commit);
    return BOOT_OK;
}

boot_result_t boot_metadata_select(const uint8_t *page0, size_t size0,
                                   const uint8_t *page1, size_t size1,
                                   boot_metadata_t *out, unsigned *page)
{
    boot_metadata_t values[2];
    boot_result_t results[2];
    unsigned selected;
    if (out == NULL || page == NULL)
        return BOOT_ERROR_ARGUMENT;
    results[0] = boot_metadata_decode(page0, size0, &values[0]);
    results[1] = boot_metadata_decode(page1, size1, &values[1]);
    if (results[0] != BOOT_OK && results[1] != BOOT_OK)
        return BOOT_ERROR_NO_RECORD;
    if (results[0] == BOOT_OK && results[1] == BOOT_OK)
    {
        if (values[0].sequence == values[1].sequence &&
            memcmp(page0, page1, BOOT_METADATA_BYTES) != 0)
            return BOOT_ERROR_CONFLICT;
        selected = values[1].sequence > values[0].sequence ? 1u : 0u;
    }
    else
        selected = results[0] == BOOT_OK ? 0u : 1u;
    *out = values[selected];
    *page = selected;
    return BOOT_OK;
}

boot_result_t boot_metadata_next_sequence(const boot_metadata_t *metadata,
                                          uint32_t *next)
{
    boot_result_t result;
    if (next == NULL)
        return BOOT_ERROR_ARGUMENT;
    result = boot_metadata_validate(metadata);
    if (result != BOOT_OK)
        return result;
    if (metadata->sequence == UINT32_MAX)
        return BOOT_ERROR_EXHAUSTED;
    *next = metadata->sequence + 1u;
    return BOOT_OK;
}

boot_result_t boot_metadata_image_matches(const boot_metadata_t *metadata,
                                          const boot_manifest_t *manifest)
{
    const boot_image_entry_t *entry;
    boot_result_t result = boot_metadata_validate(metadata);
    if (result != BOOT_OK)
        return result;
    result = boot_manifest_validate(manifest);
    if (result != BOOT_OK)
        return result;
    entry = &metadata->images[manifest->slot];
    if (entry->payload_length != manifest->payload_length ||
        entry->release != manifest->release ||
        memcmp(entry->payload_hash, manifest->payload_hash, 32) != 0)
        return BOOT_ERROR_IDENTITY;
    return BOOT_OK;
}
