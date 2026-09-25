#include <stdbool.h>
#include <stdio.h>

#include "lm3s6965/led.h"
#include "lm3s6965/memory_map.h"
#include "lm3s6965/system_control.h"
#include "lm3s6965/trace.h"
#include "lm3s6965/uart.h"

#define UART0_BAUD_RATE 115200u
#define SWO_BAUD_RATE   1000000u

int main(void)
{
    const bool pll_lock_acquired = system_control_configure_pll();
    const uint32_t system_clock_hz =
        pll_lock_acquired ? SYSTEM_CLOCK_FREQUENCY_HZ : EXTERNAL_CRYSTAL_FREQUENCY_HZ;

    uart0_initialize(system_clock_hz, UART0_BAUD_RATE);
    trace_initialize(system_clock_hz, SWO_BAUD_RATE);

    user_led_initialize();
    user_led_write(true);

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
    }
}
