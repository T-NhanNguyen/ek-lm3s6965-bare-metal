/* LM3S6965 datasheet chapter 15. MAC FIFOs are little-endian byte streams. */
#include "ethernet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpio.h"
#include "memory_map.h"
#include "system_control.h"

#define ETHERNET_MII_WAIT_ITERATIONS 32768u
#define ETHERNET_PHY_RESET_ATTEMPTS 1024u
#define ETHERNET_FCS_BYTES 4u
#define ETHERNET_FIFO_LENGTH_BYTES 2u
#define ETHERNET_FIFO_BYTES 2048u
#define ETHERNET_MIN_RECEIVED_BYTES 60u
#define ETHERNET_MDC_DIVISOR_HZ 5000000u

typedef enum
{
    MAC_STATUS = 0x000,
    MAC_INTERRUPT_MASK = 0x004,
    MAC_RECEIVE_CONTROL = 0x008,
    MAC_TRANSMIT_CONTROL = 0x00C,
    MAC_DATA = 0x010,
    MAC_ADDRESS_0 = 0x014,
    MAC_ADDRESS_1 = 0x018,
    MAC_THRESHOLD = 0x01C,
    MAC_MII_CONTROL = 0x020,
    MAC_MII_DIVIDER = 0x024,
    MAC_MII_TRANSMIT = 0x02C,
    MAC_MII_RECEIVE = 0x030,
    MAC_PACKET_COUNT = 0x034,
    MAC_TRANSMIT_REQUEST = 0x038
} ethernet_register_offset_t;

#define MAC_RX_EVENT (1u << 0)
#define MAC_TX_ERROR (1u << 1)
#define MAC_TX_COMPLETE (1u << 2)
#define MAC_RX_OVERRUN (1u << 3)
#define MAC_RX_ERROR (1u << 4)
#define MAC_MII_COMPLETE (1u << 5)
#define MAC_RX_ENABLE (1u << 0)
#define MAC_REJECT_BAD_CRC (1u << 3)
#define MAC_RX_RESET (1u << 4)
#define MAC_TX_ENABLE (1u << 0)
#define MAC_TX_PAD (1u << 1)
#define MAC_TX_CRC (1u << 2)
#define MAC_TX_DUPLEX (1u << 4)
#define MAC_MII_START (1u << 0)
#define MAC_MII_WRITE (1u << 1)
#define MAC_MII_ADDRESS_MASK (0x1Fu << 3)
#define MAC_NEW_TX (1u << 0)
#define PHY_RESET (1u << 15)
#define PHY_LINK (1u << 2)
#define PHY_AUTONEG_COMPLETE (1u << 5)
#define PHY_RATE_100 (1u << 10)
#define PHY_FULL_DUPLEX (1u << 11)

static bool initialized;
static bool mac_ready;
static ethernet_stats_t counters;

static uint32_t mac_read(ethernet_register_offset_t offset)
{
    return REGISTER32(ETHERNET_BASE_ADDRESS + (uint32_t)offset);
}

static void mac_write(ethernet_register_offset_t offset, uint32_t value)
{
    REGISTER32(ETHERNET_BASE_ADDRESS + (uint32_t)offset) = value;
}

static void mac_update(ethernet_register_offset_t offset, uint32_t mask,
                       uint32_t value)
{
    mac_write(offset, (mac_read(offset) & ~mask) | value);
}

static void delay_cycles(uint32_t cycles)
{
    for (uint32_t cycle = 0u; cycle < cycles; ++cycle)
    {
        __asm__ volatile ("nop");
    }
}

static bool mii_wait(void)
{
    for (uint32_t attempt = 0u; attempt < ETHERNET_MII_WAIT_ITERATIONS; ++attempt)
    {
        if ((mac_read(MAC_MII_CONTROL) & MAC_MII_START) == 0u)
        {
            return true;
        }
    }
    ++counters.mii_timeouts;
    return false;
}

/* REGADR is bits 7:3, not 6:2; START and WRITE share one write (p574). */
static bool mii_transfer(uint8_t address, bool write, uint16_t *data)
{
    if (!mii_wait())
    {
        return false;
    }
    if (write)
    {
        mac_update(MAC_MII_TRANSMIT, 0xFFFFu, *data);
    }
    mac_write(MAC_STATUS, MAC_MII_COMPLETE);
    mac_update(MAC_MII_CONTROL,
               MAC_MII_ADDRESS_MASK | MAC_MII_WRITE | MAC_MII_START,
               ((uint32_t)address << 3) | MAC_MII_START |
               (write ? MAC_MII_WRITE : 0u));
    if (!mii_wait())
    {
        return false;
    }
    if (!write)
    {
        *data = (uint16_t)mac_read(MAC_MII_RECEIVE);
    }
    mac_write(MAC_STATUS, MAC_MII_COMPLETE);
    return true;
}

