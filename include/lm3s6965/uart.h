#ifndef LM3S6965_UART_H
#define LM3S6965_UART_H

#include <stdint.h>

#include "lm3s6965/memory_map.h"

#define UART_DATA_OFFSET            0x000u
#define UART_FLAGS_OFFSET           0x018u
#define UART_INTEGER_DIVISOR_OFFSET 0x024u
#define UART_FRACTIONAL_DIVISOR_OFFSET 0x028u
#define UART_LINE_CONTROL_OFFSET    0x02Cu
#define UART_CONTROL_OFFSET         0x030u

#define UART_FLAG_TRANSMIT_FIFO_FULL (1u << 5)

#define UART_LINE_CONTROL_8_BIT_WORD (0x3u << 5)
#define UART_LINE_CONTROL_FIFO_ENABLE (1u << 4)

#define UART_CONTROL_ENABLE          (1u << 0)
#define UART_CONTROL_TRANSMIT_ENABLE (1u << 8)
#define UART_CONTROL_RECEIVE_ENABLE  (1u << 9)

#define UART_FRACTIONAL_DIVISOR_SCALE 64u

void uart0_initialize(uint32_t peripheral_clock_hz, uint32_t baud_rate);
void uart0_write_byte(uint8_t byte);
void uart0_write(const char *text);

#endif
