/* Host-only register/FIFO model; run with the command below, no target access.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -Iinclude/lm3s6965 scripts/ethernet/platform/tests/ethernet_test.c -o /tmp/ethernet_test
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memory_map.h"
#include "gpio.h"
#include "system_control.h"

static uint32_t registers[16];
static uint32_t system_reset;
static uint32_t fifo[512];
static size_t fifo_words;
static size_t fifo_index;
static size_t packet_data_end[2];
static size_t packet_count;
static size_t early_fcs_reads;
static uint32_t tx_fifo[512];
static size_t tx_words;
static size_t tx_writes;
static size_t tx_starts;
static size_t tx_reset_count;
static size_t rx_reset_count;
static size_t rx_reset_reads;
static uint32_t pending_value;
static uint32_t pending_offset;
static bool pending_write;

/* REGISTER32 is an lvalue: commit its store at the next modeled access.
 * This is host instrumentation, not a model of hardware write latency. */
static void settle_write(void)
{
    if (!pending_write)
    {
        return;
    }
    pending_write = false;
    if (pending_offset == 0u)
    {
        registers[0] &= ~pending_value;
        if ((pending_value & 2u) != 0u)
        {
            tx_words = 0u;
            ++tx_reset_count;
        }
    }
    else if (pending_offset == 0x10u)
    {
        assert((registers[0x38u / 4u] & 1u) == 0u);
        assert(tx_words < 512u);
        tx_fifo[tx_words++] = pending_value;
        ++tx_writes;
    }
    else
    {
        if (pending_offset == 0x08u && (pending_value & 16u) != 0u)
        {
            assert((pending_value & 1u) == 0u);
            fifo_index = 0u;
            fifo_words = 0u;
            packet_count = 0u;
            registers[0x34u / 4u] = 0u;
            registers[0] &= ~1u;
            ++rx_reset_count;
        }
        if (pending_offset == 0x38u && (pending_value & 1u) != 0u)
        {
            assert(tx_words != 0u);
            ++tx_starts;
        }
        registers[pending_offset / 4u] = pending_value;
    }
}

static uint32_t queued_packets(void)
{
    uint32_t count = 0u;
    for (size_t packet = 0u; packet < packet_count; ++packet)
    {
        count += fifo_index < packet_data_end[packet];
    }
    return count;
}

static uint32_t management_commands[32];
static size_t management_count;
static uint32_t management_data;
static uint16_t phy_status;
static uint16_t phy_diagnostic;
static bool mii_stuck;
static bool phy_reset_stuck;
static bool receive_mode;
static uint32_t clock_mask;
static uint32_t alternate_mask;
static uint32_t digital_mask;

static volatile uint32_t *host_register(uint32_t address,
                                       const char *caller)
{
    settle_write();
    if (address == SYSTEM_CONTROL_BASE_ADDRESS + SYSTEM_CONTROL_SRCR2_OFFSET)
    {
        return &system_reset;
    }
    assert(address >= ETHERNET_BASE_ADDRESS &&
           address <= ETHERNET_BASE_ADDRESS + 0x38u);
    const uint32_t offset = address - ETHERNET_BASE_ADDRESS;
    if (strcmp(caller, "mac_write") == 0)
    {
        pending_offset = offset;
        pending_write = true;
        return &pending_value;
    }
    uint32_t *control = &registers[0x20u / 4u];
    if ((*control & 1u) != 0u && !mii_stuck)
    {
        if (management_count < 32u)
        {
            management_commands[management_count] = *control;
        }
        ++management_count;
        management_data = registers[0x2Cu / 4u];
        const uint32_t phy_register = (*control >> 3) & 31u;
        registers[0x30u / 4u] = phy_register == 1u ? phy_status :
            phy_register == 18u ? phy_diagnostic :
            (phy_reset_stuck ? 0x8000u : 0x3100u);
        *control &= ~1u;
    }
    if (offset == 0x10u && receive_mode)
    {
        assert(fifo_index < fifo_words);
        early_fcs_reads += queued_packets() == 0u;
        uint32_t *word = &fifo[fifo_index++];
        registers[0x34u / 4u] = queued_packets();
        if (queued_packets() == 0u)
        {
            registers[0] &= ~1u;
        }
        return word;
    }
    if (offset == 0x34u && receive_mode)
    {
        registers[offset / 4u] = queued_packets();
    }
    if (offset == 0x08u && (registers[offset / 4u] & 16u) != 0u)
    {
        registers[offset / 4u] &= ~16u;
        ++rx_reset_reads;
    }
    return &registers[offset / 4u];
}

