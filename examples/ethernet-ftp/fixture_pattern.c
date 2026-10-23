#include "fixture_pattern.h"

#include <stdbool.h>
#include <stdint.h>

bool fixture_pattern_tick(fixture_pattern_t *state, unsigned pattern)
{
    const unsigned phase = state->phase;
    bool on;
    if (pattern == FTP_FIXTURE_BREATHE) {
        /* Triangular 0..1000..1 envelope; 2000 delivered ticks per cycle.
         * First-order pulse density spreads ON ticks instead of a slow PWM
         * frame. Endpoints are exactly OFF at 0 and ON at 1000. */
        const unsigned brightness = phase <= 1000u ? phase : 2000u - phase;
        unsigned density = state->density + brightness;
        on = density >= 1000u;
        if (on) density -= 1000u;
        state->density = (uint16_t)density;
        state->phase = (uint16_t)(phase == 1999u ? 0u : phase + 1u);
    } else {
        on = phase < 100u || (phase >= 200u && phase < 300u);
        state->phase = (uint16_t)(phase == 799u ? 0u : phase + 1u);
    }
    return on;
}
