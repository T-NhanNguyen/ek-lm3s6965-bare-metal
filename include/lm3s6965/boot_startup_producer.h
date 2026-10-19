/* Setup-only physical boot producer; not a verifier or production entry. */
#ifndef LM3S6965_BOOT_STARTUP_PRODUCER_H
#define LM3S6965_BOOT_STARTUP_PRODUCER_H

#include "lm3s6965/boot_startup_gptm.h"

/* Explicitly reserved full-width Timer2, not an implicit backend default. */
#define BOOT_STARTUP_PRODUCER_TIMER 2u

/* BEGIN ONLY: exactly once per genuine hardware boot reset, before verification
 * or any retained use. Never adopt a live controller, retry (even on false), or
 * call from application entry. Caller supplies trusted reset flags and ACTUAL
 * unchanged system_hz; RCC/RCC2 observations do not measure frequency. Requires
 * privileged, interrupt-excluded Thread mode, exclusive reset/default WDT and
 * Timer2 ownership, stable clock/gates, and no callback reentry/concurrent mutations.
 * In particular WDT debug STALL must have its reset/default value; this helper
 * does not establish/debug-redesign STALL policy or repair inherited hardware.
 *
 * On ARM c/diagnostic must be the linker-reserved continuity/diagnostic regions.
 * Invalid Hz (zero or 5*Hz overflow) or structural arguments return false BEFORE
 * any effect, including diagnostic invalidation. Such a return leaves possibly
 * stale retained bytes untouched, NOT usable: caller MUST stop on every false.
 * After admission reset clears retained state and publishes fresh diagnostics;
 * any later failure marks FAILED and blocks the watchdog without peripheral
 * cleanup, feed, retry, reset actuation or rollback of partially completed IO.
 *
 * Success establishes WDT gate readiness, claims Timer2, records observed full
 * RCC/RCC2 provenance and configures c->watchdog ONCE with checked 5*system_hz.
 * Pass &c->watchdog itself onward to the real verifier/runtime wrapper: no copy,
 * reinit, reconfigure, history/epoch restart or IO replacement after this call.
 * Existing GPTM matches reject subsequent RCC/RCC2 changes; no ongoing sampling
 * or WDT feed is introduced here. Success leaves startup state EMPTY, not ready
 * for dispatch: real verification/handoff/prepare remain caller obligations.
 *
 * Link this code and ALL transitive callable code/helpers/literals (including
 * compiler/libc helpers) into immutable boot FLASH [0,0x8000). Mutable callback
 * dependencies must live solely in c->context, never boot/app BSS/data, heap,
 * TLS or an old stack. Descriptors here are copied, not retained. Final consumer
 * must audit link/map/disassembly closure and fit; this API/CMake target is NOT
 * a full production boot entry or a guarantee of its placement/resource fit. */
bool boot_startup_producer_begin(boot_startup_t *c,
    volatile uint32_t *diagnostic, uint32_t reset_flags, uint32_t system_hz);

/* Same begin-only contract with existing register/delay injection conventions.
 * io->context MUST equal c->context (GPTM occupies its prefix: ARM40/HOST64).
 * IO must be stateless or use only nonoverlapping remaining retained bytes;
 * HOST64 has no remaining bytes. No external model context is retained here.
 * Descriptor must not overlap c or diagnostic64. Callbacks/delay and their
 * transitive closure must satisfy the lifetime above and MUST NOT reenter.
 * delay(io->context) must return only after at least three system clocks. */
bool boot_startup_producer_begin_io(boot_startup_t *c,
    volatile uint32_t *diagnostic, uint32_t reset_flags, uint32_t system_hz,
    const boot_watchdog_io_t *io, void (*gate_delay)(void *));

#endif
