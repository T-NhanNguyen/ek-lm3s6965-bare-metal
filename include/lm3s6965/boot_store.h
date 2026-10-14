/* Synchronous commit-last metadata primitive; no target flash driver. */
#ifndef LM3S6965_BOOT_STORE_H
#define LM3S6965_BOOT_STORE_H
#include "lm3s6965/boot_format.h"

/* Callbacks return zero only on complete success. Failure may still have
 * physical effects. read must report errors, not substitute fabricated bytes.
 * program_word takes a numeric LE word, aligned, and may only clear bits.
 * erase_page erases exactly 1024 bytes. All requests stay in metadata bounds.
 * Caller exclusively owns both pages, adapter/context and all arguments for
 * the entire call: no ISR, concurrency, reentry, or callback mutation. No
 * allocation or persistent RAM state. Inputs and outputs must not overlap. */
typedef struct
{
    void *context;
    int (*read)(void *context, uint32_t address, uint8_t *bytes, size_t size);
    int (*erase_page)(void *context, uint32_t address);
    int (*program_word)(void *context, uint32_t address, uint32_t word);
} boot_store_adapter_t;

typedef struct
{
    boot_metadata_t record;
    unsigned page; /* codec-selected storage representative, not image slot */
} boot_store_snapshot_t;

typedef enum
{
    BOOT_STORE_OK = 0,
    BOOT_STORE_ARGUMENT,
    BOOT_STORE_INVALID_RECORD,
    BOOT_STORE_NO_RECORD,
    BOOT_STORE_CONFLICT,
    BOOT_STORE_EXHAUSTED,
    BOOT_STORE_SEQUENCE,
    BOOT_STORE_STALE,
    BOOT_STORE_IO,
    BOOT_STORE_VERIFY,
    BOOT_STORE_UNCERTAIN
} boot_store_result_t;

/* No initialization/factory fallback. Outputs unchanged on every failure. */
boot_store_result_t boot_store_load(const boot_store_adapter_t *adapter,
                                    boot_store_snapshot_t *out);
/* expected is a previously loaded snapshot. desired must be codec-valid and
 * exactly expected.sequence+1 (no wrap). Re-read/check current before erase.
 * Erase ONLY the alternate page; preserve the authoritative old page, even
 * after success. No retries, fallback erase or policy/lifecycle transforms.
 * IO/VERIFY before commit leave old physical page untouched under adapter
 * contract. Once commit callback is invoked ANY failure is UNCERTAIN: reload
 * before deciding old/new/recovery; never infer rollback from callback error.
 * Success requires complete durable readback and authoritative codec selection.
 * Readback cannot protect against a dishonest adapter or later corruption. */
boot_store_result_t boot_store_replace(const boot_store_adapter_t *adapter,
                                       const boot_store_snapshot_t *expected,
                                       const boot_metadata_t *desired,
                                       boot_store_snapshot_t *out);
#endif
