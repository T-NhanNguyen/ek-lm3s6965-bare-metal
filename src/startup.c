#include <stdint.h>

#include "lm3s6965/interrupts.h"

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

int main(void);
void __libc_init_array(void);

void Reset_Handler(void);
void Default_Handler(void);

#define DEFAULT_HANDLER_ALIAS(handler_name) \
    void handler_name(void) __attribute__((weak, alias("Default_Handler")))

DEFAULT_HANDLER_ALIAS(NMI_Handler);
DEFAULT_HANDLER_ALIAS(HardFault_Handler);
DEFAULT_HANDLER_ALIAS(MemManage_Handler);
DEFAULT_HANDLER_ALIAS(BusFault_Handler);
DEFAULT_HANDLER_ALIAS(UsageFault_Handler);
DEFAULT_HANDLER_ALIAS(SVCall_Handler);
DEFAULT_HANDLER_ALIAS(DebugMonitor_Handler);
DEFAULT_HANDLER_ALIAS(PendSV_Handler);
DEFAULT_HANDLER_ALIAS(SysTick_Handler);

DEFAULT_HANDLER_ALIAS(GPIOA_Handler);
DEFAULT_HANDLER_ALIAS(GPIOB_Handler);
DEFAULT_HANDLER_ALIAS(GPIOC_Handler);
DEFAULT_HANDLER_ALIAS(GPIOD_Handler);
DEFAULT_HANDLER_ALIAS(GPIOE_Handler);
DEFAULT_HANDLER_ALIAS(GPIOF_Handler);
DEFAULT_HANDLER_ALIAS(GPIOG_Handler);
DEFAULT_HANDLER_ALIAS(UART0_Handler);
DEFAULT_HANDLER_ALIAS(UART1_Handler);
DEFAULT_HANDLER_ALIAS(UART2_Handler);
DEFAULT_HANDLER_ALIAS(SSI0_Handler);
DEFAULT_HANDLER_ALIAS(SSI1_Handler);
DEFAULT_HANDLER_ALIAS(I2C0_Handler);
DEFAULT_HANDLER_ALIAS(I2C1_Handler);
DEFAULT_HANDLER_ALIAS(PWM0_Handler);
DEFAULT_HANDLER_ALIAS(PWM1_Handler);
DEFAULT_HANDLER_ALIAS(PWM2_Handler);
DEFAULT_HANDLER_ALIAS(QEI0_Handler);
DEFAULT_HANDLER_ALIAS(QEI1_Handler);
DEFAULT_HANDLER_ALIAS(ADC0_SS0_Handler);
DEFAULT_HANDLER_ALIAS(ADC0_SS1_Handler);
DEFAULT_HANDLER_ALIAS(ADC0_SS2_Handler);
DEFAULT_HANDLER_ALIAS(ADC0_SS3_Handler);
DEFAULT_HANDLER_ALIAS(WATCHDOG_Handler);
DEFAULT_HANDLER_ALIAS(TIMER0A_Handler);
DEFAULT_HANDLER_ALIAS(TIMER0B_Handler);
DEFAULT_HANDLER_ALIAS(TIMER1A_Handler);
DEFAULT_HANDLER_ALIAS(TIMER1B_Handler);
DEFAULT_HANDLER_ALIAS(TIMER2A_Handler);
DEFAULT_HANDLER_ALIAS(TIMER2B_Handler);
DEFAULT_HANDLER_ALIAS(TIMER3A_Handler);
DEFAULT_HANDLER_ALIAS(TIMER3B_Handler);
DEFAULT_HANDLER_ALIAS(COMP0_Handler);
DEFAULT_HANDLER_ALIAS(COMP1_Handler);
DEFAULT_HANDLER_ALIAS(SYSCTL_Handler);
DEFAULT_HANDLER_ALIAS(FLASH_Handler);
DEFAULT_HANDLER_ALIAS(ETHERNET_Handler);

typedef void (*interrupt_handler_t)(void);

