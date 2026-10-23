/* Injected watchdog ownership/progress policy; no MMIO or ISR binding. */
#ifndef LM3S6965_BOOT_WATCHDOG_H
#define LM3S6965_BOOT_WATCHDOG_H

#include <stdint.h>
#include <stdatomic.h>

#include "lm3s6965/boot_verify.h"

#define BOOT_WATCHDOG_LOAD UINT32_C(0x40000000)
#define BOOT_WATCHDOG_CTL UINT32_C(0x40000008)
#define BOOT_WATCHDOG_ICR UINT32_C(0x4000000c)
#define BOOT_WATCHDOG_RIS UINT32_C(0x40000010)
#define BOOT_WATCHDOG_LOCK UINT32_C(0x40000c00)
#define BOOT_WATCHDOG_RCGC0 UINT32_C(0x400fe100)

typedef struct
{
    void *context;
    int (*read_register)(void *context, uint32_t address, uint32_t *value);
    int (*write_register)(void *context, uint32_t address, uint32_t value);
} boot_watchdog_io_t;

typedef enum
{
    BOOT_WATCHDOG_OK = 0,
    BOOT_WATCHDOG_ARGUMENT,
    BOOT_WATCHDOG_BUSY,
    BOOT_WATCHDOG_BLOCKED,
    BOOT_WATCHDOG_STATE,
    BOOT_WATCHDOG_PROGRESS,
    BOOT_WATCHDOG_IO,
    BOOT_WATCHDOG_VERIFY
} boot_watchdog_result_t;

typedef enum
{
    BOOT_WATCHDOG_BOOT = 1,
    BOOT_WATCHDOG_APPLICATION = 2
} boot_watchdog_owner_t;

#define BOOT_WATCHDOG_ETHERNET UINT32_C(1)
#define BOOT_WATCHDOG_TIMERS UINT32_C(2)
#define BOOT_WATCHDOG_FTP UINT32_C(4)

typedef struct
{
    boot_watchdog_io_t io;
    atomic_flag entered;
    unsigned configured, blocked;
    boot_watchdog_owner_t owner;
    uint32_t epoch, evidence, reload;
    uint32_t verification, total, completed;
    unsigned phase;
} boot_watchdog_t;

/* Initialize ONCE, fresh unpublished storage only; never reset this object to
 * escape quarantine or adopt a live controller. All later calls require init.
 * Foreground only, exclusive controller ownership and stable truthful callbacks;
 * callbacks return 0 on success, nonzero MAY have effects. No mutation/aliasing
 * of objects/inputs, independent register users, ISR calls or clock transitions.
 * Atomic entry rejects callback reentry, not external controller access. */
boot_watchdog_result_t boot_watchdog_init(boot_watchdog_t *watchdog,
                                         const boot_watchdog_io_t *io);
/* Boot must configure BEFORE verification. Caller establishes actual stable
 * system clock, enabled RCGC0.WDT and its readiness. reload is an explicitly
 * chosen nonzero finite register budget, nominally actual_system_hz * 5 for
 * 5 seconds/stage, 10 seconds TOTAL; this arithmetic is not cycle-exact timing.
 * This bounded policy requires that nominal budget to fit uint32. No discovery
 * from a 50 MHz macro. Reject already enabled CTL (including INTEN-only): CTL
 * cannot be repaired after INTEN. No adopting unknown inherited hardware.
 * Any admitted IO/readback failure quarantines until hardware reset; best-effort
 * relock never disables protection and never converts failure into success. */
boot_watchdog_result_t boot_watchdog_configure(boot_watchdog_t *watchdog,
    uint32_t actual_system_hz, uint32_t reload);
/* Each report names current owner/epoch (initial epoch 1). Successful service
 * consumes evidence and advances epoch; transfer also advances/discards it.
 * No wrap: exhausted epoch quarantines. Struct fields are inspectable, not
 * security/provenance tokens; caller must never modify them after init. */
boot_watchdog_result_t boot_watchdog_service(boot_watchdog_t *watchdog,
    boot_watchdog_owner_t owner, uint32_t epoch);
/* Single one-way boot->application transfer, NO register writes or feed; same
 * enabled controller/counter continues. No old-owner service, no reconfigure.
 * Application confirmation has no effect on this lifetime protection. This
 * software transition is NOT the physical MSP/VTOR/clock/startup handoff. */
boot_watchdog_result_t boot_watchdog_transfer(boot_watchdog_t *watchdog,
                                            uint32_t epoch);
/* Caller reports actual verifier callbacks only. BEGIN uses a new monotonically
 * increasing nonzero verification ID and checked total; it discards pending
 * boot evidence and provides NONE. AUTHENTICATED is accepted once after BEGIN,
 * completed=0: a successfully finished signature operation. PAYLOAD_PROGRESS
 * must advance by exactly the next verifier chunk (min(remaining,
 * BOOT_VERIFY_CHUNK_BYTES)) of successfully read+hashed bytes.
 * Same ID/total throughout a stream. New BEGIN may abandon a failed stream.
 * No evidence inside nonrestartable ECDSA; callbacks are not blind permission.
 * Epoch binds evidence to service, ID/cumulative bytes reject stream replay.
 * One completed milestone suffices for boot; IDs do not wrap/recycle. */
boot_watchdog_result_t boot_watchdog_boot_report(boot_watchdog_t *watchdog,
    uint32_t epoch, uint32_t verification, boot_verify_phase_t phase,
    uint32_t completed, uint32_t total);
/* Report exactly one predicate, once per epoch, only AFTER its bounded
 * foreground work returns successfully: Ethernet poll/drain (including idle),
 * all due network timers serviced (including none due), FTP state-machine work
 * serviced (including no client/idle). work_complete must equal 1. ALL THREE
 * required before app feed. Not traffic, startup health or confirmation bits. */
boot_watchdog_result_t boot_watchdog_application_report(boot_watchdog_t *watchdog,
    uint32_t epoch, uint32_t predicate, unsigned work_complete);

#endif
