#ifndef BOOT_BREATHING_LED_PWM_ENVELOPE_H
#define BOOT_BREATHING_LED_PWM_ENVELOPE_H

#include <stdbool.h>
#include <stdint.h>

/* Pure, periodic electrical-high duty envelope: 0..64..1, then repeat.
 * Both endpoints occur once per cycle. Active-low LEDs breathe in reverse.
 * No time, MMIO, watchdog evidence or peripheral ownership is implied. */
enum {
    BREATHING_PWM_SLICES = 64,
    BREATHING_PWM_PHASES = 128,
    BREATHING_PWM_FRAMES_PER_PHASE = 16,
    BREATHING_PWM_HOLD_ITERATIONS = 32
};

static inline uint32_t breathing_pwm_duty(uint32_t phase)
{
    phase %= BREATHING_PWM_PHASES;
    return phase <= BREATHING_PWM_SLICES ? phase :
        BREATHING_PWM_PHASES - phase;
}

/* The caller supplies a slice in [0, BREATHING_PWM_SLICES). */
static inline bool breathing_pwm_high(uint32_t phase, uint32_t slice)
{
    return slice < breathing_pwm_duty(phase);
}

#endif
