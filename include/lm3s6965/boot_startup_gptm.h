/* Optional startup-only SPMS144I GPTM backend; no default timer selection. */
#ifndef LM3S6965_BOOT_STARTUP_GPTM_H
#define LM3S6965_BOOT_STARTUP_GPTM_H
#include "lm3s6965/boot_startup.h"
#define BOOT_STARTUP_GPTM_SOURCE UINT32_C(0x4750544d)
typedef struct {
    boot_watchdog_io_t registers;
    void (*gate_delay)(void *); /* at least three system clocks */
    uint32_t hz, timer, state, rcc, rcc2; /* state also guards/latches operations */
    boot_startup_clock_fn reader_identity; /* boot image address, not app copy */
    /* Remaining bytes in c->context are available to a future physical WDT
     * adapter; no watchdog register implementation is supplied here. */
} boot_startup_gptm_t;
_Static_assert(sizeof(boot_startup_gptm_t) <= 64, "shared callback context");
#if defined(__arm__) || defined(__thumb__)
_Static_assert(sizeof(boot_startup_gptm_t) == 40, "ARM GPTM callback layout");
#endif
/* Call AFTER boot_startup_reset clears context, BEFORE verification. Caller
 * selects timer 0..3 explicitly, supplies actual unchanged system hz, exclusive
 * ownership and trusted boot-FLASH/retained transitive IO/delay lifetime. No
 * ordinary boot BSS may be reached, including through registers.context.
 * Clock descriptor uses read below, SOURCE above, and c->context. A physical
 * producer/caller is deliberately not wired by this optional backend.
 * Claim is one-shot even on failure. Injected IO/delay callbacks must not
 * reenter claim/read/release: attempted reentry latches failure in the shared
 * state word and aborts the outer operation before further IO or publication.
 * This synchronous callback guard is not interrupt/thread synchronization. */
bool boot_startup_gptm_claim(boot_startup_gptm_t *, unsigned timer,
    uint32_t hz, const boot_watchdog_io_t *, void (*gate_delay)(void *));
int boot_startup_gptm_read(void *, boot_startup_sample_t *);
/* supported checks backend/capsule identity without IO. Release is one-shot,
 * consumed before matched configuration/status callback validation; never
 * clears TAEN on an unknown modeled/injected startup clock. */
bool boot_startup_gptm_supported(const boot_startup_t *);
bool boot_startup_gptm_release(boot_startup_t *);
/* Production volatile layer, separate from injected host register models. */
int boot_startup_gptm_direct_read(void *, uint32_t, uint32_t *);
int boot_startup_gptm_direct_write(void *, uint32_t, uint32_t);
void boot_startup_gptm_direct_delay(void *);
#endif
