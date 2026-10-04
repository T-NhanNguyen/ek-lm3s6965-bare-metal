/* OLED panel bring-up and full-screen pixel transport over SSI0. */
#ifndef LM3S6965_OLED_H
#define LM3S6965_OLED_H

#include <stdint.h>

/* Owns SSI0 and PA2/PA3/PA5/PC6/PC7; uses 1 MHz mode-3 SPI.
 * Requires a system clock supported by ssi0_initialize() at 1 MHz. */
void oled_initialize(uint32_t system_clock_hz);
/* Requires initialization; clears all 128 x 96 pixels and waits for completion. */
void oled_clear_screen(void);
/* Requires initialization; streams MSB-first 1 bpp rows at the panel top.
 * Width must be 128, height <= 96, stride >= 16 bytes; image must contain
 * height rows at that stride. NULL or invalid dimensions cause no I/O.
 * Source 1 is provisionally bright, 0 black; unused bottom rows are black.
 * No framebuffer is allocated; waits for completion before returning. */
void oled_draw_image(const uint8_t *image, uint16_t image_width,
                     uint16_t image_height, uint8_t bytes_per_row);

#endif
