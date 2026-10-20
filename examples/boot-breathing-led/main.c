#include "lm3s6965/boot_startup.h"
#include "led.h"
#include "pwm_envelope.h"

/* Bounded software work, not a calibrated delay. Keep the hold present at -Os.
 * Never claim a PWM frequency or breath period from the nominal CPU clock. */
static void hold_slice(void)
{
    for (uint32_t i = 0u; i < BREATHING_PWM_HOLD_ITERATIONS; ++i) {
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
        for (uint32_t phase = 0u; phase < BREATHING_PWM_PHASES; ++phase) {
            for (uint32_t frame = 0u;
                 frame < BREATHING_PWM_FRAMES_PER_PHASE; ++frame) {
                for (uint32_t slice = 0u; slice < BREATHING_PWM_SLICES; ++slice) {
                    user_led_write(breathing_pwm_high(phase, slice));
                    hold_slice();
                }
            }
        }
        /* Deliberately no watchdog report/feed: PWM is not Ethernet, timer
         * service or FTP progress. This fixture is NOT hardware ready. */
    }
}
