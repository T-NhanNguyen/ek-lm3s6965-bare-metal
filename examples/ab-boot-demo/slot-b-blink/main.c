#include <stdbool.h>
#include <stdint.h>

#include "blink_pattern.h"
#include "led.h"
#include "lm3s6965/boot_startup.h"

/* Bounded software work, not a calibrated delay. Keep the hold present at -Os.
 * Never claim a blink timing or period from the nominal CPU clock. */
static void hold_slice(void)
{
    for (uint32_t i = 0u; i < BLINK_PATTERN_HOLD_ITERATIONS; ++i) {
        __asm__ volatile ("nop");
    }
}

int main(void)
{
    /* Application_Reset_Handler already validated inherited continuity/clock
     * before ordinary data/BSS/constructors. No PLL, timer or IRQ changes here. */
    user_led_initialize();
    user_led_write(false);
    if (!boot_startup_c_ready(&boot_startup_continuity)) {
        boot_startup_stop();
    }

    for (;;) {
        for (uint32_t phase = 0u; phase < BLINK_PATTERN_PHASES; ++phase) {
            for (uint32_t frame = 0u;
                 frame < BLINK_PATTERN_FRAMES_PER_PHASE; ++frame) {
                for (uint32_t slice = 0u; slice < BLINK_PATTERN_SLICES; ++slice) {
                    user_led_write(blink_pattern_high(phase, slice));
                    hold_slice();
                }
            }
        }
        /* Deliberately no watchdog report/feed: blink steps are not Ethernet,
         * timer service or FTP progress. This fixture is NOT hardware ready. */
    }
}
