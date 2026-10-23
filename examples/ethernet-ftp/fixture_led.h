#ifndef FTP_FIXTURE_LED_H
#define FTP_FIXTURE_LED_H

/* Foreground image only. Initialize while masked, before net_clock_initialize.
 * tick is called exclusively by the existing application SysTick handler. */
void fixture_led_initialize(void);
void fixture_led_tick(void);

#endif