#undef REGISTER32
#define REGISTER32(address) (*host_register(address, __func__))
#include "../../../../src/ethernet.c"

void system_control_enable_peripheral_clock(uint32_t offset, uint32_t mask)
{
    assert(offset == SYSTEM_CONTROL_RCGC2_OFFSET);
    clock_mask = mask;
}

void gpio_select_alternate_function(uint32_t base, uint32_t mask)
{
    assert(base == GPIO_PORT_F_BASE_ADDRESS);
    alternate_mask = mask;
}

void gpio_enable_digital_function(uint32_t base, uint32_t mask)
{
    assert(base == GPIO_PORT_F_BASE_ADDRESS);
    digital_mask = mask;
}

static void clear_model(void)
{
    pending_write = false;
    memset(registers, 0, sizeof(registers));
    packet_count = 0u;
    early_fcs_reads = 0u;
    tx_words = 0u;
    tx_writes = 0u;
    tx_starts = 0u;
    tx_reset_count = 0u;
    rx_reset_count = 0u;
    rx_reset_reads = 0u;
    management_count = 0u;
    mii_stuck = false;
    phy_reset_stuck = false;
    receive_mode = false;
    fifo_index = 0u;
    fifo_words = 0u;
    system_reset = 0u;
}

static void append_packet(const uint8_t *frame, size_t length)
{
    /* Accepted DriverLib count convention, not independent hardware proof.
     * Choose an allowed p578 case: NPR falls after the last data word.
     * The exact hardware decrement timing is not established here. */
    assert(packet_count < 2u);
    packet_data_end[packet_count++] = fifo_words + (length + 5u) / 4u;
    const size_t total = length + 6u;
    for (size_t position = 0u; position < total; position += 4u)
    {
        uint32_t word = 0u;
        for (size_t byte = 0u; byte < 4u; ++byte)
        {
            const size_t index = position + byte;
            const uint8_t value = index < 2u ?
                (uint8_t)(total >> (8u * index)) :
                index - 2u < length ? frame[index - 2u] : 0xFCu;
            word |= (uint32_t)value << (byte * 8u);
        }
        assert(fifo_words < 512u);
        fifo[fifo_words++] = word;
    }
}

static void test_packing(void)
{
    uint8_t storage[ETHERNET_FRAME_MAX_BYTES + 4u];
    for (size_t alignment = 0u; alignment < 4u; ++alignment)
    {
        uint8_t *frame = storage + alignment;
        for (size_t byte = 0u; byte < ETHERNET_FRAME_MAX_BYTES; ++byte)
        {
            frame[byte] = (uint8_t)byte;
        }
        for (size_t length = 14u; length <= ETHERNET_FRAME_MAX_BYTES; ++length)
        {
            for (size_t offset = 0u; offset < length + 2u; offset += 4u)
            {
                const uint32_t word = tx_word(frame, length, offset);
                for (size_t byte = 0u; byte < 4u; ++byte)
                {
                    const size_t position = offset + byte;
                    const uint8_t expected = position < 2u ?
                        (uint8_t)((length - 14u) >> (8u * position)) :
                        position - 2u < length ? frame[position - 2u] : 0u;
                    assert((uint8_t)(word >> (byte * 8u)) == expected);
                }
            }
        }
    }
}

