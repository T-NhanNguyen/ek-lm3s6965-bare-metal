#include "boot_clock_setup.h"

#include <stdbool.h>
#include <stdint.h>

#include "lm3s6965/boot_flash.h"
#include "lm3s6965/system_control.h"

#define RCC UINT32_C(0x400fe060)
#define RCC2 UINT32_C(0x400fe070)
#define RIS UINT32_C(0x400fe050)
#define MISC UINT32_C(0x400fe058)
/* Local LM3S6965 datasheet, July 15 2014, pp.184-185 and 200-202:
 * USERCC2 overrides RCC; BYPASS2 is bit 11. No TM4C-only fields. */
#define USERCC2 (UINT32_C(1) << 31)
#define BYPASS2 (UINT32_C(1) << 11)
_Static_assert(SYSTEM_CLOCK_FREQUENCY_HZ == 50000000u &&
               EXTERNAL_CRYSTAL_FREQUENCY_HZ == 8000000u &&
               SYSTEM_CLOCK_DIVISOR == 4u, "reviewed boot clock profile");
#define PROFILE_MASK (RCC_SYSDIV_MASK | RCC_USESYSDIV | RCC_PWRDN | \
                      RCC_BYPASS | RCC_XTAL_MASK | RCC_OSCSRC_MASK | RCC_MOSCDIS)
#define PROFILE (RCC_SYSDIV_FOR_DIVISOR(SYSTEM_CLOCK_DIVISOR) | \
                 RCC_USESYSDIV | RCC_XTAL_8_000_MHZ | RCC_OSCSRC_MAIN)

bool boot_clock_setup(const boot_clock_io_t *io)
{
    uint32_t rcc, rcc2, remaining;
    if (!io || !io->read || !io->write || !io->delay) return false;
    rcc2 = io->read(RCC2);
    /* Bypass the CURRENT authority before disabling the common divider.
     * Preserve every unrelated/reserved bit; RCC2 inactive fields stay inert. */
    if ((rcc2 & USERCC2) != 0u) {
        rcc2 |= BYPASS2;
        io->write(RCC2, rcc2);
        if (io->read(RCC2) != rcc2) return false;
    }
    rcc = io->read(RCC) | RCC_BYPASS;
    rcc &= ~(RCC_USESYSDIV | RCC_MOSCDIS);
    io->write(RCC, rcc);
    if (io->read(RCC) != rcc) return false;
    io->delay(MAIN_OSCILLATOR_STARTUP_DELAY_ITERATIONS);
    rcc = (rcc & ~(RCC_XTAL_MASK | RCC_OSCSRC_MASK)) |
          RCC_XTAL_8_000_MHZ | RCC_OSCSRC_MAIN;
    io->write(RCC, rcc);
    if (io->read(RCC) != rcc) return false;
    /* RCC is now a prepared, enabled raw MAIN oscillator, not an unready PLL.
     * Only now transfer authority from RCC2. */
    rcc2 &= ~USERCC2;
    io->write(RCC2, rcc2);
    if (io->read(RCC2) != rcc2) return false;
    io->write(MISC, SYSTEM_CONTROL_MISC_PLL_LOCK_CLEAR);
    rcc &= ~RCC_PWRDN;
    io->write(RCC, rcc);
    if (io->read(RCC) != rcc) return false;
    remaining = PLL_LOCK_TIMEOUT_ITERATIONS;
    while ((io->read(RIS) & SYSTEM_CONTROL_RIS_PLL_LOCK) == 0u) {
        if (--remaining == 0u) return false;
    }
    rcc = (rcc & ~RCC_SYSDIV_MASK) |
          RCC_SYSDIV_FOR_DIVISOR(SYSTEM_CLOCK_DIVISOR) | RCC_USESYSDIV;
    io->write(RCC, rcc);
    if (io->read(RCC) != rcc) return false;
    rcc &= ~RCC_BYPASS;
    io->write(RCC, rcc);
    io->delay(PLL_DIVIDER_SETTLE_DELAY_ITERATIONS);
    if ((io->read(RCC) & PROFILE_MASK) != PROFILE ||
        io->read(RCC2) != rcc2 ||
        (io->read(RIS) & SYSTEM_CONTROL_RIS_PLL_LOCK) == 0u) return false;
    /* Datasheet p.275: flash timing reload = operating MHz - 1. */
    io->write(BOOT_FLASH_USECRL, 49u);
    return io->read(BOOT_FLASH_USECRL) == 49u;
}
