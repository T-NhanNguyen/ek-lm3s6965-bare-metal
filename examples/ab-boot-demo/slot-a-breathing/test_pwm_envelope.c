/* Standalone native pure-envelope check; no framework, MMIO or target build.
 * Execution belongs to test-merit-gate, not the firmware build. */
#include <assert.h>
#include <stdint.h>

#include "pwm_envelope.h"

int main(void)
{
    uint32_t previous = 0u;
    uint32_t total_high = 0u;
    for (uint32_t phase = 0u; phase < BREATHING_PWM_PHASES; ++phase) {
        const uint32_t duty = breathing_pwm_duty(phase);
        uint32_t high = 0u;
        assert(duty <= BREATHING_PWM_SLICES);
        assert(breathing_pwm_duty(phase + BREATHING_PWM_PHASES) == duty);
        if (phase > 0u) {
            if (phase <= BREATHING_PWM_SLICES) assert(duty == previous + 1u);
            else assert(duty + 1u == previous);
        }
        for (uint32_t slice = 0u; slice < BREATHING_PWM_SLICES; ++slice) {
            high += breathing_pwm_high(phase, slice) ? 1u : 0u;
        }
        assert(high == duty);
        total_high += high;
        previous = duty;
    }
    assert(previous == 1u); /* Wrap to zero is also a one-step transition. */
    assert(breathing_pwm_duty(0u) == 0u);
    assert(breathing_pwm_duty(BREATHING_PWM_SLICES) == BREATHING_PWM_SLICES);
    assert(total_high == BREATHING_PWM_PHASES * BREATHING_PWM_SLICES / 2u);
    return 0;
}
