/* Opt-in physical IO only; no reset entry, clock setup or execution. */
#ifndef LM3S6965_BOOT_TARGET_IO_H
#define LM3S6965_BOOT_TARGET_IO_H

#include "lm3s6965/boot_flash.h"
#include "lm3s6965/boot_sequence.h"

/* Privileged, interrupt-excluded, exclusive boot caller only. All pointers must
 * cover valid aligned objects, disjoint from MMIO/source FLASH and from each
 * other except explicitly shared contexts. Hardware mappings must be valid and
 * readable/writable as required. Direct volatile IO cannot catch bus faults or
 * prove clock truth, durability or permission. Nonzero reports software argument
 * rejection, NOT hardware fault containment. Descriptors are stateless (NULL
 * context); use only via the existing reset/flash policies, never raw actuation.
 * See .dev-vault/design/physical-boot-io.md for exact-part evidence/limits. */
boot_reset_register_adapter_t boot_target_reset_adapter(void);
boot_flash_io_t boot_target_flash_io(void);

/* Fresh unpublished controller ONCE per boot. Validate trusted stable integral
 * 1..50 MHz in Hz and nonzero polls before MMIO; read back USECRL low byte and
 * require MHz-1. Does NOT configure timing or RCC/RCC2. Caller establishes timing
 * before this call; boot_flash checks it again before every modification.
 * No retry/reinitialization to escape failure. poll_limit is NOT elapsed time. */
boot_flash_result_t boot_target_flash_init(boot_flash_t *flash,
    uint32_t actual_hz, uint32_t poll_limit);

/* Read actual fixed A/B 160-byte manifest prefixes through boot_flash_read.
 * No decode, defaults, metadata creation or verification. Out unchanged on
 * error. Flash and images stay exclusive/stable through RUN publication.
 * Acquisition before producer is read-only; after producer consumes live budget.
 * Never use this function as a feed/progress event. */
boot_flash_result_t boot_target_images_read(boot_flash_t *flash,
    boot_select_images_t *out);

/* boot_verify_read_fn bridge: context is the original boot_flash_t*. Payload
 * intervals only, 1..256 bytes, no manifest/spare/metadata reads. Returns exactly
 * requested on BOOT_FLASH_OK, zero otherwise; no fabricated bytes or retries. */
size_t boot_target_verify_read(void *context, uint32_t address,
    uint8_t *buffer, size_t requested);

/* Offline descriptor composition ONLY, no MMIO/configuration/reset/init. Caller
 * first initializes flash and acquires images using the APIs above, and fills
 * parameters' continuity/diagnostic/actual_hz/producer_io/gate_delay explicitly.
 * Their existing values are preserved. registers/store are caller-owned backing
 * descriptors (not temporaries); all five objects must be distinct/disjoint and
 * live through synchronous boot_sequence_run, as must flash/images. This helper
 * neither validates clock provenance nor substitutes for sequence admission.
 * Returns 0 on assembly, -1 on NULL arguments without changing outputs.
 * All runtime/storage context is boot-local, never continuity->context. Retained
 * WDT/GPTM callbacks and retained layout are untouched. Success is not RUN. */
int boot_target_sequence_bind(boot_sequence_parameters_t *parameters,
    boot_flash_t *flash, const boot_select_images_t *images,
    boot_reset_register_adapter_t *registers, boot_store_adapter_t *store);

#endif
