#include <stdint.h>
#include "lm3s6965/interrupts.h"

extern uint32_t _estack;
void Boot_Reset_Handler(void);
void Boot_Terminal_Handler(void);
typedef void (*boot_vector_t)(void);

/* Same 16 core + 43 IRQ positions as startup.c, including reserved IRQs.
 * All exceptions/IRQs terminate; no weak overrides or watchdog ISR feeding. */
__attribute__((section(".isr_vector"), used))
const boot_vector_t boot_reset_vectors[LM3S6965_VECTOR_TABLE_ENTRIES] = {
    (boot_vector_t)&_estack,
    Boot_Reset_Handler,
    Boot_Terminal_Handler, /* NMI */
    Boot_Terminal_Handler, /* HardFault */
    Boot_Terminal_Handler, /* MemManage */
    Boot_Terminal_Handler, /* BusFault */
    Boot_Terminal_Handler, /* UsageFault */
    0, 0, 0, 0,           /* 0x1C-0x28 reserved */
    Boot_Terminal_Handler, /* SVCall */
    Boot_Terminal_Handler, /* DebugMonitor */
    0,                    /* 0x34 reserved */
    Boot_Terminal_Handler, /* PendSV */
    Boot_Terminal_Handler, /* SysTick */

    Boot_Terminal_Handler, /* IRQ  0 GPIOA */
    Boot_Terminal_Handler, /* IRQ  1 GPIOB */
    Boot_Terminal_Handler, /* IRQ  2 GPIOC */
    Boot_Terminal_Handler, /* IRQ  3 GPIOD */
    Boot_Terminal_Handler, /* IRQ  4 GPIOE */
    Boot_Terminal_Handler, /* IRQ  5 UART0 */
    Boot_Terminal_Handler, /* IRQ  6 UART1 */
    Boot_Terminal_Handler, /* IRQ  7 SSI0 */
    Boot_Terminal_Handler, /* IRQ  8 I2C0 */
    Boot_Terminal_Handler, /* IRQ  9 reserved (PWM fault) */
    Boot_Terminal_Handler, /* IRQ 10 PWM0 */
    Boot_Terminal_Handler, /* IRQ 11 PWM1 */
    Boot_Terminal_Handler, /* IRQ 12 PWM2 */
    Boot_Terminal_Handler, /* IRQ 13 QEI0 */
    Boot_Terminal_Handler, /* IRQ 14 ADC0 SS0 */
    Boot_Terminal_Handler, /* IRQ 15 ADC0 SS1 */
    Boot_Terminal_Handler, /* IRQ 16 ADC0 SS2 */
    Boot_Terminal_Handler, /* IRQ 17 ADC0 SS3 */
    Boot_Terminal_Handler, /* IRQ 18 WATCHDOG */
    Boot_Terminal_Handler, /* IRQ 19 TIMER0A */
    Boot_Terminal_Handler, /* IRQ 20 TIMER0B */
    Boot_Terminal_Handler, /* IRQ 21 TIMER1A */
    Boot_Terminal_Handler, /* IRQ 22 TIMER1B */
    Boot_Terminal_Handler, /* IRQ 23 TIMER2A */
    Boot_Terminal_Handler, /* IRQ 24 TIMER2B */
    Boot_Terminal_Handler, /* IRQ 25 COMP0 */
    Boot_Terminal_Handler, /* IRQ 26 COMP1 */
    Boot_Terminal_Handler, /* IRQ 27 reserved */
    Boot_Terminal_Handler, /* IRQ 28 SYSCTL */
    Boot_Terminal_Handler, /* IRQ 29 FLASH */
    Boot_Terminal_Handler, /* IRQ 30 GPIOF */
    Boot_Terminal_Handler, /* IRQ 31 GPIOG */
    Boot_Terminal_Handler, /* IRQ 32 reserved */
    Boot_Terminal_Handler, /* IRQ 33 UART2 */
    Boot_Terminal_Handler, /* IRQ 34 SSI1 */
    Boot_Terminal_Handler, /* IRQ 35 TIMER3A */
    Boot_Terminal_Handler, /* IRQ 36 TIMER3B */
    Boot_Terminal_Handler, /* IRQ 37 I2C1 */
    Boot_Terminal_Handler, /* IRQ 38 QEI1 */
    Boot_Terminal_Handler, /* IRQ 39 reserved */
    Boot_Terminal_Handler, /* IRQ 40 reserved */
    Boot_Terminal_Handler, /* IRQ 41 reserved */
    Boot_Terminal_Handler  /* IRQ 42 ETHERNET */
};
_Static_assert(sizeof boot_reset_vectors == 59u * sizeof(boot_vector_t),
               "exact LM3S6965 vector count");
