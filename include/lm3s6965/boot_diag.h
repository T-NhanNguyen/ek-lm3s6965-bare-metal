/* Pure ABD1 codec. No live SRAM publication or hardware access. */
#ifndef LM3S6965_BOOT_DIAG_H
#define LM3S6965_BOOT_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_format.h"

#define BOOT_DIAG_MAGIC UINT32_C(0x31444241)
#define BOOT_DIAG_VERSION 1u
#define BOOT_DIAG_BYTES BOOT_DIAGNOSTIC_BYTES
#define BOOT_DIAG_END BOOT_STACK_BASE
#define BOOT_DIAG_HEALTH_APP UINT32_C(1)
#define BOOT_DIAG_HEALTH_ETHERNET UINT32_C(2)
#define BOOT_DIAG_HEALTH_FTP UINT32_C(4)
#define BOOT_DIAG_HEALTH_MASK UINT32_C(7)

typedef enum {
    BOOT_DIAG_STAGING = 0,
    BOOT_DIAG_PUBLISHED = 1
} boot_diag_validity_t;
typedef enum {
    BOOT_DIAG_STAGE_RESET = 0, BOOT_DIAG_STAGE_METADATA = 1,
    BOOT_DIAG_STAGE_VALIDATION = 2, BOOT_DIAG_STAGE_STARTUP = 3,
    BOOT_DIAG_STAGE_TRIAL = 4, BOOT_DIAG_STAGE_RECOVERY = 5
} boot_diag_stage_t;
typedef enum {
    BOOT_DIAG_FAULT_NONE = 0, BOOT_DIAG_FAULT_METADATA = 1,
    BOOT_DIAG_FAULT_VALIDATION = 2, BOOT_DIAG_FAULT_STARTUP = 3,
    BOOT_DIAG_FAULT_TRIAL = 4, BOOT_DIAG_FAULT_RECOVERY = 5
} boot_diag_fault_t;
typedef enum {
    BOOT_DIAG_DETAIL_NONE = 0, BOOT_DIAG_DETAIL_NO_METADATA = 1,
    BOOT_DIAG_DETAIL_CONFLICT = 2, BOOT_DIAG_DETAIL_CRC = 3,
    BOOT_DIAG_DETAIL_EXHAUSTED = 4, BOOT_DIAG_DETAIL_MANIFEST = 5,
    BOOT_DIAG_DETAIL_TARGET = 6, BOOT_DIAG_DETAIL_SIGNATURE = 7,
    BOOT_DIAG_DETAIL_HASH = 8, BOOT_DIAG_DETAIL_BOUNDS = 9,
    BOOT_DIAG_DETAIL_VECTORS = 10, BOOT_DIAG_DETAIL_STARTUP_UNKNOWN = 11,
    BOOT_DIAG_DETAIL_APP = 12, BOOT_DIAG_DETAIL_ETHERNET = 13,
    BOOT_DIAG_DETAIL_FTP = 14, BOOT_DIAG_DETAIL_WATCHDOG = 15,
    BOOT_DIAG_DETAIL_EXTERNAL = 16, BOOT_DIAG_DETAIL_BROWNOUT = 17,
    BOOT_DIAG_DETAIL_DEADLINE = 18, BOOT_DIAG_DETAIL_COMBINED = 19,
    BOOT_DIAG_DETAIL_NO_RECOVERY = 20
} boot_diag_detail_t;
typedef enum {
    BOOT_DIAG_OK = 0, BOOT_DIAG_ERROR_ARGUMENT, BOOT_DIAG_ERROR_SIZE,
    BOOT_DIAG_ERROR_MAGIC, BOOT_DIAG_ERROR_VERSION, BOOT_DIAG_ERROR_LENGTH,
    BOOT_DIAG_ERROR_VALIDITY, BOOT_DIAG_ERROR_RESERVED,
    BOOT_DIAG_ERROR_STAGE, BOOT_DIAG_ERROR_FAULT, BOOT_DIAG_ERROR_DETAIL,
    BOOT_DIAG_ERROR_SLOT, BOOT_DIAG_ERROR_COUNT, BOOT_DIAG_ERROR_HEALTH
} boot_diag_result_t;

/* In-memory values, NOT a wire struct. Raw uint32 enum fields allow validation
 * of untrusted values. No stage/fault/detail pair restrictions are approved. */
typedef struct {
    uint32_t stage, fault, detail, reset_flags;
    uint32_t running_slot, selected_slot, sequence, trial_id;
    uint32_t failures, health, failed_slot;
} boot_diag_t;

/* Evidence availability is caller-owned context, NEVER serialized. Missing
 * numeric evidence has no approved wire sentinel. In particular zero and max
 * sequence/trial are ordinary values. Context must describe the same snapshot.
 * Stage/reset/health can also lack trustworthy evidence in future read glue. */
typedef struct {
    bool sequence_known, trial_id_known, failures_known;
    bool stage_known, reset_flags_known, health_known;
} boot_diag_context_t;
typedef struct { bool available; uint32_t value; } boot_diag_optional_u32_t;
typedef struct { bool available; uint32_t slot; } boot_diag_optional_slot_t;

/* Spans must cover their stated size; inputs/outputs must not overlap (as in
 * boot_format). Fixed 64-byte prefix only; tails ignored/unchanged. All errors
 * leave outputs unchanged. Header/reserved fields are canonicalized on encode.
 * Decode requires the EXACT requested validity: PUBLISHED for readers,
 * STAGING only for offline fixtures. Neither API performs publication. */
boot_diag_result_t boot_diag_validate(const boot_diag_t *record);
boot_diag_result_t boot_diag_encode(const boot_diag_t *record,
    boot_diag_validity_t validity, uint8_t *wire, size_t capacity);
boot_diag_result_t boot_diag_decode(const uint8_t *wire, size_t size,
    boot_diag_validity_t required_validity, boot_diag_t *out);

/* Boot fixture initialization only: raw captured flags retained, slots absent,
 * no inferred roles/sequence/trial/count. Numeric zeros are placeholders whose
 * unavailability MUST remain in context. This is not a reserved-RAM writer. */
boot_diag_result_t boot_diag_init(uint32_t captured_flags, boot_diag_t *out,
    boot_diag_context_t *context);
boot_diag_optional_slot_t boot_diag_slot(uint32_t slot);
boot_diag_optional_u32_t boot_diag_sequence(const boot_diag_t *record,
    const boot_diag_context_t *context);
boot_diag_optional_u32_t boot_diag_trial_id(const boot_diag_t *record,
    const boot_diag_context_t *context);
boot_diag_optional_u32_t boot_diag_failures(const boot_diag_t *record,
    const boot_diag_context_t *context);
#endif
