#include "switch.h"

#include "gpio.h"
#include "system_control.h"

void user_switch_initialize(void)
{
    // Enable the clock gates for the direction switches (GPIOE) and the select switch (GPIOF)
    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC2_OFFSET, RCGC2_GPIOE_BIT);
    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC2_OFFSET, RCGC2_GPIOF_BIT);

    const uint32_t direction_switch_mask = USER_UPSW_PIN_MASK | USER_DOWNSW_PIN_MASK |
                                           USER_LEFTSW_PIN_MASK | USER_RIGHTSW_PIN_MASK;

    // The switches are active-low, so each input needs an internal pull-up
    gpio_configure_input(USER_DIRECTIONSW_PORT_BASE_ADDRESS,
                         direction_switch_mask,
                         GPIO_PULL_UP);
    gpio_configure_input(USER_SELECTSW_PORT_BASE_ADDRESS, USER_SELECTSW_PIN_MASK, GPIO_PULL_UP);
}

bool user_switch_is_pressed(uint32_t port_base_address, uint32_t pin_mask)
{
    // Active-low: the pin reads 0 while the switch is pressed
    return gpio_read_pins(port_base_address, pin_mask) == 0u;
}
