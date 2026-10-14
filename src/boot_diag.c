#include "lm3s6965/boot_diag.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static bool slot_valid(uint32_t slot)
{
    return slot == BOOT_SLOT_A || slot == BOOT_SLOT_B || slot == BOOT_SLOT_NONE;
}
static bool validity_valid(boot_diag_validity_t validity)
{
    return validity == BOOT_DIAG_STAGING || validity == BOOT_DIAG_PUBLISHED;
}
boot_diag_result_t boot_diag_validate(const boot_diag_t *record)
{
    if (record == NULL) return BOOT_DIAG_ERROR_ARGUMENT;
    if (record->stage > 5u) return BOOT_DIAG_ERROR_STAGE;
    if (record->fault > 5u) return BOOT_DIAG_ERROR_FAULT;
    if (record->detail > 20u) return BOOT_DIAG_ERROR_DETAIL;
    if (!slot_valid(record->running_slot) || !slot_valid(record->selected_slot) ||
        !slot_valid(record->failed_slot)) return BOOT_DIAG_ERROR_SLOT;
    if (record->failures > 3u) return BOOT_DIAG_ERROR_COUNT;
    if ((record->health & ~BOOT_DIAG_HEALTH_MASK) != 0u)
        return BOOT_DIAG_ERROR_HEALTH;
    return BOOT_DIAG_OK;
}
boot_diag_result_t boot_diag_encode(const boot_diag_t *record,
    boot_diag_validity_t validity, uint8_t *wire, size_t capacity)
{
    boot_diag_result_t result;
    if (record == NULL || wire == NULL) return BOOT_DIAG_ERROR_ARGUMENT;
    if (capacity < BOOT_DIAG_BYTES) return BOOT_DIAG_ERROR_SIZE;
    if (!validity_valid(validity)) return BOOT_DIAG_ERROR_VALIDITY;
    result = boot_diag_validate(record);
    if (result != BOOT_DIAG_OK) return result;
    put32(wire, BOOT_DIAG_MAGIC);
    put32(wire + 4, BOOT_DIAG_VERSION);
    put32(wire + 8, BOOT_DIAG_BYTES);
    put32(wire + 12, (uint32_t)validity);
    put32(wire + 16, record->stage);
    put32(wire + 20, record->fault);
    put32(wire + 24, record->detail);
    put32(wire + 28, record->reset_flags);
    put32(wire + 32, record->running_slot);
    put32(wire + 36, record->selected_slot);
    put32(wire + 40, record->sequence);
    put32(wire + 44, record->trial_id);
    put32(wire + 48, record->failures);
    put32(wire + 52, record->health);
    put32(wire + 56, record->failed_slot);
    put32(wire + 60, 0u);
    return BOOT_DIAG_OK;
}
boot_diag_result_t boot_diag_decode(const uint8_t *wire, size_t size,
    boot_diag_validity_t required_validity, boot_diag_t *out)
{
    boot_diag_t record;
    boot_diag_result_t result;
    if (wire == NULL || out == NULL) return BOOT_DIAG_ERROR_ARGUMENT;
    if (size < BOOT_DIAG_BYTES) return BOOT_DIAG_ERROR_SIZE;
    if (!validity_valid(required_validity)) return BOOT_DIAG_ERROR_VALIDITY;
    if (get32(wire) != BOOT_DIAG_MAGIC) return BOOT_DIAG_ERROR_MAGIC;
    if (get32(wire + 4) != BOOT_DIAG_VERSION) return BOOT_DIAG_ERROR_VERSION;
    if (get32(wire + 8) != BOOT_DIAG_BYTES) return BOOT_DIAG_ERROR_LENGTH;
    if (get32(wire + 12) != (uint32_t)required_validity)
        return BOOT_DIAG_ERROR_VALIDITY;
    if (get32(wire + 60) != 0u) return BOOT_DIAG_ERROR_RESERVED;
    record.stage = get32(wire + 16);
    record.fault = get32(wire + 20);
    record.detail = get32(wire + 24);
    record.reset_flags = get32(wire + 28);
    record.running_slot = get32(wire + 32);
    record.selected_slot = get32(wire + 36);
    record.sequence = get32(wire + 40);
    record.trial_id = get32(wire + 44);
    record.failures = get32(wire + 48);
    record.health = get32(wire + 52);
    record.failed_slot = get32(wire + 56);
    result = boot_diag_validate(&record);
    if (result != BOOT_DIAG_OK) return result;
    *out = record;
    return BOOT_DIAG_OK;
}
boot_diag_result_t boot_diag_init(uint32_t captured_flags, boot_diag_t *out,
    boot_diag_context_t *context)
{
    const boot_diag_t record = {
        BOOT_DIAG_STAGE_RESET, BOOT_DIAG_FAULT_NONE, BOOT_DIAG_DETAIL_NONE,
        captured_flags, BOOT_SLOT_NONE, BOOT_SLOT_NONE, 0u, 0u, 0u, 0u,
        BOOT_SLOT_NONE
    };
    const boot_diag_context_t evidence = {false, false, false, true, true, true};
    if (out == NULL || context == NULL) return BOOT_DIAG_ERROR_ARGUMENT;
    *out = record;
    *context = evidence;
    return BOOT_DIAG_OK;
}
boot_diag_optional_slot_t boot_diag_slot(uint32_t slot)
{
    const bool available = slot == BOOT_SLOT_A || slot == BOOT_SLOT_B;
    const boot_diag_optional_slot_t value = {available, available ? slot : 0u};
    return value;
}
static boot_diag_optional_u32_t optional(uint32_t value, bool known)
{
    const boot_diag_optional_u32_t out = {known, known ? value : 0u};
    return out;
}
boot_diag_optional_u32_t boot_diag_sequence(const boot_diag_t *record,
    const boot_diag_context_t *context)
{
    if (record == NULL || context == NULL) return optional(0u, false);
    return optional(record->sequence, context->sequence_known);
}
boot_diag_optional_u32_t boot_diag_trial_id(const boot_diag_t *record,
    const boot_diag_context_t *context)
{
    if (record == NULL || context == NULL) return optional(0u, false);
    return optional(record->trial_id, context->trial_id_known);
}
boot_diag_optional_u32_t boot_diag_failures(const boot_diag_t *record,
    const boot_diag_context_t *context)
{
    if (record == NULL || context == NULL) return optional(0u, false);
    return optional(record->failures, context->failures_known && record->failures <= 3u);
}