static void test_receive(void)
{
    uint8_t frame[1515];
    uint8_t output[1518];
    for (size_t byte = 0u; byte < sizeof(frame); ++byte)
    {
        frame[byte] = (uint8_t)(byte * 7u);
    }
    initialized = true;
    mac_ready = true;
    for (size_t alignment = 0u; alignment < 4u; ++alignment)
    {
        for (size_t bytes = 60u; bytes <= 1514u; ++bytes)
        {
            clear_model();
            append_packet(frame, bytes);
            receive_mode = true;
            size_t length = 99u;
            assert(ethernet_read_frame(output + alignment, 1514u, &length) ==
                   ETHERNET_OK);
            assert(length == bytes &&
                   memcmp(output + alignment, frame, bytes) == 0);
            assert(fifo_index == fifo_words);
            assert(ethernet_read_frame(output, sizeof(output), &length) ==
                   ETHERNET_EMPTY);
            assert(length == 0u);
        }
    }
    /* A dropped packet must not corrupt the next, including FCS tails. */
    for (size_t bytes = 60u; bytes < 64u; ++bytes)
    {
        clear_model();
        append_packet(frame, bytes);
        append_packet(frame + 1, bytes + 1u);
        receive_mode = true;
        size_t length;
        memset(output, 0xA5, sizeof(output));
        assert(ethernet_read_frame(output, bytes - 1u, &length) ==
               ETHERNET_DROPPED);
        assert(length == 0u && output[0] == 0xA5u);
        assert(ethernet_read_frame(output, sizeof(output), &length) ==
               ETHERNET_OK);
        assert(length == bytes + 1u && memcmp(output, frame + 1, length) == 0);
        assert(fifo_index == fifo_words);
    }
    const size_t invalid_sizes[] = {0u, 13u, 59u, 1515u};
    for (size_t test = 0u;
         test < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++test)
    {
        clear_model();
        append_packet(frame, invalid_sizes[test]);
        receive_mode = true;
        size_t length;
        assert(ethernet_read_frame(output, sizeof(output), &length) ==
               ETHERNET_DROPPED);
        assert(fifo_index == fifo_words && length == 0u);
    }
    const uint32_t corrupt_lengths[] = {0u, 5u, 2049u, 65535u};
    for (size_t test = 0u; test < 4u; ++test)
    {
        clear_model();
        fifo[0] = corrupt_lengths[test];
        fifo_words = 1u;
        packet_count = 1u;
        packet_data_end[0] = 1u;
        receive_mode = true;
        size_t length;
        const uint32_t resets = counters.rx_fifo_resets;
        assert(ethernet_read_frame(output, sizeof(output), &length) ==
               ETHERNET_DROPPED);
        settle_write();
        assert(counters.rx_fifo_resets == resets + 1u && length == 0u);
        assert(rx_reset_count == 1u && rx_reset_reads == 1u);
        assert(fifo_words == 0u && fifo_index == 0u);
        assert((registers[MAC_RECEIVE_CONTROL / 4u] & MAC_RX_RESET) == 0u);
        assert((registers[MAC_RECEIVE_CONTROL / 4u] & MAC_RX_ENABLE) != 0u);
    }
}

static void test_arguments(void)
{
    clear_model();
    initialized = true;
    mac_ready = true;
    uint8_t output[64] = {0};
    size_t length = 99u;
    assert(ethernet_read_frame(NULL, sizeof(output), &length) ==
           ETHERNET_INVALID_ARGUMENT);
    assert(length == 0u && fifo_index == 0u && !pending_write);
    assert(ethernet_read_frame(output, sizeof(output), NULL) ==
           ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_read_frame(NULL, 0u, NULL) ==
           ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_poll(NULL) == ETHERNET_INVALID_ARGUMENT);
    ethernet_get_stats(NULL);
    assert(ethernet_try_transmit(NULL, 14u) == ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_try_transmit(output, 13u) == ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_try_transmit(output, 1515u) == ETHERNET_INVALID_ARGUMENT);
    append_packet(output, sizeof(output));
    receive_mode = true;
    assert(ethernet_read_frame(NULL, 0u, &length) == ETHERNET_DROPPED);
    assert(length == 0u && fifo_index == fifo_words);
}