/* First FIFO word: payload count and DA[0:1]. Subsequent words start at DA[2]. */
static uint32_t tx_word(const uint8_t *frame, size_t length, size_t fifo_offset)
{
    uint32_t word = 0u;
    const size_t payload_length = length - ETHERNET_FRAME_HEADER_BYTES;
    for (size_t byte = 0u; byte < 4u; ++byte)
    {
        const size_t position = fifo_offset + byte;
        uint8_t value = 0u;
        if (position < ETHERNET_FIFO_LENGTH_BYTES)
        {
            value = (uint8_t)(payload_length >> (position * 8u));
        }
        else if (position - ETHERNET_FIFO_LENGTH_BYTES < length)
        {
            value = frame[position - ETHERNET_FIFO_LENGTH_BYTES];
        }
        word |= (uint32_t)value << (byte * 8u);
    }
    return word;
}

static void reset_receive_fifo(void)
{
    /* Disable first, then reset. RSTFIFO clears on read, not a busy flag (p566). */
    mac_update(MAC_RECEIVE_CONTROL, MAC_RX_ENABLE, 0u);
    mac_update(MAC_RECEIVE_CONTROL, MAC_RX_RESET, MAC_RX_RESET);
    (void)mac_read(MAC_RECEIVE_CONTROL);
    if (mac_ready)
    {
        mac_update(MAC_RECEIVE_CONTROL, MAC_RX_ENABLE, MAC_RX_ENABLE);
    }
    ++counters.rx_fifo_resets;
}

static void collect_events(void)
{
    const uint32_t events = mac_read(MAC_STATUS);
    counters.tx_completed += (events & MAC_TX_COMPLETE) != 0u;
    counters.rx_error_events += (events & MAC_RX_ERROR) != 0u;
    counters.rx_overrun_events += (events & MAC_RX_OVERRUN) != 0u;
    /* TXER acknowledgement also resets TX write pointer (p564). Never do it
     * during NEWTX. RXINT is not a queue count; use MACNP for every frame. */
    uint32_t acknowledge = events &
        (MAC_TX_COMPLETE | MAC_RX_ERROR | MAC_RX_OVERRUN);
    if ((mac_read(MAC_TRANSMIT_REQUEST) & MAC_NEW_TX) == 0u)
    {
        counters.tx_error_events += (events & MAC_TX_ERROR) != 0u;
        acknowledge |= events & MAC_TX_ERROR;
    }
    mac_write(MAC_STATUS, acknowledge);
}

