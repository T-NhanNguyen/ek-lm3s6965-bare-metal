#ifndef LM3S6965_GPIO_H
#define LM3S6965_GPIO_H

#include <stdint.h>

typedef enum
{
    GPIO_DATA_OFFSET                      = 0x000,
    GPIO_DIRECTION_OFFSET                 = 0x400,
    GPIO_ALTERNATE_FUNCTION_SELECT_OFFSET = 0x420,
    GPIO_PULL_UP_SELECT_OFFSET            = 0x510,
    GPIO_PULL_DOWN_SELECT_OFFSET          = 0x514,
    GPIO_DIGITAL_ENABLE_OFFSET            = 0x51C,
    GPIO_LOCK_OFFSET                      = 0x520,
    GPIO_COMMIT_OFFSET                    = 0x524
} gpio_register_offset_t;

/* Pull resistor selection for gpio_configure_input(). */
typedef enum
{
    GPIO_PULL_DISABLED = 0,
    GPIO_PULL_UP,
    GPIO_PULL_DOWN
} gpio_pull_t;

/* One mask bit per port pin: GPIO_PIN(1) addresses pin 1 of the given port.
 * AFSEL, PUR, PDR, DEN, and CR are all 8-bit fields covering pins 0-7. */
#define GPIO_PIN(number) (1u << (number))

/* GPIODATA aliases address bits [9:2] as a pin mask, so data is read and
 * written at port_base_address + (pin_mask << GPIO_ADDRESS_MASK_SHIFT).
 * Accessing port_base_address + GPIO_DATA_OFFSET uses a zero mask, which
 * reads every bit as 0 and writes no bits. */
#define GPIO_ADDRESS_MASK_SHIFT 2u

/* GPIO_UNLOCK_KEY unlocks GPIOCR for writing; any other value re-locks it. */
#define GPIO_UNLOCK_KEY 0x1ACCE551u
#define GPIO_LOCK_KEY   0x00000000u

/* PB7 and PC[3:0] are gated by the GPIOCR commit register, which resets locked,
 * so gpio_select_alternate_function() silently no-ops on those five pins. */
void gpio_select_alternate_function(uint32_t port_base_address, uint32_t pin_mask);
void gpio_select_protected_alternate_function(uint32_t port_base_address, uint32_t pin_mask);
void gpio_enable_digital_function(uint32_t port_base_address, uint32_t pin_mask);

/* Configures the masked pins as digital inputs with the given pull resistor. */
void gpio_configure_input(uint32_t port_base_address, uint32_t pin_mask, gpio_pull_t pull);

/* Returns the masked pin levels. */
uint32_t gpio_read_pins(uint32_t port_base_address, uint32_t pin_mask);

/* GPIOPUR and GPIOPDR are mutually exclusive per pin: a write of 1 to
 * GPIOPDR[n] clears the pull-up on that pin, and the reverse also holds. */
void gpio_enable_pull_up(uint32_t port_base_address, uint32_t pin_mask);
void gpio_enable_pull_down(uint32_t port_base_address, uint32_t pin_mask);

#endif
