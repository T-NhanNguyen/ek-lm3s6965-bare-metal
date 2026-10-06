/* Serialized foreground-only raw-frame PoC, not a network stack.
 * No ISR, no unsolicited TX, no ARP/IP responder. Invalid traffic is silently
 * consumed. Drain at most eight frames per pass, even with a pending reply or
 * after the boot budget is spent. Sustained traffic can still overrun hardware.
 * One owned pending buffer; accepted requests reserve one of 32 boot slots,
 * even if submission later fails. BUSY retries: at most eight calls, once per
 * 1 ms tick; any other TX error immediately releases the buffer. No replay
 * cache: duplicate valid requests consume slots too. OK is submitted, NOT
 * delivered. RX scratch never owns the pending TX data.
 * Poll every 5 observed 1 ms ticks, status every 1000. COUNTFLAG coalesces ticks;
 * bounded driver waits and blocking PoC console output extend wall-clock time.
 * No long delay loops in the service loop, no hard real-time timing claim.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "ethernet.h"
#include "memory_map.h"
#include "system_control.h"
#include "trace.h"
#include "uart.h"
#include "raw_protocol.h"

#define REPLY_LIMIT 32u
#define BUSY_ATTEMPTS 8u
#define RX_DRAIN_LIMIT 8u
#define SYSTICK_CONTROL (SYSTICK_BASE_ADDRESS + 0x000u)

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
    const uint32_t clock_hz = pll_locked ? SYSTEM_CLOCK_FREQUENCY_HZ :
        EXTERNAL_CRYSTAL_FREQUENCY_HZ;
    uart0_initialize(clock_hz, 115200u);
    trace_initialize(clock_hz, 1000000u);
    REGISTER32(SYSTICK_BASE_ADDRESS + 0x004u) = clock_hz / 1000u - 1u;
    REGISTER32(SYSTICK_BASE_ADDRESS + 0x008u) = 0u;
    REGISTER32(SYSTICK_CONTROL) = (1u << 0) | (1u << 2);
    printf("\nLM3S6965 Ethernet RAW frame diagnostic (not link evaluator)\n");
    printf("clock pll=%s cpu_hz=%u rcc=0x%08X\n",
           pll_locked ? "locked" : "TIMEOUT_xtal_fallback",
           (unsigned)clock_hz, (unsigned)system_control_read_rcc());
    printf("console uart_baud=115200 swo_baud=1000000\n");
    printf("raw mac=02:00:00:69:65:02 identity=test-only type=0x88B5 "
           "local-experiment length=60 reply_limit=32 busy_attempts=8\n");
    const ethernet_result_t init = ethernet_initialize(clock_hz, raw_board_mac);
    printf("raw init=%s\n", result_name(init));
    printf("Raw diagnostic bring-up complete (console marker only)\n");

    uint8_t rx[RAW_FRAME_BYTES], pending_frame[RAW_FRAME_BYTES];
    bool pending = false;
    uint32_t accepted = 0u, submitted = 0u, busy = 0u, exhausted = 0u;
    uint32_t tx_errors = 0u, rx_dropped = 0u, rx_errors = 0u, poll_errors = 0u;
    uint32_t admission_drops = 0u, ticks = 0u, attempts = 0u;
    uint32_t pending_sequence = 0u;
    ethernet_status_t status = {0};
    ethernet_result_t poll = ethernet_poll(&status);
    if (poll != ETHERNET_OK) { ++poll_errors; }
    for (;;)
    {
        const bool tick = (REGISTER32(SYSTICK_CONTROL) & (1u << 16)) != 0u;
        if (tick)
        {
            ++ticks;
            if (ticks % 5u == 0u)
            {
                poll = ethernet_poll(&status);
                if (poll != ETHERNET_OK) { ++poll_errors; }
            }
            if (pending)
            {
                const ethernet_result_t tx =
                    ethernet_try_transmit(pending_frame, sizeof pending_frame);
                ++attempts;
                if (tx == ETHERNET_OK)
                {
                    ++submitted;
                    pending = false;
                    printf("raw reply submitted seq=%u length=60 submitted=%u "
                           "(not delivered)\n", (unsigned)pending_sequence,
                           (unsigned)submitted);
                }
                else if (tx == ETHERNET_BUSY)
                {
                    ++busy;
                    if (attempts >= BUSY_ATTEMPTS)
                    {
                        ++exhausted;
                        pending = false;
                    }
                }
                else
                {
                    ++tx_errors;
                    pending = false;
                    printf("raw reply submission error=%s\n", result_name(tx));
                }
            }
            if (ticks % 1000u == 0u)
            {
                ethernet_stats_t stats;
                ethernet_get_stats(&stats);
                printf("raw init=%s poll=%s link=%s negotiation=%s mac_ready=%u\n",
                       result_name(init), result_name(poll),
                       poll == ETHERNET_OK ? (status.phy_link ? "up" : "down") : "unknown",
                       poll == ETHERNET_OK ? (status.autonegotiation_complete ? "complete" : "pending") : "unknown",
                       (unsigned)status.mac_ready);
                printf("raw accepted=%u submitted=%u pending=%u admission_drops=%u "
                       "busy=%u exhausted=%u tx_errors=%u rx_dropped=%u "
                       "rx_errors=%u poll_errors=%u\n",
                       (unsigned)accepted, (unsigned)submitted, (unsigned)pending,
                       (unsigned)admission_drops, (unsigned)busy, (unsigned)exhausted,
                       (unsigned)tx_errors, (unsigned)rx_dropped,
                       (unsigned)rx_errors, (unsigned)poll_errors);
                printf("raw driver tx_error_events=%u rx_error_events=%u "
                       "rx_overrun_events=%u rx_fifo_resets=%u\n",
                       (unsigned)stats.tx_error_events, (unsigned)stats.rx_error_events,
                       (unsigned)stats.rx_overrun_events, (unsigned)stats.rx_fifo_resets);
            }
        }
        for (unsigned i = 0u; i < RX_DRAIN_LIMIT; ++i)
        {
            size_t length = 0u;
            const ethernet_result_t read = ethernet_read_frame(rx, sizeof rx, &length);
            if (read == ETHERNET_EMPTY || read == ETHERNET_NOT_READY) { break; }
            if (read == ETHERNET_DROPPED) { ++rx_dropped; continue; }
            if (read != ETHERNET_OK) { ++rx_errors; break; }
            raw_message_t message;
            if (!raw_parse_request(rx, length, &message)) { continue; }
            if (pending || accepted >= REPLY_LIMIT) { ++admission_drops; continue; }
            if (!raw_build_reply(rx, length, pending_frame, sizeof pending_frame))
            {
                ++rx_errors;
                continue;
            }
            pending = true;
            attempts = 0u;
            pending_sequence = message.sequence;
            ++accepted;
            printf("raw request accepted seq=%u nonce=", (unsigned)message.sequence);
            for (unsigned n = 0u; n < RAW_NONCE_BYTES; ++n)
            {
                printf("%02X", (unsigned)message.nonce[n]);
            }
            printf(" length=%u accepted=%u\n", (unsigned)length, (unsigned)accepted);
        }
    }
}
