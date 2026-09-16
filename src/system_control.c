#include "lm3s6965/system_control.h"

#include "lm3s6965/memory_map.h"

static uint32_t system_control_register_address(uint32_t register_offset)
{
    return SYSTEM_CONTROL_BASE_ADDRESS + register_offset;
}

static void spin_delay(uint32_t iterations)
{
    volatile uint32_t remaining = iterations;

    while (remaining > 0u)
    {
        remaining--;
    }
}

void system_control_enable_peripheral_clock(uint32_t rc_register_offset, uint32_t bit_mask)
{
    const uint32_t register_address = system_control_register_address(rc_register_offset);

    REGISTER32(register_address) |= bit_mask;

    /* Read back so the write commits before the peripheral is touched. */
    (void)REGISTER32(register_address);
}

uint32_t system_control_wait_for_pll_lock(uint32_t timeout_iterations)
{
    uint32_t remaining = timeout_iterations;

    while (remaining > 0u)
    {
        if ((REGISTER32(system_control_register_address(SYSTEM_CONTROL_RIS_OFFSET)) &
             SYSTEM_CONTROL_RIS_PLL_LOCK) != 0u)
        {
            break;
        }

        remaining--;
    }

    return remaining;
}

uint32_t system_control_read_rcc(void)
{
    return REGISTER32(system_control_register_address(SYSTEM_CONTROL_RCC_OFFSET));
}

bool system_control_configure_pll(void)
{
    const uint32_t rcc_address = system_control_register_address(SYSTEM_CONTROL_RCC_OFFSET);

    /* Keep running from the oscillator with the PLL bypassed while reconfiguring,
     * otherwise the core would lose its clock mid-sequence. */
    uint32_t rcc = REGISTER32(rcc_address);
    rcc |= RCC_BYPASS;
    rcc &= ~RCC_USESYSDIV;
    rcc &= ~RCC_MOSCDIS;
    REGISTER32(rcc_address) = rcc;

    spin_delay(MAIN_OSCILLATOR_STARTUP_DELAY_ITERATIONS);

    rcc = (rcc & ~(RCC_XTAL_MASK | RCC_OSCSRC_MASK)) | RCC_XTAL_8_000_MHZ | RCC_OSCSRC_MAIN;
    REGISTER32(rcc_address) = rcc;

    /* Clear any stale lock status before powering the PLL up, then release
     * PWRDN. The 1 -> 0 transition is what raises PLLLRIS. */
    REGISTER32(system_control_register_address(SYSTEM_CONTROL_MISC_OFFSET)) =
        SYSTEM_CONTROL_MISC_PLL_LOCK_CLEAR;

    rcc &= ~RCC_PWRDN;
    REGISTER32(rcc_address) = rcc;

    if (system_control_wait_for_pll_lock(PLL_LOCK_TIMEOUT_ITERATIONS) == 0u)
    {
        /* BYPASS is still set, so the core keeps running from the main
         * oscillator instead of switching to a PLL that never locked. */
        return false;
    }

    /* Divisor first, then drop BYPASS to hand the core over to the PLL. */
    rcc = (rcc & ~RCC_SYSDIV_MASK) |
          RCC_SYSDIV_FOR_DIVISOR(SYSTEM_CLOCK_DIVISOR) |
          RCC_USESYSDIV;
    REGISTER32(rcc_address) = rcc;

    rcc &= ~RCC_BYPASS;
    REGISTER32(rcc_address) = rcc;

    /* Let the new divider take effect before any timing-sensitive peripheral. */
    spin_delay(PLL_DIVIDER_SETTLE_DELAY_ITERATIONS);

    return true;
}
