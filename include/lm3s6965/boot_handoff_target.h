/* LM3S6965 genuine-reset, boot-only checked handoff adapter. */
#ifndef LM3S6965_BOOT_HANDOFF_TARGET_H
#define LM3S6965_BOOT_HANDOFF_TARGET_H

#include <stdint.h>
#include "lm3s6965/boot_handoff.h"
#include "lm3s6965/boot_startup.h"

/* Narrow synchronous IO seam. Production uses MRS, CPSID i, DSB/ISB and MMIO.
 * EXCLUDE means CPSID i followed by DSB; ISB. SYNC means DSB; ISB.
 * Injected callbacks must not reenter, mutate descriptors, or alias state. */
typedef enum {
    BOOT_HANDOFF_TARGET_EXCLUDE,
    BOOT_HANDOFF_TARGET_SYNC
} boot_handoff_target_barrier_t;
typedef struct {
    void *context;
    int (*read_register)(void *, uint32_t, uint32_t *);
    int (*write_register)(void *, uint32_t, uint32_t);
    int (*observe)(void *, boot_handoff_context_t *);
    int (*barrier)(void *, boot_handoff_target_barrier_t);
} boot_handoff_target_io_t;

/* Private-by-contract boot-local scratch; never place in retained continuity or
 * let retained callbacks reach it. Initial witnesses survive failed admission. */
typedef struct {
    boot_handoff_target_io_t io;
    boot_startup_t *continuity;
    boot_handoff_context_t original;
    uint32_t enables[2], pending[2], active[2], icsr, systick;
    uint32_t next, initialized, busy, failed, recorded, vtor, epoch;
    boot_handoff_intent_t intent;
} boot_handoff_target_t;

/* Actual CPU observation, output unchanged on failure; HOST returns failure.
 * Requires privileged Thread, PRIMASK=1; preserves observed original masks. */
int boot_handoff_target_observe(boot_handoff_context_t *);
/* Zero-initialize fresh unpublished target ONCE. handoff must also be fresh.
 * Binds handoff callbacks. NULL io selects real hardware, never host simulation.
 * No continuity reads/IO here: call BEFORE the sequence's producer initializes
 * continuity. All objects/descriptors/context extents must be disjoint. Caller
 * guarantees genuine reset through reviewed boot-only startup, exclusive
 * hardware/storage ownership and stable clock through dispatch; not chainload,
 * debugger adoption or demo-peripheral startup. No retry/reinitialization.
 * Boot-local scratch/handoff live through dispatch only; retained producer IO
 * must remain boot-flash + retained-context-only, independent of this adapter.
 * Returns 0 on success, -1 on failure. No branch or ownership change here. */
int boot_handoff_target_init(boot_handoff_target_t *, boot_startup_t *,
    const boot_handoff_target_io_t *, boot_handoff_t *);
/* Strict sequence values: zero for every non-VTOR event; SET_VTOR requires
 * an authenticated slot base. IRQ ranges describe enum event semantics, not
 * callback values. Physical bank1 includes IRQ43 (mask 0xfff), a superset of
 * the logical IRQ32..42 event. Any callback error
 * poisons the adapter; exclusion is never released. WDT intentionally remains
 * live (IRQ18 disabled); expiry/reset can still occur, including during checks.
 * Never feeds/acks WDT/Timer2, writes HIB, resets gates or reinitializes PLL.
 * Terminal only records a fully checked normalized intent after watchdog
 * transfer. Actual execution remains boot_startup_prepare/dispatch's job. */
int boot_handoff_target_operation(void *, boot_handoff_operation_t, uint32_t);
int boot_handoff_target_terminal_intent(void *, const boot_handoff_intent_t *);

#endif
