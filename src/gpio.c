/* GPIO configuration and masked pin access. */
#include "gpio.h"

#include <stdint.h>

#include "memory_map.h"

static uint32_t gpio_register_address(uint32_t port_base_address, uint32_t register_offset)
{
    return port_base_address + register_offset;
}

static void gpio_write_alternate_function_select(uint32_t port_base_address, uint32_t pin_mask)
{
    const uint32_t alternate_function_select_address =
        gpio_register_address(port_base_address, GPIO_ALTERNATE_FUNCTION_SELECT_OFFSET);

    REGISTER32(alternate_function_select_address) |= pin_mask;
}

static void gpio_select_pull(uint32_t port_base_address, uint32_t pin_mask, gpio_pull_t pull)
{
    const uint32_t pull_up_select_address =
        gpio_register_address(port_base_address, GPIO_PULL_UP_SELECT_OFFSET);
    const uint32_t pull_down_select_address =
        gpio_register_address(port_base_address, GPIO_PULL_DOWN_SELECT_OFFSET);

    switch (pull)
    {
        case GPIO_PULL_UP:
            REGISTER32(pull_up_select_address) |= pin_mask;
            break;
        case GPIO_PULL_DOWN:
            REGISTER32(pull_down_select_address) |= pin_mask;
            break;
        default:
            REGISTER32(pull_up_select_address) &= ~pin_mask;
            REGISTER32(pull_down_select_address) &= ~pin_mask;
            break;
    }
}

void gpio_select_alternate_function(uint32_t port_base_address, uint32_t pin_mask)
{
    gpio_write_alternate_function_select(port_base_address, pin_mask);
}

void gpio_select_protected_alternate_function(uint32_t port_base_address, uint32_t pin_mask)
{
    const uint32_t lock_address = gpio_register_address(port_base_address, GPIO_LOCK_OFFSET);
    const uint32_t commit_address = gpio_register_address(port_base_address, GPIO_COMMIT_OFFSET);

    REGISTER32(lock_address) = GPIO_UNLOCK_KEY;
    REGISTER32(commit_address) |= pin_mask;
    gpio_write_alternate_function_select(port_base_address, pin_mask);
    REGISTER32(lock_address) = GPIO_LOCK_KEY;
}

void gpio_enable_digital_function(uint32_t port_base_address, uint32_t pin_mask)
{
    const uint32_t digital_enable_address =
        gpio_register_address(port_base_address, GPIO_DIGITAL_ENABLE_OFFSET);

    REGISTER32(digital_enable_address) |= pin_mask;
}

void gpio_configure_input(uint32_t port_base_address, uint32_t pin_mask, gpio_pull_t pull)
{
    const uint32_t alternate_function_select_address =
        gpio_register_address(port_base_address, GPIO_ALTERNATE_FUNCTION_SELECT_OFFSET);
    const uint32_t direction_address =
        gpio_register_address(port_base_address, GPIO_DIRECTION_OFFSET);

    REGISTER32(alternate_function_select_address) &= ~pin_mask;
    REGISTER32(direction_address) &= ~pin_mask;
    gpio_enable_digital_function(port_base_address, pin_mask);
    gpio_select_pull(port_base_address, pin_mask, pull);
}

/* Select GPIO output mode; callers own the output latch levels. */
void gpio_configure_output(uint32_t port_base_address, uint32_t pin_mask)
{
    const uint32_t alternate_function_select_address =
        gpio_register_address(port_base_address,
                              GPIO_ALTERNATE_FUNCTION_SELECT_OFFSET);
    const uint32_t direction_address =
        gpio_register_address(port_base_address, GPIO_DIRECTION_OFFSET);

    REGISTER32(alternate_function_select_address) &= ~pin_mask;
    REGISTER32(direction_address) |= pin_mask;
    gpio_select_pull(port_base_address, pin_mask, GPIO_PULL_DISABLED);
    gpio_enable_digital_function(port_base_address, pin_mask);
}

uint32_t gpio_read_pins(uint32_t port_base_address, uint32_t pin_mask)
{
    const uint32_t masked_data_address =
        port_base_address + (pin_mask << GPIO_ADDRESS_MASK_SHIFT);

    return REGISTER32(masked_data_address);
}

void gpio_enable_pull_up(uint32_t port_base_address, uint32_t pin_mask)
{
    gpio_select_pull(port_base_address, pin_mask, GPIO_PULL_UP);
}

void gpio_enable_pull_down(uint32_t port_base_address, uint32_t pin_mask)
{
    gpio_select_pull(port_base_address, pin_mask, GPIO_PULL_DOWN);
}
