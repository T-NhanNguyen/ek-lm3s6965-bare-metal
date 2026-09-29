#ifndef LM3S6965_SWITCH_H
#define LM3S6965_SWITCH_H

#include <stdbool.h>
#include <stdint.h>

#include "gpio.h"
#include "memory_map.h"

#define USER_DIRECTIONSW_PORT_BASE_ADDRESS GPIO_PORT_E_BASE_ADDRESS
#define USER_SELECTSW_PORT_BASE_ADDRESS    GPIO_PORT_F_BASE_ADDRESS

#define USER_UPSW_PIN     0u
#define USER_DOWNSW_PIN   1u
#define USER_LEFTSW_PIN   2u
#define USER_RIGHTSW_PIN  3u
#define USER_SELECTSW_PIN 1u

#define USER_UPSW_PIN_MASK     GPIO_PIN(USER_UPSW_PIN)
#define USER_DOWNSW_PIN_MASK   GPIO_PIN(USER_DOWNSW_PIN)
#define USER_LEFTSW_PIN_MASK   GPIO_PIN(USER_LEFTSW_PIN)
#define USER_RIGHTSW_PIN_MASK  GPIO_PIN(USER_RIGHTSW_PIN)
#define USER_SELECTSW_PIN_MASK GPIO_PIN(USER_SELECTSW_PIN)

void user_switch_initialize(void);
bool user_switch_is_pressed(uint32_t port_base_address, uint32_t pin_mask);

#endif
