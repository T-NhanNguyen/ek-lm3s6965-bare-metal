#include <assert.h>
#include "fixture_pattern.h"
#include "fixture_led.h"
#include "led.h"

static unsigned initializes, writes;
static bool last;
void user_led_initialize(void) { ++initializes; }
void user_led_write(bool high) { ++writes; last = high; }

int main(void)
{
    fixture_pattern_t a = {0}, b = {0};
    for (unsigned cycle = 0; cycle < 4u; ++cycle) {
        unsigned on_count = 0, brightness_sum = 0;
        for (unsigned tick = 0; tick < 2000u; ++tick) {
            unsigned before = a.density;
            unsigned brightness = tick <= 1000u ? tick : 2000u - tick;
            bool on = fixture_pattern_tick(&a, FTP_FIXTURE_BREATHE);
            assert(a.density < 1000u);
            assert(before + brightness == a.density + (on ? 1000u : 0u));
            if (tick == 0u) assert(!on);
            if (tick == 1000u) assert(on);
            on_count += on ? 1u : 0u;
            brightness_sum += brightness;
        }
        assert(brightness_sum == 1000000u && on_count == 1000u);
        assert(a.phase == 0u && a.density == 0u);
        /* Idle foreground time / arbitrary sys_now wrap causes no catch-up:
         * without delivered ticks, state remains unchanged. */
        assert(a.phase == 0u);
    }
    for (unsigned cycle = 0; cycle < 4u; ++cycle) {
        unsigned count = 0;
        for (unsigned tick = 0; tick < 800u; ++tick) {
            bool on = fixture_pattern_tick(&b, FTP_FIXTURE_DOUBLE_BLINK);
            assert(on == (tick < 100u || (tick >= 200u && tick < 300u)));
            count += on ? 1u : 0u;
        }
        assert(count == 200u && b.phase == 0u && b.density == 0u);
    }
    fixture_led_tick();
    assert(initializes == 0u && writes == 0u); /* pre-init GPIO forbidden */
    fixture_led_initialize();
    assert(initializes == 1u && writes == 1u && !last);
    fixture_pattern_t expected = {0};
    for (unsigned tick = 0; tick < 8000u; ++tick) {
        fixture_led_tick();
        assert(last == fixture_pattern_tick(&expected, LM3S6965_FTP_FIXTURE_PATTERN));
        assert(writes == tick + 2u); /* exactly one bounded write per tick */
    }
    return 0;
}
