#ifndef FTP_NET_CLOCK_H
#define FTP_NET_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

/* Initialize once, before networking. CPU source must divide exactly to 1 kHz.
 * Reject invalid clocks without touching registers. Owns SysTick exclusively. */
bool net_clock_initialize(uint32_t cpu_clock_hz);
uint32_t sys_now(void);
void SysTick_Handler(void);
/* Modular elapsed comparison: interval < 2^31, observation gap < 2^31 ms.
 * Invalid intervals fail closed. No catch-up loop for missed foreground work. */
bool net_clock_elapsed(uint32_t now, uint32_t since, uint32_t interval);
#endif
