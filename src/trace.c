#include "lm3s6965/trace.h"

#include <stdbool.h>

#include "lm3s6965/memory_map.h"

static bool g_trace_enabled = false;

static uint32_t trace_register_address(uint32_t base_address, uint32_t register_offset)
{
    return base_address + register_offset;
}

void trace_initialize(uint32_t system_clock_hz, uint32_t swo_baud_rate)
{
    if ((swo_baud_rate == 0u) || (system_clock_hz < swo_baud_rate))
    {
        return;
    }

    const uint32_t prescaler = (system_clock_hz / swo_baud_rate) - 1u;

    if (prescaler > TRACE_MAXIMUM_PRESCALER)
    {
        return;
    }

    const uint32_t debug_monitor_address =
        trace_register_address(SCB_BASE_ADDRESS, TRACE_DEBUG_MONITOR_CONTROL_OFFSET);
    const uint32_t itm_lock_access_address =
        trace_register_address(ITM_BASE_ADDRESS, TRACE_ITM_LOCK_ACCESS_OFFSET);
    const uint32_t tpiu_lock_access_address =
        trace_register_address(TPIU_BASE_ADDRESS, TRACE_TPIU_LOCK_ACCESS_OFFSET);

    /* TRCENA must be set before any other trace register is programmed. */
    REGISTER32(debug_monitor_address) |= TRACE_CORE_ENABLE_BIT;

    REGISTER32(itm_lock_access_address) = TRACE_CORESIGHT_UNLOCK_KEY;
    REGISTER32(tpiu_lock_access_address) = TRACE_CORESIGHT_UNLOCK_KEY;

    REGISTER32(trace_register_address(ITM_BASE_ADDRESS, TRACE_ITM_ENABLE_OFFSET)) =
        TRACE_STIMULUS_PORT_ZERO;
    REGISTER32(trace_register_address(ITM_BASE_ADDRESS, TRACE_ITM_CONTROL_OFFSET)) =
        TRACE_ITM_ENABLE_BIT;

    REGISTER32(trace_register_address(TPIU_BASE_ADDRESS, TRACE_TPIU_PORT_SIZE_OFFSET)) =
        TRACE_TPIU_ONE_PIN_PORT;
    REGISTER32(trace_register_address(TPIU_BASE_ADDRESS, TRACE_TPIU_PRESCALER_OFFSET)) =
        prescaler;
    REGISTER32(trace_register_address(TPIU_BASE_ADDRESS, TRACE_TPIU_PIN_PROTOCOL_OFFSET)) =
        TRACE_TPIU_NRZ_PROTOCOL;
    REGISTER32(trace_register_address(TPIU_BASE_ADDRESS, TRACE_TPIU_FORMATTER_OFFSET)) =
        TRACE_TPIU_CONTINUOUS_FORMATTING;

    g_trace_enabled = true;
}

void trace_write_byte(uint8_t byte)
{
    if (!g_trace_enabled)
    {
        return;
    }

    const uint32_t stimulus_port_address =
        trace_register_address(ITM_BASE_ADDRESS, TRACE_STIMULUS_PORT_ZERO_OFFSET);

    uint32_t waited_cycles = 0u;

    while (((REGISTER32(stimulus_port_address) & TRACE_STIMULUS_PORT_READY_BIT) == 0u) &&
           (waited_cycles < TRACE_STIMULUS_READY_WAIT_LIMIT))
    {
        waited_cycles++;
    }

    if (waited_cycles >= TRACE_STIMULUS_READY_WAIT_LIMIT)
    {
        return;
    }

    REGISTER32(stimulus_port_address) = (uint32_t)byte;
}
