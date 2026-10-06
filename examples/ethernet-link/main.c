/* PHY link diagnostic only. No frame transmission or network stack. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ethernet.h"
#include "memory_map.h"
#include "system_control.h"
#include "trace.h"
#include "uart.h"

#define UART0_BAUD_RATE 115200u
#define SWO_BAUD_RATE 1000000u
#define MILLISECONDS_PER_SECOND 1000u

#define SYSTICK_CONTROL_OFFSET 0x000u
#define SYSTICK_RELOAD_OFFSET 0x004u
#define SYSTICK_CURRENT_OFFSET 0x008u
#define SYSTICK_ENABLE_BIT (1u << 0)
#define SYSTICK_CLOCK_SOURCE_BIT (1u << 2)
#define SYSTICK_COUNT_FLAG_BIT (1u << 16)

/* Locally administered unicast, link-test-only. Not a production identity
 * or an assigned hardware MAC. Do not reuse this value for packet tests. */
static const uint8_t link_test_mac[6] = {0x02, 0x00, 0x00, 0x69, 0x65, 0x01};

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
    /* Same blocking delay as baremetal. No interrupt or elapsed-time clock.
     * COUNTFLAG coalesces ticks. Console and poll time extend the interval. */
    (void)REGISTER32(SYSTICK_BASE_ADDRESS + SYSTICK_CONTROL_OFFSET);
    for (uint32_t elapsed = 0u; elapsed < milliseconds; ++elapsed)
    {
        while ((REGISTER32(SYSTICK_BASE_ADDRESS + SYSTICK_CONTROL_OFFSET) &
                SYSTICK_COUNT_FLAG_BIT) == 0u)
        {
        }
    }
}

static const char *result_name(ethernet_result_t result)
{
    switch (result)
    {
        case ETHERNET_OK: return "OK";
        case ETHERNET_EMPTY: return "EMPTY";
        case ETHERNET_BUSY: return "BUSY";
        case ETHERNET_NOT_READY: return "NOT_READY";
        case ETHERNET_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case ETHERNET_TIMEOUT: return "TIMEOUT";
        case ETHERNET_DROPPED: return "DROPPED";
        default: return "UNKNOWN";
    }
}

int main(void)
{
    const bool pll_locked = system_control_configure_pll();
    const uint32_t system_clock_hz = pll_locked ? SYSTEM_CLOCK_FREQUENCY_HZ :
        EXTERNAL_CRYSTAL_FREQUENCY_HZ;

    uart0_initialize(system_clock_hz, UART0_BAUD_RATE);
    trace_initialize(system_clock_hz, SWO_BAUD_RATE);
    systick_initialize(system_clock_hz);

    printf("\nLM3S6965 Ethernet link diagnostic\n");
    printf("clock pll=%s cpu_hz=%u rcc=0x%08X\n",
           pll_locked ? "locked" : "TIMEOUT_xtal_fallback",
           (unsigned)system_clock_hz,
           (unsigned)system_control_read_rcc());
    printf("console uart_baud=%u swo_baud=%u\n",
           (unsigned)UART0_BAUD_RATE, (unsigned)SWO_BAUD_RATE);
    printf("ethernet mac=02:00:00:69:65:01 identity=link-test-only tx=none\n");

    /* Driver reset and MII waits are bounded by iteration counts. OK means
     * initialized, not linked. Keep the original result in each heartbeat. */
    const ethernet_result_t init_result =
        ethernet_initialize(system_clock_hz, link_test_mac);
    printf("ethernet init=%s\n", result_name(init_result));
    printf("Ethernet diagnostic bring-up complete. Link acceptance pending\n");

    uint32_t sample = 0u;
    uint32_t poll_errors = 0u;
    for (;;)
    {
        ethernet_status_t status = {0};
        const ethernet_result_t poll_result = ethernet_poll(&status);
        const bool valid = poll_result == ETHERNET_OK;
        const bool negotiated = valid && status.phy_link &&
            status.autonegotiation_complete;
        if (!valid)
        {
            ++poll_errors;
        }
        printf("ethernet sample=%u init=%s poll=%s poll_errors=%u "
               "link=%s negotiation=%s speed_mbps=%s duplex=%s "
               "mac_ready=%u\n",
               (unsigned)sample++, result_name(init_result),
               result_name(poll_result), (unsigned)poll_errors,
               valid ? (status.phy_link ? "up" : "down") : "unknown",
               valid ? (status.autonegotiation_complete ? "complete" :
                        "pending") : "unknown",
               negotiated ? (status.speed_100_mbps ? "100" : "10") :
                            "unknown",
               negotiated ? (status.full_duplex ? "full" : "half") :
                            "unknown",
               (unsigned)(valid && status.mac_ready));
        delay_milliseconds(MILLISECONDS_PER_SECOND);
    }
}
