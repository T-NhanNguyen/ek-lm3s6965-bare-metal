#ifndef LM3S6965_GPIO_H
#define LM3S6965_GPIO_H

#include <stdint.h>

#include "lm3s6965/memory_map.h"

typedef enum
{
    GPIO_ALTERNATE_FUNCTION_SELECT_OFFSET = 0x420,
    GPIO_DIGITAL_ENABLE_OFFSET            = 0x51C,
    GPIO_LOCK_OFFSET                      = 0x520,
    GPIO_COMMIT_OFFSET                    = 0x524
} gpio_register_offset_t;

/* One mask bit per port pin: GPIO_PIN(1) addresses pin 1 of the given port.
 * AFSEL, DEN, and CR are all 8-bit fields covering pins 0-7. */
#define GPIO_PIN(number) (1u << (number))

/* GPIO_UNLOCK_KEY unlocks GPIOCR for writing; any other value re-locks it. */
#define GPIO_UNLOCK_KEY 0x1ACCE551u
#define GPIO_LOCK_KEY   0x00000000u

/* PB7 and PC[3:0] are gated by the GPIOCR commit register, which resets locked,
 * so gpio_select_alternate_function() silently no-ops on those five pins. */
void gpio_select_alternate_function(uint32_t port_base_address, uint32_t pin_mask);
void gpio_select_protected_alternate_function(uint32_t port_base_address, uint32_t pin_mask);
void gpio_enable_digital_function(uint32_t port_base_address, uint32_t pin_mask);

#endif
