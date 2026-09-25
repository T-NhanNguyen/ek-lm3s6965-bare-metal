#include "lm3s6965/led.h"

#include "lm3s6965/gpio.h"
#include "lm3s6965/memory_map.h"
#include "lm3s6965/system_control.h"

void user_led_initialize(void)
{
    // Every write to your desired port is discarded unless the gate is enabled for that port (GPIOF_BIT)
    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC2_OFFSET, RCGC2_GPIOF_BIT);
    // Making AFSEL it disabled for this GPIO by clearing bit
    REGISTER32(USER_LED_PORT_BASE_ADDRESS + GPIO_ALTERNATE_FUNCTION_SELECT_OFFSET) &= ~USER_LED_PIN_MASK;
    // Enable the digital buffer by enables the digital buffer
    REGISTER32(USER_LED_PORT_BASE_ADDRESS + GPIO_DIGITAL_ENABLE_OFFSET) |= USER_LED_PIN_MASK;
    // Configure as an output
    REGISTER32(USER_LED_PORT_BASE_ADDRESS + GPIO_DIRECTION_OFFSET) |= USER_LED_PIN_MASK;
}

void user_led_write(bool high)
{
    const uint32_t masked_data_address = USER_LED_PORT_BASE_ADDRESS + (USER_LED_PIN_MASK << GPIO_ADDRESS_MASK_SHIFT);
    REGISTER32(masked_data_address) = high ? USER_LED_PIN_MASK : 0u;
}
