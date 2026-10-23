/* Standalone native pure-pattern check; no framework, MMIO or target build.
 * Execution belongs to test-merit-gate, not the firmware build. */
#include <assert.h>
#include <stdint.h>

#include "blink_pattern.h"

int main(void)
{
    uint32_t previous = 0u;
    uint32_t total_high = 0u;
    for (uint32_t phase = 0u; phase < BLINK_PATTERN_PHASES; ++phase) {
        const uint32_t duty = blink_pattern_duty(phase);
        uint32_t high = 0u;
        assert(duty <= BLINK_PATTERN_SLICES);
        assert(blink_pattern_duty(phase + BLINK_PATTERN_PHASES) == duty);
        assert(duty == 0u || duty == BLINK_PATTERN_SLICES);
        if (phase > 0u) {
            assert(duty == previous || duty + BLINK_PATTERN_SLICES == previous
                || previous + BLINK_PATTERN_SLICES == duty);
        }
        for (uint32_t slice = 0u; slice < BLINK_PATTERN_SLICES; ++slice) {
            high += blink_pattern_high(phase, slice) ? 1u : 0u;
        }
        assert(high == duty);
        total_high += high;
        previous = duty;
    }
    assert(previous == 0u); /* Last phase is off; wrap to on is a full step. */
    assert(blink_pattern_duty(0u) == BLINK_PATTERN_SLICES);
    assert(blink_pattern_duty(1u) == 0u);
    assert(blink_pattern_duty(2u) == BLINK_PATTERN_SLICES);
    assert(total_high == 2u * BLINK_PATTERN_SLICES);
    return 0;
}
