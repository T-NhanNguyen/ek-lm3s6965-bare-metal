#include "net_clock.h"
#include "memory_map.h"

#define TICKS_PER_SECOND 1000u
#define SYSTICK_MAX_PERIOD 0x01000000u

/* Single ISR writer, aligned word loads on Cortex-M3. Volatile is compiler
 * visibility, not general synchronization. Initialization precedes enable. */
static volatile uint32_t milliseconds __attribute__((aligned(4)));

bool net_clock_initialize(uint32_t cpu_clock_hz)
{
    const uint32_t period = cpu_clock_hz / TICKS_PER_SECOND;
    if (cpu_clock_hz % TICKS_PER_SECOND != 0u || period == 0u ||
        period > SYSTICK_MAX_PERIOD)
    {
        return false;
    }
    REGISTER32(SYSTICK_BASE_ADDRESS) = 0u;
    milliseconds = 0u;
    REGISTER32(SYSTICK_BASE_ADDRESS + 4u) = period - 1u;
    REGISTER32(SYSTICK_BASE_ADDRESS + 8u) = 0u;
    /* ENABLE | TICKINT | CLKSOURCE; never reads COUNTFLAG. */
    REGISTER32(SYSTICK_BASE_ADDRESS) = 7u;
    return true;
}

void SysTick_Handler(void)
{
    ++milliseconds;
}

uint32_t sys_now(void)
{
    return milliseconds;
}

bool net_clock_elapsed(uint32_t now, uint32_t since, uint32_t interval)
{
    return interval < UINT32_C(0x80000000) &&
        (uint32_t)(now - since) >= interval;
}
