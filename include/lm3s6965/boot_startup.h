/* Opt-in native continuity ABI. Not a wire format or reset classifier. */
#ifndef LM3S6965_BOOT_STARTUP_H
#define LM3S6965_BOOT_STARTUP_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_diag_publication.h"
#include "lm3s6965/boot_handoff.h"
#include "lm3s6965/boot_runtime_watchdog.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"

#define BOOT_STARTUP_MAGIC UINT32_C(0x31534342)
#define BOOT_STARTUP_ABI UINT32_C(0x4d330001)
#define BOOT_STARTUP_BYTES 1024u

typedef enum { BOOT_STARTUP_EMPTY, BOOT_STARTUP_PREPARED,
    BOOT_STARTUP_DISPATCHING, BOOT_STARTUP_ENTERED, BOOT_STARTUP_C_READY,
    BOOT_STARTUP_FAILED } boot_startup_state_t;
typedef struct {
    uint64_t us;
    uint32_t rcc, rcc2, hz, source;
} boot_startup_sample_t;
/* Must read OBSERVED RCC/RCC2 plus a continuously running time source.
 * Boot-flash code and transitive helpers/literals, retained context only.
 * No peripheral mutations, ordinary BSS, allocation or libc state. */
typedef int (*boot_startup_clock_fn)(void *, boot_startup_sample_t *);
typedef struct {
    boot_startup_clock_fn read;
    void *context;
    uint32_t rcc, rcc2, rcc_mask, rcc2_mask, hz, source;
} boot_startup_clock_t;
typedef struct {
    uint32_t msp, control, ipsr, primask, basepri, faultmask, lr, vtor;
    uint32_t pc, stub, initialized;
} boot_startup_observations_t;
typedef struct {
    uint32_t magic, version, bytes, abi, state, base, reset_vector, entry_epoch;
    uint32_t hz, reload;
    uint64_t dispatch_us, entry_us;
    uint32_t active, reserved; /* serialized callback reentry guard, not atomic_flag */
    union { boot_watchdog_t watchdog; uint8_t watchdog_slot[64]; };
    union { boot_diag_publication_t publication; uint8_t publication_slot[32]; };
    union { boot_verified_image_t image; uint8_t image_slot[128]; };
    boot_handoff_intent_t intent;
    union { boot_startup_clock_t clock; uint8_t clock_slot[64]; };
    _Alignas(8) uint8_t context[64];
    union { boot_startup_observations_t observed; uint8_t observations_slot[64]; };
    uint8_t reserve[512];
} boot_startup_t;
/* Absolute linker symbol; no C definition, initialized inputs or NOINIT adoption.
 * GNU ARM object access convention, with all native offsets asserted below. */
extern boot_startup_t boot_startup_continuity;
#if defined(__arm__) || defined(__thumb__)
_Static_assert(sizeof(void *) == 4 && sizeof(boot_startup_state_t) == 4, "native ABI");
_Static_assert(sizeof(atomic_flag) == 1 && ATOMIC_BOOL_LOCK_FREE == 2, "guard ABI");
_Static_assert(sizeof(boot_watchdog_t) == 56, "watchdog ABI");
_Static_assert(sizeof(boot_diag_publication_t) == 24, "publisher ABI");
_Static_assert(sizeof(boot_verified_image_t) == 120, "image ABI");
_Static_assert(offsetof(boot_startup_t, watchdog) == 64, "watchdog offset");
_Static_assert(offsetof(boot_startup_t, publication) == 128, "publisher offset");
_Static_assert(offsetof(boot_startup_t, image) == 160, "image offset");
_Static_assert(offsetof(boot_startup_t, intent) == 288, "intent offset");
_Static_assert(offsetof(boot_startup_t, clock) == 320, "clock offset");
_Static_assert(offsetof(boot_startup_t, context) == 384, "context offset");
_Static_assert(offsetof(boot_startup_t, observed) == 448, "capture offset");
_Static_assert(sizeof(boot_startup_t) == 1024 && _Alignof(boot_startup_t) == 8, "region ABI");
#endif
/* GENUINE boot reset only, before any retained use. Invalidates diagnostic
 * readiness using fresh boot publication, never adopt. IO context must already
 * name c->context; descriptors themselves may be temporary and are copied.
 * Initialize/configure watchdog once here/at caller before real verification. */
bool boot_startup_reset(boot_startup_t *, volatile uint32_t *diagnostic,
    uint32_t reset_flags, const boot_watchdog_io_t *, const boot_startup_clock_t *);
/* Trusted boot call site ONLY, actual successful wrapper RETURN, exclusive
 * stable authenticated storage/authority and PRIMASK=1 through dispatch.
 * Released guards are a call-site postcondition: never peek atomic_flag.
 * Failure consumes capsule. Never pass a default/preflight/failed report. */
bool boot_startup_prepare(boot_startup_t *, const boot_runtime_watchdog_result_t *,
    const boot_runtime_watchdog_launch_t *);
/* Minimized composition: only immediately after real successful handoff_run
 * return on verified+authority-matched image, not a replacement verifier. */
bool boot_startup_prepare_handoff(boot_startup_t *, const boot_handoff_t *,
    boot_handoff_result_t, const boot_verified_image_t *, const boot_handoff_intent_t *);
/* One-shot final check/time capture. Exposed for deterministic failure tests;
 * production caller uses dispatch, never terminal directly. */
bool boot_startup_begin_dispatch(boot_startup_t *);
_Noreturn void boot_startup_dispatch(boot_startup_t *);
/* Pre-C, app compiled. local_stub is exact signed vector target; pc is exact
 * Application_Entry_Witness label, not an arbitrary in-payload address. */
bool boot_startup_early(boot_startup_t *, uint32_t local_stub, uint32_t witness);
/* Masked Thread publication before app networking, with validated ownership
 * and clock >= entry time; no feed. This is not the final inherited sample,
 * which is taken at the later clock transition AFTER SysTick starts. */
bool boot_startup_c_ready(boot_startup_t *);
/* Serialized application sampling through the SAME validated inherited clock.
 * Available after actual pre-C entry, including after live epochs advance,
 * but ONLY while the backend is claimed; never after its explicit release.
 * Failure quarantines continuity; no raw callback bypass or peripheral writes.
 * Caller must check monotonicity between successive samples. Output must not
 * alias retained storage or diagnostics. Does not feed or change entry origin. */
bool boot_startup_time(boot_startup_t *, uint64_t *us);
/* App-resident wrapper supplies its own executable address and live VTOR.
 * Boot helper location is never treated as evidence of app execution. */
bool boot_startup_running_at(boot_startup_t *, uint32_t wrapper_pc,
    uint32_t live_vtor, boot_verified_image_t *);
/* boot_snapshot_sources_t signature: zero on success, nonzero unavailable. */
int boot_startup_running(void *, boot_verified_image_t *);
_Noreturn void boot_startup_stop(void);
#endif
