#include "lm3s6965/uart.h"

#include "lm3s6965/system_control.h"

#define UART_DIVISOR_FACTOR 16u
#define UART_FRACTIONAL_DIVISOR_ROUNDING_NUMERATOR 8u

/* peripheral_clock_hz is scaled by UART_FRACTIONAL_DIVISOR_SCALE in the divisor
 * math below, which would overflow uint32_t above UINT32_MAX / 64 = 67.1 MHz.
 * The 50 MHz system clock sits comfortably inside this, but the bound is enforced
 * so that a future clock change cannot silently program a garbage divisor. */
#define UART_MAXIMUM_CLOCK_HZ (0xFFFFFFFFu / UART_FRACTIONAL_DIVISOR_SCALE)

static uint32_t uart0_register_address(uint32_t register_offset)
{
    return UART0_BASE_ADDRESS + register_offset;
}

void uart0_initialize(uint32_t peripheral_clock_hz, uint32_t baud_rate)
{
    if ((peripheral_clock_hz > UART_MAXIMUM_CLOCK_HZ) || (baud_rate == 0u))
    {
        return;
    }

    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC1_OFFSET, RCGC1_UART0_BIT);
    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC2_OFFSET, RCGC2_GPIOA_BIT);

    REGISTER32(uart0_register_address(UART_CONTROL_OFFSET)) = 0u;

    const uint32_t divisor_denominator = UART_DIVISOR_FACTOR * baud_rate;
    const uint32_t divisor_scaled =
        ((peripheral_clock_hz * UART_FRACTIONAL_DIVISOR_SCALE) +
         (UART_FRACTIONAL_DIVISOR_ROUNDING_NUMERATOR * baud_rate)) / divisor_denominator;

    REGISTER32(uart0_register_address(UART_INTEGER_DIVISOR_OFFSET)) =
        divisor_scaled / UART_FRACTIONAL_DIVISOR_SCALE;
    REGISTER32(uart0_register_address(UART_FRACTIONAL_DIVISOR_OFFSET)) =
        divisor_scaled % UART_FRACTIONAL_DIVISOR_SCALE;

    REGISTER32(uart0_register_address(UART_LINE_CONTROL_OFFSET)) =
        UART_LINE_CONTROL_8_BIT_WORD | UART_LINE_CONTROL_FIFO_ENABLE;

    REGISTER32(uart0_register_address(UART_CONTROL_OFFSET)) =
        UART_CONTROL_ENABLE | UART_CONTROL_TRANSMIT_ENABLE | UART_CONTROL_RECEIVE_ENABLE;
}

void uart0_write_byte(uint8_t byte)
{
    while ((REGISTER32(uart0_register_address(UART_FLAGS_OFFSET)) &
            UART_FLAG_TRANSMIT_FIFO_FULL) != 0u)
    {
    }

    REGISTER32(uart0_register_address(UART_DATA_OFFSET)) = byte;
}

void uart0_write(const char *text)
{
    while (*text != '\0')
    {
        uart0_write_byte((uint8_t)*text);
        text++;
    }
}
