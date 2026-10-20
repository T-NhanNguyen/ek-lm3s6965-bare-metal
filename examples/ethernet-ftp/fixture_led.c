#include "fixture_led.h"
#include "fixture_pattern.h"
#include "led.h"

#if !defined(LM3S6965_FTP_FOREGROUND_OWNER) || \
    !defined(LM3S6965_FTP_FIXTURE_PATTERN)
#error "Fixture LED is local to the slot-selected FTP foreground image"
#endif
#if LM3S6965_FTP_FIXTURE_PATTERN != FTP_FIXTURE_BREATHE && \
    LM3S6965_FTP_FIXTURE_PATTERN != FTP_FIXTURE_DOUBLE_BLINK
#error "Unsupported fixture pattern"
#endif

static fixture_pattern_t pattern;
static bool ready;

void fixture_led_initialize(void)
{
    user_led_initialize(); /* PF0 only; Ethernet retains PF2/PF3. */
    user_led_write(false);
    pattern = (fixture_pattern_t){0};
    ready = true;
}

void fixture_led_tick(void)
{
    /* Defensive guard: no GPIO access before initialization. No service
     * evidence/feed, network work, timer setup or interrupt state changes. */
    if (ready) user_led_write(fixture_pattern_tick(&pattern,
        LM3S6965_FTP_FIXTURE_PATTERN));
}
