#ifndef FTP_FIXTURE_PATTERN_H
#define FTP_FIXTURE_PATTERN_H

#include <stdbool.h>
#include <stdint.h>

#define FTP_FIXTURE_BREATHE 1u
#define FTP_FIXTURE_DOUBLE_BLINK 2u

typedef struct {
    uint16_t phase;
    uint16_t density;
} fixture_pattern_t;

/* Zero-initialize before use. One call represents ONE delivered 1 ms tick,
 * not elapsed wall time: missed/coalesced interrupts stretch the pattern.
 * No catch-up, timestamps, division, delays or hardware/clock ownership. */
bool fixture_pattern_tick(fixture_pattern_t *state, unsigned pattern);

#endif