__attribute__((section(".isr_vector"), used))
const interrupt_handler_t g_interrupt_vector_table[LM3S6965_VECTOR_TABLE_ENTRIES] =
{
    (interrupt_handler_t)&_estack,   /* 0x00 initial main stack pointer */
    Reset_Handler,                   /* 0x04 */
    NMI_Handler,                     /* 0x08 */
    HardFault_Handler,               /* 0x0C */
    MemManage_Handler,               /* 0x10 */
    BusFault_Handler,                /* 0x14 */
    UsageFault_Handler,              /* 0x18 */
    0, 0, 0, 0,                      /* 0x1C-0x28 reserved */
    SVCall_Handler,                  /* 0x2C */
    DebugMonitor_Handler,            /* 0x30 */
    0,                               /* 0x34 reserved */
    PendSV_Handler,                  /* 0x38 */
    SysTick_Handler,                 /* 0x3C */

    GPIOA_Handler,                   /* IRQ  0 */
    GPIOB_Handler,                   /* IRQ  1 */
    GPIOC_Handler,                   /* IRQ  2 */
    GPIOD_Handler,                   /* IRQ  3 */
    GPIOE_Handler,                   /* IRQ  4 */
    UART0_Handler,                   /* IRQ  5 */
    UART1_Handler,                   /* IRQ  6 */
    SSI0_Handler,                    /* IRQ  7 */
    I2C0_Handler,                    /* IRQ  8 */
    Default_Handler,                 /* IRQ  9 reserved (PWM fault) */
    PWM0_Handler,                    /* IRQ 10 */
    PWM1_Handler,                    /* IRQ 11 */
    PWM2_Handler,                    /* IRQ 12 */
    QEI0_Handler,                    /* IRQ 13 */
    ADC0_SS0_Handler,                /* IRQ 14 */
    ADC0_SS1_Handler,                /* IRQ 15 */
    ADC0_SS2_Handler,                /* IRQ 16 */
    ADC0_SS3_Handler,                /* IRQ 17 */
    WATCHDOG_Handler,                /* IRQ 18 */
    TIMER0A_Handler,                 /* IRQ 19 */
    TIMER0B_Handler,                 /* IRQ 20 */
    TIMER1A_Handler,                 /* IRQ 21 */
    TIMER1B_Handler,                 /* IRQ 22 */
    TIMER2A_Handler,                 /* IRQ 23 */
    TIMER2B_Handler,                 /* IRQ 24 */
    COMP0_Handler,                   /* IRQ 25 */
    COMP1_Handler,                   /* IRQ 26 */
    Default_Handler,                 /* IRQ 27 reserved */
    SYSCTL_Handler,                  /* IRQ 28 */
    FLASH_Handler,                   /* IRQ 29 */
    GPIOF_Handler,                   /* IRQ 30 */
    GPIOG_Handler,                   /* IRQ 31 */
    Default_Handler,                 /* IRQ 32 reserved */
    UART2_Handler,                   /* IRQ 33 */
    SSI1_Handler,                    /* IRQ 34 */
    TIMER3A_Handler,                 /* IRQ 35 */
    TIMER3B_Handler,                 /* IRQ 36 */
    I2C1_Handler,                    /* IRQ 37 */
    QEI1_Handler,                    /* IRQ 38 */
    Default_Handler,                 /* IRQ 39 reserved */
    Default_Handler,                 /* IRQ 40 reserved */
    Default_Handler,                 /* IRQ 41 reserved */
    ETHERNET_Handler                 /* IRQ 42 */
};

void _init(void)
{
}

void Reset_Handler(void)
{
    uint32_t *source = &_sidata;
    uint32_t *destination = &_sdata;

    while (destination < &_edata)
    {
        *destination = *source;
        destination++;
        source++;
    }

    for (destination = &_sbss; destination < &_ebss; destination++)
    {
        *destination = 0u;
    }

    __libc_init_array();

    (void)main();

    for (;;)
    {
    }
}

void Default_Handler(void)
{
    for (;;)
    {
    }
}