ethernet_result_t ethernet_initialize(uint32_t cpu_clock_hz,
                                      const uint8_t mac_address[6])
{
    if ((cpu_clock_hz == 0u) || (cpu_clock_hz > 50000000u) ||
        (mac_address == NULL) || ((mac_address[0] & 1u) != 0u))
    {
        return ETHERNET_INVALID_ARGUMENT;
    }
    uint8_t nonzero = 0u;
    for (size_t byte = 0u; byte < 6u; ++byte)
    {
        nonzero |= mac_address[byte];
    }
    if (nonzero == 0u)
    {
        return ETHERNET_INVALID_ARGUMENT;
    }
    initialized = false;
    mac_ready = false;
    counters = (ethernet_stats_t){0};
    const uint32_t ethernet_clocks = RCGC2_EMAC0_BIT | RCGC2_EPHY0_BIT;
    system_control_enable_peripheral_clock(SYSTEM_CONTROL_RCGC2_OFFSET,
                                           ethernet_clocks | RCGC2_GPIOF_BIT);
    __asm__ volatile ("nop\n\tnop\n\tnop");
    /* SRCR2 set/clear resets only Ethernet (pp176,238). A conservative 1 ms
     * hold/recovery also spans the independent 25 MHz PHY clock. */
    REGISTER32(SYSTEM_CONTROL_BASE_ADDRESS + SYSTEM_CONTROL_SRCR2_OFFSET) |=
        ethernet_clocks;
    delay_cycles(cpu_clock_hz / 1000u + 1u);
    REGISTER32(SYSTEM_CONTROL_BASE_ADDRESS + SYSTEM_CONTROL_SRCR2_OFFSET) &=
        ~ethernet_clocks;
    delay_cycles(cpu_clock_hz / 1000u + 1u);
    mac_update(MAC_INTERRUPT_MASK, 0x7Fu, 0u);
    /* MDC = CPU / (2*(DIV+1)), p575: DIV=9 at 50 MHz, 1 at 8 MHz. */
    const uint32_t divisor = cpu_clock_hz / ETHERNET_MDC_DIVISOR_HZ +
        ((cpu_clock_hz % ETHERNET_MDC_DIVISOR_HZ != 0u) ? 1u : 0u);
    mac_update(MAC_MII_DIVIDER, 0xFFu, divisor - 1u);
    uint16_t control = PHY_RESET;
    if (!mii_transfer(0u, true, &control))
    {
        return ETHERNET_TIMEOUT;
    }
    bool reset_complete = false;
    for (uint32_t attempt = 0u; attempt < ETHERNET_PHY_RESET_ATTEMPTS; ++attempt)
    {
        if (!mii_transfer(0u, false, &control))
        {
            return ETHERNET_TIMEOUT;
        }
        if ((control & PHY_RESET) == 0u)
        {
            reset_complete = true;
            break;
        }
    }
    if (!reset_complete)
    {
        return ETHERNET_TIMEOUT;
    }
    /* PHY reset defaults enable autonegotiation, auto-MDIX and LED0 link /
     * LED1 activity (pp580,596,597). Do not restart negotiation on each poll. */
    gpio_select_alternate_function(GPIO_PORT_F_BASE_ADDRESS, GPIO_PIN(2) | GPIO_PIN(3));
    gpio_enable_digital_function(GPIO_PORT_F_BASE_ADDRESS, GPIO_PIN(2) | GPIO_PIN(3));
    mac_write(MAC_ADDRESS_0, (uint32_t)mac_address[0] |
              ((uint32_t)mac_address[1] << 8) |
              ((uint32_t)mac_address[2] << 16) |
              ((uint32_t)mac_address[3] << 24));
    mac_update(MAC_ADDRESS_1, 0xFFFFu, (uint32_t)mac_address[4] |
               ((uint32_t)mac_address[5] << 8));
    mac_update(MAC_THRESHOLD, 0x3Fu, 0x3Fu);
    mac_update(MAC_TRANSMIT_CONTROL, 0x17u, MAC_TX_PAD | MAC_TX_CRC);
    mac_update(MAC_RECEIVE_CONTROL, 0x1Fu, MAC_REJECT_BAD_CRC);
    reset_receive_fifo();
    mac_write(MAC_STATUS, 0x7Fu);
    initialized = true;
    return ETHERNET_OK;
}

ethernet_result_t ethernet_poll(ethernet_status_t *status)
{
    if (status == NULL)
    {
        return ETHERNET_INVALID_ARGUMENT;
    }
    *status = (ethernet_status_t){0};
    if (!initialized)
    {
        return ETHERNET_NOT_READY;
    }
    collect_events();
    status->tx_busy = (mac_read(MAC_TRANSMIT_REQUEST) & MAC_NEW_TX) != 0u;
    status->queued_frames = (uint8_t)(mac_read(MAC_PACKET_COUNT) & 0x3Fu);
    uint16_t phy_status = 0u;
    uint16_t diagnostic = 0u;
    /* Two status reads tolerate the standard PHY link latch behavior. */
    if (!mii_transfer(1u, false, &phy_status) ||
        !mii_transfer(1u, false, &phy_status) ||
        !mii_transfer(18u, false, &diagnostic))
    {
        mac_ready = false;
        mac_update(MAC_RECEIVE_CONTROL, MAC_RX_ENABLE, 0u);
        return ETHERNET_TIMEOUT;
    }
    status->phy_link = (phy_status & PHY_LINK) != 0u;
    status->autonegotiation_complete = (phy_status & PHY_AUTONEG_COMPLETE) != 0u;
    status->full_duplex = (diagnostic & PHY_FULL_DUPLEX) != 0u;
    status->speed_100_mbps = (diagnostic & PHY_RATE_100) != 0u;
    const bool negotiated = status->phy_link && status->autonegotiation_complete;
    const uint32_t duplex = status->full_duplex ? MAC_TX_DUPLEX : 0u;
    const bool matches = (mac_read(MAC_TRANSMIT_CONTROL) & MAC_TX_DUPLEX) == duplex;
    mac_ready = negotiated && (!status->tx_busy || matches);
    if (!status->tx_busy)
    {
        mac_update(MAC_TRANSMIT_CONTROL, MAC_TX_ENABLE | MAC_TX_DUPLEX,
                   duplex | (mac_ready ? MAC_TX_ENABLE : 0u));
    }
    mac_update(MAC_RECEIVE_CONTROL, MAC_RX_ENABLE,
               mac_ready ? MAC_RX_ENABLE : 0u);
    status->mac_ready = mac_ready;
    return ETHERNET_OK;
}

