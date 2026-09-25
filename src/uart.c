#include "lm3s6965/uart.h"

#include "lm3s6965/gpio.h"
#include "lm3s6965/memory_map.h"
#include "lm3s6965/system_control.h"

#define UART0_GPIO_PIN_MASK (GPIO_PIN(0) | GPIO_PIN(1))

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

    // Enable the UART0 clock gate (RCGC1)
    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC1_OFFSET, RCGC1_UART0_BIT);
    // Enable the GPIOA clock gate so PA0 and PA1 can be configured (RCGC2)
    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC2_OFFSET, RCGC2_GPIOA_BIT);

    // Route PA0 and PA1 to their UART alternate function (AFSEL)
    gpio_select_alternate_function(GPIO_PORT_A_BASE_ADDRESS, UART0_GPIO_PIN_MASK);
    // Enable the digital function on PA0 and PA1 (DEN)
    gpio_enable_digital_function(GPIO_PORT_A_BASE_ADDRESS, UART0_GPIO_PIN_MASK);

    // Disable the UART before changing the divisor and framing (CTL)
    REGISTER32(uart0_register_address(UART_CONTROL_OFFSET)) = 0u;

    // Scale the baud divisor by 64, then round to split it into integer and fractional parts
    const uint32_t divisor_denominator = UART_DIVISOR_FACTOR * baud_rate;
    const uint32_t divisor_scaled =
        ((peripheral_clock_hz * UART_FRACTIONAL_DIVISOR_SCALE) +
         (UART_FRACTIONAL_DIVISOR_ROUNDING_NUMERATOR * baud_rate)) / divisor_denominator;

    // Program the integer part of the divisor (IBRD)
    REGISTER32(uart0_register_address(UART_INTEGER_DIVISOR_OFFSET)) =
        divisor_scaled / UART_FRACTIONAL_DIVISOR_SCALE;
    // Program the fractional part of the divisor (FBRD)
    REGISTER32(uart0_register_address(UART_FRACTIONAL_DIVISOR_OFFSET)) =
        divisor_scaled % UART_FRACTIONAL_DIVISOR_SCALE;

    // Select 8-bit words with the FIFOs enabled (LCRH)
    REGISTER32(uart0_register_address(UART_LINE_CONTROL_OFFSET)) =
        UART_LINE_CONTROL_8_BIT_WORD | UART_LINE_CONTROL_FIFO_ENABLE;

    // Enable the UART with both transmit and receive (CTL)
    REGISTER32(uart0_register_address(UART_CONTROL_OFFSET)) =
        UART_CONTROL_ENABLE | UART_CONTROL_TRANSMIT_ENABLE | UART_CONTROL_RECEIVE_ENABLE;
}

void uart0_write_byte(uint8_t byte)
{
    // Wait until the transmit FIFO has room (FR)
    while ((REGISTER32(uart0_register_address(UART_FLAGS_OFFSET)) &
            UART_FLAG_TRANSMIT_FIFO_FULL) != 0u)
    {
    }

    // Write the byte to the data register (DR)
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
