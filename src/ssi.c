/* SSI0 polling driver; GPIO routing is configured by the caller. */
#include "ssi.h"

#include <stdint.h>

#include "memory_map.h"
#include "system_control.h"

#define SSI_PRESCALE_MINIMUM 2u
#define SSI_PRESCALE_MAXIMUM 254u
#define SSI_PRESCALE_STEP 2u
#define SSI_SERIAL_CLOCK_DIVISOR_MAXIMUM 256u
#define SSI_TOTAL_DIVISOR_MAXIMUM \
    (SSI_PRESCALE_MAXIMUM * SSI_SERIAL_CLOCK_DIVISOR_MAXIMUM)

static uint32_t ssi0_register_address(uint32_t register_offset)
{
    return SSI0_BASE_ADDRESS + register_offset;
}

/* Discard unused MISO data so a full RX FIFO cannot stall transmission. */
static void ssi0_discard_received(void)
{
    while ((REGISTER32(ssi0_register_address(SSI_STATUS_OFFSET)) &
            SSI_STATUS_RECEIVE_FIFO_NOT_EMPTY) != 0u)
    {
        (void)REGISTER32(ssi0_register_address(SSI_DATA_OFFSET));
    }
}

void ssi0_initialize(uint32_t system_clock_hz, uint32_t bitrate_hz)
{
    if ((system_clock_hz == 0u) || (bitrate_hz == 0u) ||
        (bitrate_hz > system_clock_hz / SSI_PRESCALE_MINIMUM))
    {
        return;
    }

    /* Ceiling division avoids overspeed and overflowing clock + bitrate - 1. */
    const uint32_t required_divisor = system_clock_hz / bitrate_hz +
        ((system_clock_hz % bitrate_hz != 0u) ? 1u : 0u);
    if (required_divisor > SSI_TOTAL_DIVISOR_MAXIMUM)
    {
        return;
    }

    uint32_t best_divisor = SSI_TOTAL_DIVISOR_MAXIMUM + 1u;
    uint32_t clock_prescale = 0u;
    uint32_t serial_clock_rate = 0u;
    for (uint32_t prescale = SSI_PRESCALE_MINIMUM;
         prescale <= SSI_PRESCALE_MAXIMUM; prescale += SSI_PRESCALE_STEP)
    {
        const uint32_t serial_divisor = required_divisor / prescale +
            ((required_divisor % prescale != 0u) ? 1u : 0u);
        const uint32_t divisor = prescale * serial_divisor;
        if ((serial_divisor <= SSI_SERIAL_CLOCK_DIVISOR_MAXIMUM) &&
            (divisor < best_divisor))
        {
            best_divisor = divisor;
            clock_prescale = prescale;
            serial_clock_rate = serial_divisor - 1u;
        }
    }

    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC1_OFFSET,
                                           RCGC1_SSI0_BIT);
    /* Three system clocks must pass after the gate write commits. */
    __asm__ volatile ("nop\n\tnop\n\tnop");

    ssi0_disable();
    REGISTER32(ssi0_register_address(SSI_CONTROL_1_OFFSET)) = 0u;
    REGISTER32(ssi0_register_address(SSI_CLOCK_PRESCALE_OFFSET)) = clock_prescale;
    REGISTER32(ssi0_register_address(SSI_CONTROL_0_OFFSET)) =
        (serial_clock_rate << SSI_CONTROL_0_SERIAL_CLOCK_RATE_SHIFT) |
        SSI_CONTROL_0_CLOCK_PHASE | SSI_CONTROL_0_CLOCK_POLARITY |
        SSI_CONTROL_0_8_BIT_WORD;
    ssi0_enable();
}

void ssi0_write_byte(uint8_t byte)
{
    ssi0_discard_received();
    while ((REGISTER32(ssi0_register_address(SSI_STATUS_OFFSET)) &
            SSI_STATUS_TRANSMIT_FIFO_NOT_FULL) == 0u)
    {
    }

    REGISTER32(ssi0_register_address(SSI_DATA_OFFSET)) = byte;
}

void ssi0_wait_idle(void)
{
    ssi0_discard_received();
    while ((REGISTER32(ssi0_register_address(SSI_STATUS_OFFSET)) &
            SSI_STATUS_BUSY) != 0u)
    {
        ssi0_discard_received();
    }
}

void ssi0_enable(void)
{
    REGISTER32(ssi0_register_address(SSI_CONTROL_1_OFFSET)) |= SSI_CONTROL_1_ENABLE;
}

void ssi0_disable(void)
{
    REGISTER32(ssi0_register_address(SSI_CONTROL_1_OFFSET)) &= ~SSI_CONTROL_1_ENABLE;
}
