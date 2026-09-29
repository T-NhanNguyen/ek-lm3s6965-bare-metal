#include <stdbool.h>
#include <stdio.h>

#include "switch.h"
#include "led.h"
#include "memory_map.h"
#include "system_control.h"
#include "trace.h"
#include "uart.h"

#define UART0_BAUD_RATE 115200u
#define SWO_BAUD_RATE   1000000u

#define MILLISECONDS_PER_SECOND 1000u

#define SYSTICK_CONTROL_OFFSET   0x000u
#define SYSTICK_RELOAD_OFFSET    0x004u
#define SYSTICK_CURRENT_OFFSET   0x008u
#define SYSTICK_ENABLE_BIT       (1u << 0)
#define SYSTICK_CLOCK_SOURCE_BIT (1u << 2)
#define SYSTICK_COUNT_FLAG_BIT   (1u << 16)

#define SWITCH_PRESS_DELAY_MS 500u
#define LED_BLINK_INTERVAL_MS 100u

#define USER_UP_BLINK_COUNT    1u
#define USER_DOWN_BLINK_COUNT  2u
#define USER_LEFT_BLINK_COUNT  3u
#define USER_RIGHT_BLINK_COUNT 4u

static void systick_initialize(uint32_t system_clock_hz)
{
    REGISTER32(SYSTICK_BASE_ADDRESS + SYSTICK_RELOAD_OFFSET) =
        (system_clock_hz / MILLISECONDS_PER_SECOND) - 1u;
    REGISTER32(SYSTICK_BASE_ADDRESS + SYSTICK_CURRENT_OFFSET) = 0u;
    REGISTER32(SYSTICK_BASE_ADDRESS + SYSTICK_CONTROL_OFFSET) =
        SYSTICK_ENABLE_BIT | SYSTICK_CLOCK_SOURCE_BIT;
}

static void delay_milliseconds(uint32_t milliseconds)
{
    /* Clear a stale COUNTFLAG so the first wait is a full millisecond. */
    (void)REGISTER32(SYSTICK_BASE_ADDRESS + SYSTICK_CONTROL_OFFSET);

    for (uint32_t elapsed = 0u; elapsed < milliseconds; elapsed++)
    {
        while ((REGISTER32(SYSTICK_BASE_ADDRESS + SYSTICK_CONTROL_OFFSET) &
                SYSTICK_COUNT_FLAG_BIT) == 0u)
        {
        }
    }
}

static void blink_status_led(uint32_t blink_count)
{
    for (uint32_t blink = 0u; blink < blink_count; blink++)
    {
        user_led_write(true);
        delay_milliseconds(LED_BLINK_INTERVAL_MS);
        user_led_write(false);
        delay_milliseconds(LED_BLINK_INTERVAL_MS);
    }
}

int main(void)
{
    const bool pll_lock_acquired = system_control_configure_pll();
    const uint32_t system_clock_hz =
        pll_lock_acquired ? SYSTEM_CLOCK_FREQUENCY_HZ : EXTERNAL_CRYSTAL_FREQUENCY_HZ;

    uart0_initialize(system_clock_hz, UART0_BAUD_RATE);
    trace_initialize(system_clock_hz, SWO_BAUD_RATE);
    systick_initialize(system_clock_hz);

    user_led_initialize();
    user_led_write(false);
    user_switch_initialize();

    printf("\n");
    printf("LM3S6965 bare-metal bring-up\n");
    printf("cpu:   cortex-m3 thumb-2, no fpu, no dsp\n");
    printf("flash: %u bytes\n", (unsigned)FLASH_SIZE_BYTES);
    printf("sram:  %u bytes\n", (unsigned)SRAM_SIZE_BYTES);
    printf("xtal:  %u Hz\n", (unsigned)EXTERNAL_CRYSTAL_FREQUENCY_HZ);
    printf("pll:   %u Hz / %u\n", (unsigned)PLL_OUTPUT_FREQUENCY_HZ,
           (unsigned)SYSTEM_CLOCK_DIVISOR);
    printf("plllck:%s\n", pll_lock_acquired ? "acquired" : "TIMEOUT, running from xtal");
    printf("sysclk:%u Hz\n", (unsigned)system_clock_hz);
    printf("rcc:   0x%08X\n", (unsigned)system_control_read_rcc());
    printf("uart0: %u 8N1\n", (unsigned)UART0_BAUD_RATE);
    printf("lm3s6965 bring-up complete\n");

    for (;;)
    {
        if (user_switch_is_pressed(USER_DIRECTIONSW_PORT_BASE_ADDRESS, USER_UPSW_PIN_MASK))
        {
            delay_milliseconds(SWITCH_PRESS_DELAY_MS);
            blink_status_led(USER_UP_BLINK_COUNT);
        }
        else if (user_switch_is_pressed(USER_DIRECTIONSW_PORT_BASE_ADDRESS, USER_DOWNSW_PIN_MASK))
        {
            delay_milliseconds(SWITCH_PRESS_DELAY_MS);
            blink_status_led(USER_DOWN_BLINK_COUNT);
        }
        else if (user_switch_is_pressed(USER_DIRECTIONSW_PORT_BASE_ADDRESS, USER_LEFTSW_PIN_MASK))
        {
            delay_milliseconds(SWITCH_PRESS_DELAY_MS);
            blink_status_led(USER_LEFT_BLINK_COUNT);
        }
        else if (user_switch_is_pressed(USER_DIRECTIONSW_PORT_BASE_ADDRESS, USER_RIGHTSW_PIN_MASK))
        {
            delay_milliseconds(SWITCH_PRESS_DELAY_MS);
            blink_status_led(USER_RIGHT_BLINK_COUNT);
        }
    }
}