static void test_transmit(void)
{
    uint8_t storage[ETHERNET_FRAME_MAX_BYTES + 3u];
    initialized = true;
    mac_ready = true;
    for (size_t alignment = 0u; alignment < 4u; ++alignment)
    {
        uint8_t *frame = storage + alignment;
        for (size_t byte = 0u; byte < ETHERNET_FRAME_MAX_BYTES; ++byte)
        {
            frame[byte] = (uint8_t)(byte * 7u);
        }
        for (size_t length = 14u; length <= ETHERNET_FRAME_MAX_BYTES;
             ++length)
        {
            clear_model();
            tx_words = 3u; /* Simulate an abandoned FIFO write. */
            registers[MAC_STATUS / 4u] = MAC_TX_ERROR | MAC_TX_COMPLETE |
                MAC_RX_ERROR | MAC_RX_OVERRUN | MAC_RX_EVENT;
            const ethernet_stats_t before = counters;
            assert(ethernet_try_transmit(frame, length) == ETHERNET_OK);
            settle_write();
            assert(tx_reset_count == 1u && tx_starts == 1u);
            assert(tx_words == (length + 5u) / 4u);
            assert(tx_writes == tx_words);
            assert(registers[MAC_TRANSMIT_REQUEST / 4u] == MAC_NEW_TX);
            assert(registers[MAC_STATUS / 4u] == MAC_RX_EVENT);
            assert(counters.tx_submitted == before.tx_submitted + 1u);
            assert(counters.tx_error_events == before.tx_error_events + 1u);
            assert(counters.tx_completed == before.tx_completed + 1u);
            assert(counters.rx_error_events == before.rx_error_events + 1u);
            assert(counters.rx_overrun_events ==
                   before.rx_overrun_events + 1u);
            for (size_t position = 0u; position < tx_words * 4u; ++position)
            {
                const uint8_t expected = position < 2u ?
                    (uint8_t)((length - 14u) >> (8u * position)) :
                    position - 2u < length ? frame[position - 2u] : 0u;
                assert((uint8_t)(tx_fifo[position / 4u] >>
                                (8u * (position % 4u))) == expected);
            }
            registers[MAC_STATUS / 4u] |= MAC_TX_ERROR;
            assert(ethernet_try_transmit(frame, length) == ETHERNET_BUSY);
            assert(!pending_write && tx_writes == tx_words);
            assert(tx_starts == 1u && tx_reset_count == 1u);
            assert(counters.tx_submitted == before.tx_submitted + 1u);
        }
    }
    phy_status = PHY_LINK | PHY_AUTONEG_COMPLETE;
    phy_diagnostic = 0u;
    ethernet_status_t status;
    const uint32_t errors = counters.tx_error_events;
    const size_t writes = tx_writes;
    assert(ethernet_poll(&status) == ETHERNET_OK && status.tx_busy);
    settle_write();
    assert((registers[MAC_STATUS / 4u] & MAC_TX_ERROR) != 0u);
    assert(counters.tx_error_events == errors && tx_reset_count == 1u);
    assert(tx_writes == writes && tx_words == writes);
    /* Completion is injected explicitly; no transmission timing is modeled. */
    registers[MAC_TRANSMIT_REQUEST / 4u] = 0u;
    assert(ethernet_poll(&status) == ETHERNET_OK && !status.tx_busy);
    settle_write();
    assert((registers[MAC_STATUS / 4u] & MAC_TX_ERROR) == 0u);
    assert(counters.tx_error_events == errors + 1u);
    assert(tx_reset_count == 2u && tx_words == 0u);
    assert(ethernet_poll(&status) == ETHERNET_OK);
    settle_write();
    assert(counters.tx_error_events == errors + 1u);
    assert(tx_reset_count == 2u);
}

