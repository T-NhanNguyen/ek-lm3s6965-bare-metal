#ifndef LM3S6965_SYSTEM_CONTROL_H
#define LM3S6965_SYSTEM_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "lm3s6965/memory_map.h"

#define SYSTEM_CONTROL_RIS_OFFSET    0x050u
#define SYSTEM_CONTROL_MISC_OFFSET   0x058u
#define SYSTEM_CONTROL_RCC_OFFSET    0x060u
#define SYSTEM_CONTROL_RCC2_OFFSET   0x070u
#define SYSTEM_CONTROL_RCGC0_OFFSET  0x100u
#define SYSTEM_CONTROL_RCGC1_OFFSET  0x104u
#define SYSTEM_CONTROL_RCGC2_OFFSET  0x108u

/* RCC bit fields. Field positions cross-verified against TI's hw_sysctl.h
 * (SYSCTL_RCC_*) and QEMU's hw/arm/stellaris.c model. */
#define RCC_SYSDIV_SHIFT      23u
#define RCC_SYSDIV_MASK       (0xFu << RCC_SYSDIV_SHIFT)
#define RCC_USESYSDIV         (1u << 22u)
#define RCC_PWRDN             (1u << 13u)
#define RCC_BYPASS            (1u << 11u)
#define RCC_XTAL_SHIFT        6u
#define RCC_XTAL_MASK         (0x1Fu << RCC_XTAL_SHIFT)
#define RCC_OSCSRC_SHIFT      4u
#define RCC_OSCSRC_MASK       (0x3u << RCC_OSCSRC_SHIFT)
#define RCC_OSCSRC_MAIN       (0x0u << RCC_OSCSRC_SHIFT)
#define RCC_MOSCDIS           (1u << 0u)

#define RCC_XTAL_8_000_MHZ    (0xEu << RCC_XTAL_SHIFT)

#define SYSTEM_CONTROL_RIS_PLL_LOCK (1u << 6u)

/* Same bit position as RIS.PLLLRIS, but a different register: writing 1 to
 * MISC.PLLLMIS clears the latched PLL lock status. */
#define SYSTEM_CONTROL_MISC_PLL_LOCK_CLEAR (1u << 6u)

/* LM3S6965 (Fury class) PLL produces 200 MHz; SYSDIV divides it down, where
 * divisor = SYSDIV + 1. 200 / 4 = 50 MHz, so the SYSDIV field holds 3. */
#define EXTERNAL_CRYSTAL_FREQUENCY_HZ 8000000u
#define PLL_OUTPUT_FREQUENCY_HZ       200000000u
#define SYSTEM_CLOCK_FREQUENCY_HZ     50000000u
#define SYSTEM_CLOCK_DIVISOR          (PLL_OUTPUT_FREQUENCY_HZ / SYSTEM_CLOCK_FREQUENCY_HZ)
#define RCC_SYSDIV_FOR_DIVISOR(divisor) (((divisor) - 1u) << RCC_SYSDIV_SHIFT)

#define MAIN_OSCILLATOR_STARTUP_DELAY_ITERATIONS 524288u
#define PLL_DIVIDER_SETTLE_DELAY_ITERATIONS      1u
#define PLL_LOCK_TIMEOUT_ITERATIONS              32768u

#define RCGC1_UART0_BIT  (1u << 0)
#define RCGC1_UART1_BIT  (1u << 1)
#define RCGC1_UART2_BIT  (1u << 2)
#define RCGC2_GPIOA_BIT  (1u << 0)
#define RCGC2_GPIOB_BIT  (1u << 1)
#define RCGC2_GPIOC_BIT  (1u << 2)
#define RCGC2_GPIOD_BIT  (1u << 3)
#define RCGC2_GPIOE_BIT  (1u << 4)
#define RCGC2_GPIOF_BIT  (1u << 5)
#define RCGC2_GPIOG_BIT  (1u << 6)

void system_control_enable_peripheral_clock(uint32_t rc_register_offset, uint32_t bit_mask);
bool system_control_configure_pll(void);
uint32_t system_control_wait_for_pll_lock(uint32_t timeout_iterations);
uint32_t system_control_read_rcc(void);

#endif
