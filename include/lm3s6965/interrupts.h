#ifndef LM3S6965_INTERRUPTS_H
#define LM3S6965_INTERRUPTS_H

/* IRQ numbers for the LM3S6965, from the datasheet interrupt table (rev I).
 * The core exceptions (Reset, NMI, HardFault...) are ARM-defined and identical
 * across all Cortex-M3 parts; everything below is Luminary/TI-specific. */

typedef enum
{
    LM3S6965_IRQ_GPIO_PORT_A     = 0,
    LM3S6965_IRQ_GPIO_PORT_B     = 1,
    LM3S6965_IRQ_GPIO_PORT_C     = 2,
    LM3S6965_IRQ_GPIO_PORT_D     = 3,
    LM3S6965_IRQ_GPIO_PORT_E     = 4,
    LM3S6965_IRQ_UART0           = 5,
    LM3S6965_IRQ_UART1           = 6,
    LM3S6965_IRQ_SSI0            = 7,
    LM3S6965_IRQ_I2C0            = 8,
    LM3S6965_IRQ_PWM_GENERATOR_0 = 10,
    LM3S6965_IRQ_PWM_GENERATOR_1 = 11,
    LM3S6965_IRQ_PWM_GENERATOR_2 = 12,
    LM3S6965_IRQ_QEI0            = 13,
    LM3S6965_IRQ_ADC0_SEQUENCE_0 = 14,
    LM3S6965_IRQ_ADC0_SEQUENCE_1 = 15,
    LM3S6965_IRQ_ADC0_SEQUENCE_2 = 16,
    LM3S6965_IRQ_ADC0_SEQUENCE_3 = 17,
    LM3S6965_IRQ_WATCHDOG        = 18,
    LM3S6965_IRQ_TIMER0A         = 19,
    LM3S6965_IRQ_TIMER0B         = 20,
    LM3S6965_IRQ_TIMER1A         = 21,
    LM3S6965_IRQ_TIMER1B         = 22,
    LM3S6965_IRQ_TIMER2A         = 23,
    LM3S6965_IRQ_TIMER2B         = 24,
    LM3S6965_IRQ_ANALOG_COMPARATOR_0 = 25,
    LM3S6965_IRQ_ANALOG_COMPARATOR_1 = 26,
    LM3S6965_IRQ_SYSTEM_CONTROL  = 28,
    LM3S6965_IRQ_FLASH_CONTROL   = 29,
    LM3S6965_IRQ_GPIO_PORT_F     = 30,
    LM3S6965_IRQ_GPIO_PORT_G     = 31,
    LM3S6965_IRQ_UART2           = 33,
    LM3S6965_IRQ_SSI1            = 34,
    LM3S6965_IRQ_TIMER3A         = 35,
    LM3S6965_IRQ_TIMER3B         = 36,
    LM3S6965_IRQ_I2C1            = 37,
    LM3S6965_IRQ_QEI1            = 38,
    LM3S6965_IRQ_ETHERNET        = 42
} lm3s6965_irq_t;

#define LM3S6965_CORE_EXCEPTION_COUNT 16u
#define LM3S6965_IRQ_COUNT            43u
#define LM3S6965_VECTOR_TABLE_ENTRIES (LM3S6965_CORE_EXCEPTION_COUNT + LM3S6965_IRQ_COUNT)

#define LM3S6965_NVIC_PRIORITY_BITS   3u

/* Handler names referenced by the vector table in src/startup.c. Each is defined
 * there as a weak alias of Default_Handler, so firmware may override any of them
 * by defining a function with the matching name. */
void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVCall_Handler(void);
void DebugMonitor_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

void GPIOA_Handler(void);
void GPIOB_Handler(void);
void GPIOC_Handler(void);
void GPIOD_Handler(void);
void GPIOE_Handler(void);
void GPIOF_Handler(void);
void GPIOG_Handler(void);
void UART0_Handler(void);
void UART1_Handler(void);
void UART2_Handler(void);
void SSI0_Handler(void);
void SSI1_Handler(void);
void I2C0_Handler(void);
void I2C1_Handler(void);
void PWM0_Handler(void);
void PWM1_Handler(void);
void PWM2_Handler(void);
void QEI0_Handler(void);
void QEI1_Handler(void);
void ADC0_SS0_Handler(void);
void ADC0_SS1_Handler(void);
void ADC0_SS2_Handler(void);
void ADC0_SS3_Handler(void);
void WATCHDOG_Handler(void);
void TIMER0A_Handler(void);
void TIMER0B_Handler(void);
void TIMER1A_Handler(void);
void TIMER1B_Handler(void);
void TIMER2A_Handler(void);
void TIMER2B_Handler(void);
void TIMER3A_Handler(void);
void TIMER3B_Handler(void);
void COMP0_Handler(void);
void COMP1_Handler(void);
void SYSCTL_Handler(void);
void FLASH_Handler(void);
void ETHERNET_Handler(void);

#endif
