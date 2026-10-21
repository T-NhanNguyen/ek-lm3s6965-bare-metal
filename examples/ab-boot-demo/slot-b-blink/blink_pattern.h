#ifndef AB_BOOT_DEMO_BLINK_PATTERN_H
#define AB_BOOT_DEMO_BLINK_PATTERN_H

#include <stdbool.h>
#include <stdint.h>

/* Pure, periodic dual-blink on/off pattern: nominal 100/100/100/500 ms shape
 * (ON, OFF, ON, OFF x5 over 8 phases), then repeat. Both endpoints occur per
 * cycle (fully-on and fully-off phases). Active-low LEDs show inverted levels
 * but the same timing shape. Mirrors pwm_envelope.h granularity (64 slices,
 * 16 frames per phase, 32-iteration hold) so slot-B wiring matches slot A.
 * No time, MMIO, watchdog evidence or peripheral ownership is implied. */
enum {
    BLINK_PATTERN_SLICES = 64,
    BLINK_PATTERN_PHASES = 8,
    BLINK_PATTERN_FRAMES_PER_PHASE = 16,
    BLINK_PATTERN_HOLD_ITERATIONS = 32
};

static inline uint32_t blink_pattern_duty(uint32_t phase)
{
    phase %= BLINK_PATTERN_PHASES;
    return (phase == 0u || phase == 2u) ? BLINK_PATTERN_SLICES : 0u;
}

/* The caller supplies a slice in [0, BLINK_PATTERN_SLICES). */
static inline bool blink_pattern_high(uint32_t phase, uint32_t slice)
{
    return slice < blink_pattern_duty(phase);
}

#endif
