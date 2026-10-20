#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "boot_clock_setup.h"
#include "lm3s6965/system_control.h"
#include "lm3s6965/boot_flash.h"

#define RCC 0x400fe060u
#define RCC2 0x400fe070u
#define RIS 0x400fe050u
#define MISC 0x400fe058u
#define USERCC2 0x80000000u
static uint32_t rcc, rcc2, timing, writes, polls, drop_write, lock;
static uint32_t read_register(uint32_t address)
{
    switch (address) {
    case RCC: return rcc;
    case RCC2: return rcc2;
    case RIS: ++polls; return lock ? SYSTEM_CONTROL_RIS_PLL_LOCK : 0u;
    case BOOT_FLASH_USECRL: return timing;
    default: assert(0); return 0;
    }
}
static void write_register(uint32_t address, uint32_t value)
{
    ++writes;
    if (writes == drop_write) return;
    switch (address) {
    case RCC:
        /* Never disable the divider while the active authority uses PLL. */
        if (!(value & RCC_USESYSDIV))
            assert((rcc2 & USERCC2) ? (rcc2 & RCC_BYPASS) : (value & RCC_BYPASS));
        rcc = value; break;
    case RCC2:
        if ((rcc2 & USERCC2) && !(value & USERCC2))
            assert((rcc & (RCC_BYPASS | RCC_MOSCDIS | RCC_OSCSRC_MASK)) == RCC_BYPASS);
        rcc2 = value; break;
    case BOOT_FLASH_USECRL: timing = value; break;
    case MISC: assert(value == SYSTEM_CONTROL_MISC_PLL_LOCK_CLEAR); break;
    default: assert(0);
    }
}
static void delay(uint32_t iterations) { assert(iterations != 0u); }
static void reset(unsigned override)
{
    rcc = 0x078e3ac0u;
    rcc2 = 0x07802810u | (override ? USERCC2 : 0u);
    if (override) rcc2 &= ~RCC_BYPASS;
    timing = 15u; writes = polls = drop_write = 0u; lock = 1u;
}
int main(void)
{
    const boot_clock_io_t io = { read_register, write_register, delay };
    for (unsigned override = 0; override < 2; ++override) {
        reset(override);
        assert(boot_clock_setup(&io));
        assert(!(rcc2 & USERCC2));
        assert((rcc & RCC_SYSDIV_MASK) == RCC_SYSDIV_FOR_DIVISOR(4u));
        assert(!(rcc & (RCC_BYPASS | RCC_PWRDN | RCC_MOSCDIS)));
        assert(timing == 49u);
        uint32_t count = writes;
        for (uint32_t i = 1; i <= count; ++i) {
            reset(override); drop_write = i;
            /* MISC clear is modeled as a latched-lock no-op. Writes that
             * preserve existing values are not distinguishable by readback. */
            const bool success = boot_clock_setup(&io);
            if (i == count || (override && i == 1u)) assert(!success);
            if (success) {
                assert(timing == 49u && !(rcc2 & USERCC2));
                assert((rcc & RCC_SYSDIV_MASK) == RCC_SYSDIV_FOR_DIVISOR(4u));
                assert(!(rcc & (RCC_BYPASS | RCC_PWRDN | RCC_MOSCDIS)));
            }
            assert(polls <= PLL_LOCK_TIMEOUT_ITERATIONS + 1u);
        }
        reset(override); lock = 0;
        assert(!boot_clock_setup(&io));
        assert(polls == PLL_LOCK_TIMEOUT_ITERATIONS);
        assert(timing == 15u && (rcc & RCC_BYPASS));
    }
    assert(!boot_clock_setup(NULL));
    puts("boot clock setup checks passed");
    return 0;
}
