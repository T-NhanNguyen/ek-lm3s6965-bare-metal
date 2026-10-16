/* Injected LM3S6965 flash controller; no physical MMIO binding. */
#ifndef LM3S6965_BOOT_FLASH_H
#define LM3S6965_BOOT_FLASH_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "lm3s6965/boot_store.h"

#define BOOT_FLASH_FMA UINT32_C(0x400fd000)
#define BOOT_FLASH_FMD UINT32_C(0x400fd004)
#define BOOT_FLASH_FMC UINT32_C(0x400fd008)
#define BOOT_FLASH_FCRIS UINT32_C(0x400fd00c)
#define BOOT_FLASH_FCMISC UINT32_C(0x400fd014)
#define BOOT_FLASH_USECRL UINT32_C(0x400fe140)

typedef struct
{
    void *context;
    int (*read_register)(void *context, uint32_t address, uint32_t *value);
    int (*write_register)(void *context, uint32_t address, uint32_t value);
    int (*read_flash)(void *context, uint32_t address, uint8_t *bytes, size_t size);
} boot_flash_io_t;

typedef enum
{
    BOOT_FLASH_OK = 0,
    BOOT_FLASH_ARGUMENT,
    BOOT_FLASH_BUSY,
    BOOT_FLASH_BLOCKED,
    BOOT_FLASH_IO,
    BOOT_FLASH_CLOCK,
    BOOT_FLASH_TIMEOUT,
    BOOT_FLASH_STATUS,
    BOOT_FLASH_PROTECTED,
    BOOT_FLASH_VERIFY,
    BOOT_FLASH_BITS,
    BOOT_FLASH_DONE
} boot_flash_result_t;

typedef struct
{
    boot_flash_io_t io;
    uint32_t mhz, polls;
    atomic_flag entered;
    unsigned blocked;
    /* First cause of the current blocked episode; preserved by BLOCKED reads.
     * Valid while blocked, cleared only by successful explicit recovery. */
    boot_flash_result_t blocked_result;
} boot_flash_t;

/* Initialize ONCE before publication, never to bypass a blocked operation.
 * actual_mhz is caller-established stable integral system MHz (1..50), NOT
 * inferred from reset/USECRL. Caller configures USECRL; each modification checks
 * its low byte equals actual_mhz-1. No clock writes or implicit clock discovery.
 * Callbacks are fallible: nonzero may have effects, success must be truthful.
 * No mutation of context/arguments, overlapping objects, ISR use, or independent
 * controller users. Atomic entry rejects concurrent/reentrant adapter calls;
 * it does not serialize other controller users or protect an entire store call.
 * Caller exclusively owns controller, metadata and images across transactions.
 * poll_limit bounds register reads, NOT elapsed time: flash fetch stalls can
 * defeat wall-clock bounds. No SRAM execution or physical timeout claim. */
boot_flash_result_t boot_flash_init(boot_flash_t *flash, const boot_flash_io_t *io,
                                   uint32_t actual_mhz, uint32_t poll_limit);
/* Mutable bounds [metadata0, slot B end); boot/spare are never writable.
 * Caller authorizes the specific page/word; these primitives establish no
 * lifecycle authority. Program rejects 0->1 and verifies exact LE readback;
 * erase verifies all 1024 bytes. No retry, cancellation, MERASE or COMT. */
boot_flash_result_t boot_flash_program(boot_flash_t *flash, uint32_t address, uint32_t word);
boot_flash_result_t boot_flash_erase(boot_flash_t *flash, uint32_t address);
boot_flash_result_t boot_flash_read(boot_flash_t *flash, uint32_t address,
                                   uint8_t *bytes, size_t size);
/* After timeout/ambiguous command failure all ordinary operations are blocked.
 * Recovery ONLY observes all FMC command bits idle (bounded polls), W1C-clears
 * status and verifies clear. It never cancels/reissues a command or establishes
 * its durable outcome. Once OK, reload authoritative metadata/reconcile image
 * state BEFORE any further destructive action; never retry cached authority. */
boot_flash_result_t boot_flash_recover(boot_flash_t *flash);

/* Unchanged store callback ABI; read/erase/program restricted to the two
 * metadata pages. No write outside them even though the primitive allows slots.
 * Exclusive ownership must span the WHOLE boot_store call. */
boot_store_adapter_t boot_flash_store_adapter(boot_flash_t *flash);

typedef struct
{
    uint32_t destination;
    unsigned page, failed;
    atomic_flag entered;
} boot_flash_erase_plan_t;

/* Initialize a fresh, unpublished plan only. PRECONDITIONS: caller has verified
 * the incoming signed manifest/admission and actually-running image; snapshot
 * is authoritative durable invalidation from BOOT_STORE_OK (or authoritative
 * reload/reconciliation), NOT a lifecycle RAM proposal. Running=current
 * confirmed; destination canonical empty with no roles/confirmation/intent.
 * C structs cannot prove provenance or durability. Caller keeps authority and
 * both images exclusively stable through the entire erase/upload; no concurrent
 * metadata change, plan mutation/reinitialization, or independent flash writer.
 * This performs structural checks only and NO metadata operation. */
boot_flash_result_t boot_flash_erase_begin(boot_flash_erase_plan_t *plan,
    const boot_store_snapshot_t *invalidated, uint32_t running, uint32_t destination);
/* One verified page per call: old manifest FIRST, then payload ascending.
 * OK means one page erased; DONE means no command (all 110 pages done).
 * Any admitted step error permanently stops the plan, no retry/advance.
 * Argument rejection before entry or BUSY reentry does not mutate the plan.
 * A blocked controller still requires recovery and authoritative reconciliation;
 * only a newly authorized plan may restart, again manifest first. */
boot_flash_result_t boot_flash_erase_step(boot_flash_t *flash, boot_flash_erase_plan_t *plan);

#endif