ethernet_result_t ethernet_try_transmit(const uint8_t *frame, size_t length)
{
    if ((frame == NULL) || (length < ETHERNET_FRAME_HEADER_BYTES) ||
        (length > ETHERNET_FRAME_MAX_BYTES))
    {
        return ETHERNET_INVALID_ARGUMENT;
    }
    if (!initialized || !mac_ready)
    {
        return ETHERNET_NOT_READY;
    }
    if ((mac_read(MAC_TRANSMIT_REQUEST) & MAC_NEW_TX) != 0u)
    {
        return ETHERNET_BUSY;
    }
    collect_events();
    for (size_t offset = 0u; offset < length + ETHERNET_FIFO_LENGTH_BYTES; offset += 4u)
    {
        mac_write(MAC_DATA, tx_word(frame, length, offset));
    }
    mac_update(MAC_TRANSMIT_REQUEST, MAC_NEW_TX, MAC_NEW_TX);
    ++counters.tx_submitted;
    return ETHERNET_OK;
}

ethernet_result_t ethernet_read_frame(uint8_t *frame, size_t capacity,
                                      size_t *length)
{
    if (length == NULL)
    {
        return ETHERNET_INVALID_ARGUMENT;
    }
    *length = 0u;
    if ((frame == NULL) && (capacity != 0u))
    {
        return ETHERNET_INVALID_ARGUMENT;
    }
    if (!initialized)
    {
        return ETHERNET_NOT_READY;
    }
    if ((mac_read(MAC_PACKET_COUNT) & 0x3Fu) == 0u)
    {
        return ETHERNET_EMPTY;
    }
    uint32_t word = mac_read(MAC_DATA);
    /* FIFO length includes the two-byte prefix and FCS. Drain by length,
     * not NPR: NPR may fall to zero before FCS has been read (p578). */
    const size_t fifo_length = word & 0xFFFFu;
    if ((fifo_length < ETHERNET_FIFO_LENGTH_BYTES + ETHERNET_FCS_BYTES) ||
        (fifo_length > ETHERNET_FIFO_BYTES))
    {
        reset_receive_fifo();
        ++counters.rx_dropped;
        return ETHERNET_DROPPED;
    }
    const size_t frame_length = fifo_length -
        ETHERNET_FIFO_LENGTH_BYTES - ETHERNET_FCS_BYTES;
    const bool accept = (frame != NULL) &&
        (frame_length >= ETHERNET_MIN_RECEIVED_BYTES) &&
        (frame_length <= ETHERNET_FRAME_MAX_BYTES) && (frame_length <= capacity);
    for (size_t offset = 0u; offset < fifo_length; offset += 4u)
    {
        if (offset != 0u)
        {
            word = mac_read(MAC_DATA);
        }
        for (size_t byte = 0u; byte < 4u; ++byte)
        {
            const size_t position = offset + byte;
            if (accept && (position >= ETHERNET_FIFO_LENGTH_BYTES) &&
                (position - ETHERNET_FIFO_LENGTH_BYTES < frame_length))
            {
                frame[position - ETHERNET_FIFO_LENGTH_BYTES] =
                    (uint8_t)(word >> (byte * 8u));
            }
        }
    }
    /* W1C only RXINT; remaining queued frames are discovered through MACNP. */
    mac_write(MAC_STATUS, MAC_RX_EVENT);
    if (!accept)
    {
        ++counters.rx_dropped;
        return ETHERNET_DROPPED;
    }
    *length = frame_length;
    ++counters.rx_frames;
    return ETHERNET_OK;
}

void ethernet_get_stats(ethernet_stats_t *stats)
{
    if (stats != NULL)
    {
        *stats = counters;
    }
}
