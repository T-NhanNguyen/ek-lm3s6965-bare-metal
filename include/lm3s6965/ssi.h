/* SSI0 master transport for 8-bit, mode-3 Freescale SPI. */
#ifndef LM3S6965_SSI_H
#define LM3S6965_SSI_H

#include <stdint.h>

typedef enum
{
    SSI_CONTROL_0_OFFSET = 0x000,
    SSI_CONTROL_1_OFFSET = 0x004,
    SSI_DATA_OFFSET      = 0x008,
    SSI_STATUS_OFFSET    = 0x00C,
    SSI_CLOCK_PRESCALE_OFFSET = 0x010
} ssi_register_offset_t;

#define SSI_STATUS_TRANSMIT_FIFO_EMPTY (1u << 0)
#define SSI_STATUS_TRANSMIT_FIFO_NOT_FULL (1u << 1)
#define SSI_STATUS_RECEIVE_FIFO_NOT_EMPTY (1u << 2)
#define SSI_STATUS_RECEIVE_FIFO_FULL (1u << 3)
#define SSI_STATUS_BUSY (1u << 4)

#define SSI_CONTROL_1_ENABLE (1u << 1)
#define SSI_CONTROL_1_SLAVE (1u << 2)
#define SSI_CONTROL_0_CLOCK_PHASE (1u << 7)
#define SSI_CONTROL_0_CLOCK_POLARITY (1u << 6)
#define SSI_CONTROL_0_8_BIT_WORD 7u
#define SSI_CONTROL_0_SERIAL_CLOCK_RATE_SHIFT 8u

/* Selects the fastest representable rate <= bitrate_hz. Zero or out-of-range
 * inputs leave hardware unchanged. Pin muxing is the caller's responsibility. */
void ssi0_initialize(uint32_t system_clock_hz, uint32_t bitrate_hz);
/* Requires initialization; blocks for TX space, not transfer completion. */
void ssi0_write_byte(uint8_t byte);
/* Wait before changing external framing signals or disabling SSI0. */
void ssi0_wait_idle(void);
void ssi0_enable(void);
void ssi0_disable(void);

#endif
