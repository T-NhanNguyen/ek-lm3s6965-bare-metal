/* Polled, single-owner Ethernet MAC/PHY driver. No ISR or lwIP dependency. */
#ifndef LM3S6965_ETHERNET_H
#define LM3S6965_ETHERNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ETHERNET_FRAME_HEADER_BYTES 14u
#define ETHERNET_FRAME_MAX_BYTES 1514u

typedef enum
{
    ETHERNET_OK = 0,
    ETHERNET_EMPTY,
    ETHERNET_BUSY,
    ETHERNET_NOT_READY,
    ETHERNET_INVALID_ARGUMENT,
    ETHERNET_TIMEOUT,
    ETHERNET_DROPPED
} ethernet_result_t;

typedef struct
{
    bool phy_link;
    bool autonegotiation_complete;
    bool full_duplex;
    bool speed_100_mbps;
    bool mac_ready;
    bool tx_busy;
    uint8_t queued_frames;
} ethernet_status_t;

typedef struct
{
    uint32_t tx_submitted;
    uint32_t tx_completed;
    uint32_t tx_error_events;
    uint32_t rx_frames;
    uint32_t rx_dropped;
    uint32_t rx_error_events;
    uint32_t rx_overrun_events;
    uint32_t rx_fifo_resets;
    uint32_t mii_timeouts;
} ethernet_stats_t;

/* cpu_clock_hz must match the current clock (1..50 MHz), held fixed thereafter.
 * mac_address is six network-order octets, nonzero and unicast. Initialization
 * resets MAC/PHY, discards queues and stats, and leaves default PHY autonegotiation
 * running. OK means initialized, not linked. All calls must be serialized. */
ethernet_result_t ethernet_initialize(uint32_t cpu_clock_hz,
                                      const uint8_t mac_address[6]);
/* Poll regularly, and before transmitting after a link change. Matches MAC duplex
 * to the negotiated PHY mode only when TX is idle. No normal-poll FIFO flushes.
 * On timeout the returned status is not ready. Counters count observed events,
 * not exact wire errors: hardware status bits can coalesce multiple events. */
ethernet_result_t ethernet_poll(ethernet_status_t *status);
/* Copies DA through payload, no preamble/FCS, 14..1514 bytes, any alignment.
 * OK means queued, not delivered. Hardware adds padding and CRC. No TX wait. */
ethernet_result_t ethernet_try_transmit(const uint8_t *frame, size_t length);
/* Reads one complete FIFO frame, strips FCS, retains padding. EMPTY consumes
 * nothing. DROPPED consumes the whole oversized/invalid frame (or resets RX if
 * its length cannot be trusted). Too-small buffers receive no partial frame.
 * length is zero except on OK. NULL frame with zero capacity discards one frame.
 * Queued RX remains readable while the link is down. */
ethernet_result_t ethernet_read_frame(uint8_t *frame, size_t capacity,
                                      size_t *length);
void ethernet_get_stats(ethernet_stats_t *stats);

#endif
