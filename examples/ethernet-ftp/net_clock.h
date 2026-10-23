#ifndef FTP_NET_CLOCK_H
#define FTP_NET_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

/* CPU hz must divide exactly to 1 kHz and LOAD=hz/1000-1 must fit 24 bits.
 * SysTick is exclusively owned in both modes. Standalone may reinitialize,
 * resetting sys_now and reload; foreground initialization is one-shot.
 * Foreground requires masked privileged Thread C_READY and the explicit GPTM
 * backend. It masks/clears external NVIC sources (kept masked), starts SysTick,
 * validates the final inherited sample, commits the offset, then unmasks IRQs.
 * No RCC/PLL/WDT changes. Foreground failure is terminal at the caller, not retryable.
 * sys_now is modular milliseconds from the NEW SysTick origin, NOT inherited
 * GPTM time. Aligned word reads may observe either side of an ISR increment.
 * Initial phase is <1 ms at first tick; startup offset deliberately overlaps
 * start-to-final-sample time, biasing future entry-relative deadlines early.
 * Short masked transition is required: pending ticks coalesce; arbitrary
 * masking/missed tick loss is NOT recovered and no strict physical reset
 * deadline is promised. Owner/service latency remains a separate obligation. */
bool net_clock_initialize(uint32_t cpu_clock_hz);
uint32_t sys_now(void);
#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
/* Immutable conservative ceil entry-relative elapsed through final sample;
 * includes pre-C->C and switch gap, plus <1 us truncation allowance. Separate
 * from sys_now; reserved for future candidate-deadline composition, NOT
 * implemented here. A deadline consumer must handle range/modular limits.
 * Earlier verification is in the GPTM's protected origin but precedes entry;
 * immutable dispatch_us/entry_us retain that provenance without restarting WDT. */
bool net_clock_startup_elapsed(uint32_t *ms);
#endif
void SysTick_Handler(void);
/* Modular elapsed comparison: interval < 2^31, observation gap < 2^31 ms.
 * Invalid intervals fail closed. No catch-up loop for missed foreground work. */
bool net_clock_elapsed(uint32_t now, uint32_t since, uint32_t interval);
#endif
