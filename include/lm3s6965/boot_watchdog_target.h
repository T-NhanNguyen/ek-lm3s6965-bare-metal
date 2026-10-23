/* Injected target watchdog clock readiness only; no controller ownership. */
#ifndef LM3S6965_BOOT_WATCHDOG_TARGET_H
#define LM3S6965_BOOT_WATCHDOG_TARGET_H

#include <stdbool.h>

#include "lm3s6965/boot_watchdog.h"

/* Before any WDT register access: preserve RCGC0 bits, set WDT (bit 3),
 * require exact gate readback, then call gate_delay(io->context). The trusted
 * delay must return only AFTER at least three system clocks have elapsed;
 * this adapter cannot measure or verify that timing. IO returns 0 on success.
 * Caller supplies stable truthful callbacks, exclusive clock-gate ownership,
 * unchanged system clock and no callback reentry or concurrent gate changes.
 * No state/context is retained, no WDT registers are accessed, and protection
 * is neither configured nor serviced. Caller MUST stop on false: no configure,
 * retry or cleanup through this adapter; failed IO may already have effects.
 * Success establishes readiness only while the caller keeps the gate/clock
 * stable. Controller configuration and lifetime ownership remain separate. */
bool boot_watchdog_target_clock_ready(const boot_watchdog_io_t *io,
    void (*gate_delay)(void *));

#endif
