#ifndef LM3S6965_LED_H
#define LM3S6965_LED_H

#include <stdbool.h>

#include "lm3s6965/gpio.h"
#include "lm3s6965/memory_map.h"

#define USER_LED_PORT_BASE_ADDRESS  GPIO_PORT_F_BASE_ADDRESS
#define USER_LED_PIN                0u
#define USER_LED_PIN_MASK           GPIO_PIN(USER_LED_PIN)

#define GPIO_ADDRESS_MASK_SHIFT 2u

void user_led_initialize(void);
void user_led_write(bool high);

#endif
