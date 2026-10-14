/* Ordered callback-only reset pass; no MMIO, execution or reset request. */
#ifndef LM3S6965_BOOT_RESET_HANDLER_H
#define LM3S6965_BOOT_RESET_HANDLER_H
#include "lm3s6965/boot_reset.h"
#include "lm3s6965/boot_store.h"

typedef struct
{
    void *context;
    int (*read)(void *context, uint32_t *flags);
    int (*write)(void *context, uint32_t flags);
} boot_reset_register_adapter_t;

typedef enum
{
    BOOT_RESET_HANDLER_OK = 0,
    BOOT_RESET_HANDLER_ARGUMENT,
    BOOT_RESET_HANDLER_CAPTURE_FAILED,
    BOOT_RESET_HANDLER_CLEAR_READ_FAILED,
    BOOT_RESET_HANDLER_CLEAR_WRITE_FAILED,
    BOOT_RESET_HANDLER_RECOVERY,
    BOOT_RESET_HANDLER_STORE_FAILED,
    BOOT_RESET_HANDLER_CLASSIFY_FAILED
} boot_reset_handler_result_t;

#define BOOT_RESET_REPORT_CAPTURE UINT32_C(1)
#define BOOT_RESET_REPORT_CLEAR UINT32_C(2)
#define BOOT_RESET_REPORT_DECISION UINT32_C(4)
#define BOOT_RESET_REPORT_SNAPSHOT UINT32_C(8)
#define BOOT_RESET_REPORT_STORE_RESULT UINT32_C(16)
#define BOOT_RESET_REPORT_LIFECYCLE_RESULT UINT32_C(32)

typedef struct
{
    uint32_t valid; /* Only flagged fields/steps may be used, including on error. */
    uint32_t captured_flags; /* Original unmasked capture, retained on later errors. */
    boot_reset_decision_t decision;
    boot_store_snapshot_t snapshot; /* Final authority ONLY on handler OK. */
    boot_store_result_t store_result; /* Exact load/replace result, including STALE/UNCERTAIN. */
    boot_lifecycle_result_t lifecycle_result;
} boot_reset_handler_report_t;

/* One synchronous, exclusive, non-reentrant pass per boot. Both adapters,
 * contexts, report and metadata pages remain stable/exclusively owned; no ISR,
 * concurrent writer or callback mutation/reentry. Complete truthful reads and
 * writes return zero; nonzero may have side effects (even a completed write).
 * A successful register write guarantees the requested R/W value was written;
 * there is no register readback/retry. Failure is NOT proof of no clear.
 * Validate pointers/callbacks and report overlap with adapter objects BEFORE
 * external operations. Argument errors leave report unchanged. Context storage
 * must not overlap report (caller contract: opaque sizes cannot be checked).
 * Other errors publish only the explicitly valid partial report, never success.
 *
 * CAPTURE -> CLEAR -> load authoritative metadata -> CLASSIFY capture -> one
 * store replacement/readback iff CHANGE. Metadata loading after clear satisfies
 * the spec: authority is required for classification, not for capture/clear.
 * Source: LM3S6965 Data Sheet Rev. I (July 15, 2014), printed p.194:
 * RESC base 0x400fe000 + 0x05c, SW/WDT/BOR/POR/EXT bits 4..0; p.173
 * sticky/reset-source behavior and p.175 BOR initially disabled (board unknown).
 * Exact LM3S6965 RESC bits 0..4 are R/W, NOT W1C: fresh & ~(capture & 0x1f).
 * Preserve unsupported and newly set uncaptured bits. Zero supported capture
 * skips fresh read/write; clear step is satisfied. Same-bit relatch between
 * capture and write cannot be distinguished and can be lost; this is NOT an
 * atomic hardware transaction. No new hardware cause is used to classify.
 *
 * NO_CHANGE preserves pending valid intent and makes ZERO flash writes. Count,
 * history and matching active intent consumption share one commit; history is
 * retained. Clear/read failure never reaches metadata commit. No automatic
 * replay/retry, even for ambiguous effects. On UNCERTAIN caller MUST reload
 * store authority before further action, never reuse a cached snapshot. Never
 * jump on any handler error. OK is NOT image validation/execution permission.
 * Clear-before-commit interruption can lose an event; RAM capture is not durable.
 * No exactly-once/general replay guarantee or freshness for stale matching intent.
 */
boot_reset_handler_result_t boot_reset_handler_run(
    const boot_reset_register_adapter_t *registers,
    const boot_store_adapter_t *store, boot_reset_handler_report_t *report);
#endif