static void test_early_npr(void)
{
    uint8_t frame[59] = {0};
    uint8_t output[64];
    for (size_t bytes = 56u; bytes < 60u; ++bytes)
    {
        clear_model();
        append_packet(frame, bytes);
        receive_mode = true;
        registers[MAC_STATUS / 4u] = MAC_RX_EVENT | MAC_TX_ERROR;
        memset(output, 0xA5, sizeof(output));
        size_t length = 99u;
        assert(ethernet_read_frame(output, sizeof(output), &length) ==
               ETHERNET_DROPPED);
        settle_write();
        assert(length == 0u && output[0] == 0xA5u);
        assert(fifo_index == fifo_words && early_fcs_reads == 1u);
        assert(registers[MAC_PACKET_COUNT / 4u] == 0u);
        assert(registers[MAC_STATUS / 4u] == MAC_TX_ERROR);
        assert(tx_reset_count == 0u);
        assert(ethernet_read_frame(output, sizeof(output), &length) ==
               ETHERNET_EMPTY);
    }
}

static void test_management(void)
{
    const uint8_t mac[] = {2u, 0x12u, 0x34u, 0x56u, 0x78u, 0x9Au};
    const uint32_t clocks[] = {8000000u, 50000000u};
    for (size_t clock = 0u; clock < 2u; ++clock)
    {
        clear_model();
        assert(ethernet_initialize(clocks[clock], mac) == ETHERNET_OK);
        assert(clock_mask ==
               (RCGC2_GPIOF_BIT | RCGC2_EMAC0_BIT | RCGC2_EPHY0_BIT));
        assert(alternate_mask == 12u && digital_mask == 12u);
        assert(registers[MAC_MII_DIVIDER / 4u] == (clock == 0u ? 1u : 9u));
        assert(registers[MAC_ADDRESS_0 / 4u] == 0x56341202u);
        assert(registers[MAC_ADDRESS_1 / 4u] == 0x9A78u);
        assert(management_commands[0] == 3u && management_data == PHY_RESET);
        assert(!mac_ready);
        phy_status = PHY_LINK | PHY_AUTONEG_COMPLETE;
        phy_diagnostic = PHY_FULL_DUPLEX | PHY_RATE_100;
        ethernet_status_t status;
        assert(ethernet_poll(&status) == ETHERNET_OK);
        assert(status.phy_link && status.mac_ready && status.full_duplex &&
               status.speed_100_mbps);
        assert(management_commands[2] == 9u && management_commands[4] == 145u);
        assert((registers[MAC_TRANSMIT_CONTROL / 4u] & MAC_TX_DUPLEX) != 0u);
        phy_diagnostic = 0u;
        registers[MAC_TRANSMIT_REQUEST / 4u] = MAC_NEW_TX;
        assert(ethernet_poll(&status) == ETHERNET_OK && !status.mac_ready);
        assert((registers[MAC_TRANSMIT_CONTROL / 4u] & MAC_TX_DUPLEX) != 0u);
        registers[MAC_TRANSMIT_REQUEST / 4u] = 0u;
        assert(ethernet_poll(&status) == ETHERNET_OK && status.mac_ready);
        assert((registers[MAC_TRANSMIT_CONTROL / 4u] & MAC_TX_DUPLEX) == 0u);
        phy_status = 0u;
        assert(ethernet_poll(&status) == ETHERNET_OK && !status.mac_ready);
        mii_stuck = true;
        assert(ethernet_poll(&status) == ETHERNET_TIMEOUT && !status.mac_ready);
        assert(counters.mii_timeouts == 1u);
    }
    clear_model();
    phy_reset_stuck = true;
    assert(ethernet_initialize(50000000u, mac) == ETHERNET_TIMEOUT &&
           !initialized);
    assert(management_count == ETHERNET_PHY_RESET_ATTEMPTS + 1u);
    clear_model();
    mii_stuck = true;
    assert(ethernet_initialize(50000000u, mac) == ETHERNET_TIMEOUT &&
           !initialized);
    assert(ethernet_try_transmit(mac, 14u) == ETHERNET_NOT_READY);
    assert(ethernet_initialize(0u, mac) == ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_initialize(50000001u, mac) == ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_initialize(8000000u, NULL) == ETHERNET_INVALID_ARGUMENT);
}

int main(void)
{
    test_packing();
    test_receive();
    test_arguments();
    test_transmit();
    test_early_npr();
    test_management();
    puts("Ethernet host tests passed "
         "(TX, RX drain/reset, W1C, arguments, MII)");
    return 0;
}
