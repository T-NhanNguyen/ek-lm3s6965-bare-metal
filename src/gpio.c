#include "lm3s6965/gpio.h"

#include "lm3s6965/memory_map.h"

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
