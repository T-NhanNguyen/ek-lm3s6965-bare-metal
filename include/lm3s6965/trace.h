#ifndef LM3S6965_TRACE_H
#define LM3S6965_TRACE_H

#include <stdint.h>

#include "lm3s6965/memory_map.h"

/* Register offsets. The debug monitor control register lives in the SCB, the
 * stimulus port in the ITM, and the port size, prescaler, pin protocol and
 * formatter registers in the TPIU. Both the ITM and the TPIU keep a lock
 * access register at the same offset. */
typedef enum
{
    TRACE_DEBUG_MONITOR_CONTROL_OFFSET = 0xDFC,
    TRACE_STIMULUS_PORT_ZERO_OFFSET    = 0x000,
    TRACE_ITM_ENABLE_OFFSET            = 0xE00,
    TRACE_ITM_CONTROL_OFFSET           = 0xE80,
    TRACE_ITM_LOCK_ACCESS_OFFSET       = 0xFB0,
    TRACE_TPIU_PORT_SIZE_OFFSET        = 0x004,
    TRACE_TPIU_PRESCALER_OFFSET        = 0x010,
    TRACE_TPIU_PIN_PROTOCOL_OFFSET     = 0x0F0,
    TRACE_TPIU_FORMATTER_OFFSET        = 0x304,
    TRACE_TPIU_LOCK_ACCESS_OFFSET      = 0xFB0
} trace_register_offset_t;

/* The ITM and TPIU discard register writes until this key is written to their
 * lock access register. They do so silently, with no error and no status bit. */
#define TRACE_CORESIGHT_UNLOCK_KEY 0xC5ACCE55u

#define TRACE_CORE_ENABLE_BIT    (1u << 24)
#define TRACE_ITM_ENABLE_BIT     (1u << 0)
#define TRACE_STIMULUS_PORT_ZERO (1u << 0)

/* The stimulus port register reads 1 in bit 0 while the ITM can accept a write.
 * A write while it reads 0 is dropped, which loses console characters. */
#define TRACE_STIMULUS_PORT_READY_BIT (1u << 0)

/* Upper bound on the wait for the ITM to drain, so a stopped TPIU cannot hang
 * the firmware. At 1 Mbaud one two-byte ITM packet drains in about 20 us. */
#define TRACE_STIMULUS_READY_WAIT_LIMIT 1000000u

#define TRACE_TPIU_ONE_PIN_PORT          0x1u
#define TRACE_TPIU_NRZ_PROTOCOL          0x2u
#define TRACE_TPIU_CONTINUOUS_FORMATTING (1u << 8)

/* The prescaler field is 16 bits wide. */
#define TRACE_MAXIMUM_PRESCALER 0xFFFFu

/* Reads the console over SWO needs a debug transport that frees the TDO pin,
 * so the probe must be in SWD mode. See the README. */
void trace_initialize(uint32_t system_clock_hz, uint32_t swo_baud_rate);
void trace_write_byte(uint8_t byte);

#endif
