/* Host-only register/FIFO model; run with the command below, no target access.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -Iinclude/lm3s6965 tests/ethernet_test.c -o /tmp/ethernet_test
 */
#include <assert.h>
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
static size_t first_packet_end;
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

static volatile uint32_t *host_register(uint32_t address)
{
    if (address == SYSTEM_CONTROL_BASE_ADDRESS + SYSTEM_CONTROL_SRCR2_OFFSET)
    {
        return &system_reset;
    }
    assert(address >= ETHERNET_BASE_ADDRESS && address <= ETHERNET_BASE_ADDRESS + 0x38u);
    const uint32_t offset = address - ETHERNET_BASE_ADDRESS;
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
        return &fifo[fifo_index++];
    }
    if (offset == 0x34u && receive_mode)
    {
        registers[offset / 4u] = fifo_index < first_packet_end ? 2u :
            fifo_index < fifo_words ? 1u : 0u;
    }
    return &registers[offset / 4u];
}

#undef REGISTER32
#define REGISTER32(address) (*host_register(address))
#include "../src/ethernet.c"

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
    memset(registers, 0, sizeof(registers));
    management_count = 0u;
    mii_stuck = false;
    phy_reset_stuck = false;
    receive_mode = false;
    fifo_index = 0u;
    fifo_words = 0u;
    first_packet_end = 0u;
    system_reset = 0u;
}

static void append_packet(const uint8_t *frame, size_t length)
{
    const size_t total = length + 6u;
    for (size_t position = 0u; position < total; position += 4u)
    {
        uint32_t word = 0u;
        for (size_t byte = 0u; byte < 4u; ++byte)
        {
            const size_t index = position + byte;
            const uint8_t value = index < 2u ? (uint8_t)(total >> (8u * index)) :
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
            assert(ethernet_read_frame(output + alignment, 1514u, &length) == ETHERNET_OK);
            assert(length == bytes && memcmp(output + alignment, frame, bytes) == 0);
            assert(fifo_index == fifo_words);
            assert(ethernet_read_frame(output, sizeof(output), &length) == ETHERNET_EMPTY);
            assert(length == 0u);
        }
    }
    /* A dropped first packet must not corrupt the second, including FCS tails. */
    for (size_t bytes = 60u; bytes < 64u; ++bytes)
    {
        clear_model();
        append_packet(frame, bytes);
        first_packet_end = fifo_words;
        append_packet(frame + 1, bytes + 1u);
        receive_mode = true;
        size_t length;
        memset(output, 0xA5, sizeof(output));
        assert(ethernet_read_frame(output, bytes - 1u, &length) == ETHERNET_DROPPED);
        assert(length == 0u && output[0] == 0xA5u);
        assert(ethernet_read_frame(output, sizeof(output), &length) == ETHERNET_OK);
        assert(length == bytes + 1u && memcmp(output, frame + 1, length) == 0);
        assert(fifo_index == fifo_words);
    }
    const size_t invalid_sizes[] = {0u, 13u, 59u, 1515u};
    for (size_t test = 0u; test < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++test)
    {
        clear_model();
        append_packet(frame, invalid_sizes[test]);
        receive_mode = true;
        size_t length;
        assert(ethernet_read_frame(output, sizeof(output), &length) == ETHERNET_DROPPED);
        assert(fifo_index == fifo_words && length == 0u);
    }
    const uint32_t corrupt_lengths[] = {0u, 5u, 2049u, 65535u};
    for (size_t test = 0u; test < 4u; ++test)
    {
        clear_model();
        fifo[0] = corrupt_lengths[test];
        fifo_words = 1u;
        receive_mode = true;
        size_t length;
        const uint32_t resets = counters.rx_fifo_resets;
        assert(ethernet_read_frame(output, sizeof(output), &length) == ETHERNET_DROPPED);
        assert(counters.rx_fifo_resets == resets + 1u && length == 0u);
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
        assert(clock_mask == (RCGC2_GPIOF_BIT | RCGC2_EMAC0_BIT | RCGC2_EPHY0_BIT));
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
        assert(status.phy_link && status.mac_ready && status.full_duplex && status.speed_100_mbps);
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
    assert(ethernet_initialize(50000000u, mac) == ETHERNET_TIMEOUT && !initialized);
    assert(management_count == ETHERNET_PHY_RESET_ATTEMPTS + 1u);
    clear_model();
    mii_stuck = true;
    assert(ethernet_initialize(50000000u, mac) == ETHERNET_TIMEOUT && !initialized);
    assert(ethernet_try_transmit(mac, 14u) == ETHERNET_NOT_READY);
    assert(ethernet_initialize(0u, mac) == ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_initialize(50000001u, mac) == ETHERNET_INVALID_ARGUMENT);
    assert(ethernet_initialize(8000000u, NULL) == ETHERNET_INVALID_ARGUMENT);
}

int main(void)
{
    test_packing();
    test_receive();
    test_management();
    puts("Ethernet host tests passed (packing, RX drain, MII, link/duplex, timeouts)");
    return 0;
}
