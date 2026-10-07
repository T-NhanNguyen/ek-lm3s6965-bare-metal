/* Deterministic hardware shim, real pinned lwIP. Include platform sources to
 * inspect the private ISR counter and force RX chains without production hooks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/stats.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "ethernet.h"

volatile uint32_t test_systick[3];
#include "../../../../examples/ethernet-ftp/net_clock.c"

static bool allocation_fail;
static bool allocate_chain;
static struct pbuf *test_pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type)
{
    if (allocation_fail) return NULL;
    if (!allocate_chain) return pbuf_alloc(layer, length, type);
    struct pbuf *first = pbuf_alloc(layer, 10, PBUF_RAM);
    struct pbuf *second = pbuf_alloc(PBUF_RAW, length - 10, PBUF_RAM);
    assert(first && second);
    pbuf_cat(first, second);
    return first;
}
#define pbuf_alloc test_pbuf_alloc
#include "../../../../examples/ethernet-ftp/lm3s_netif.c"
#undef pbuf_alloc

static ethernet_result_t poll_result = ETHERNET_OK;
static ethernet_status_t fake_status;
static unsigned polls, reads, pending, discards, teardowns, transmissions;
static ethernet_result_t tx_result = ETHERNET_OK;
static uint8_t frame[1514];
static size_t frame_length = 60;
static uint8_t sent[1514];
static size_t sent_length;
static err_t input_result = ERR_OK;
static unsigned inputs;

ethernet_result_t ethernet_initialize(uint32_t hz, const uint8_t address[6])
{
    const uint8_t expected[6] = {2, 0, 0, 0x69, 0x65, 2};
    assert(hz == 50000000);
    assert(memcmp(address, expected, 6) == 0);
    return ETHERNET_OK;
}
ethernet_result_t ethernet_poll(ethernet_status_t *status)
{
    ++polls;
    *status = fake_status;
    return poll_result;
}
ethernet_result_t ethernet_try_transmit(const uint8_t *data, size_t length)
{
    ++transmissions;
    memcpy(sent, data, length);
    sent_length = length;
    return tx_result;
}
ethernet_result_t ethernet_read_frame(uint8_t *data, size_t capacity, size_t *length)
{
    ++reads;
    *length = 0;
    if (!pending) return ETHERNET_EMPTY;
    --pending;
    if (!data) { assert(capacity == 0); ++discards; return ETHERNET_DROPPED; }
    assert(capacity == 1514);
    memcpy(data, frame, frame_length);
    *length = frame_length;
    return ETHERNET_OK;
}
void ftp_tcp_link_down(void)
{
    assert(!lm3s_netif_ready());
    assert(!netif_is_up(netif_default));
    assert(!netif_is_link_up(netif_default));
    ++teardowns;
}
static err_t consume_input(struct pbuf *packet, struct netif *netif)
{
    (void)netif;
    uint8_t copy[1514];
    ++inputs;
    assert(pbuf_copy_partial(packet, copy, packet->tot_len, 0) == frame_length);
    assert(memcmp(copy, frame, frame_length) == 0);
    if (allocate_chain) assert(packet->next != NULL);
    if (input_result == ERR_OK) pbuf_free(packet);
    return input_result;
}
static void clock_tests(void)
{
    assert(net_clock_initialize(50000000));
    assert(test_systick[0] == 7 && test_systick[1] == 49999 && test_systick[2] == 0);
    for (unsigned i = 0; i < 1234; ++i) SysTick_Handler();
    assert(sys_now() == 1234); /* Foreground did not sample during ISR ticks. */
    milliseconds = UINT32_MAX - 1;
    SysTick_Handler(); SysTick_Handler(); SysTick_Handler();
    assert(sys_now() == 1);
    assert(!net_clock_elapsed(3, UINT32_MAX - 5, 10));
    assert(net_clock_elapsed(4, UINT32_MAX - 5, 10));
    assert(!net_clock_elapsed(0, 0, UINT32_C(0x80000000)));
    assert(net_clock_elapsed(0x7fffffff, 0, 0x7fffffff));
    assert(net_clock_initialize(8000000));
    assert(test_systick[1] == 7999 && sys_now() == 0);
    const uint32_t invalid[] = {0, 999, 8000001, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
    {
        assert(!net_clock_initialize(invalid[i]));
        assert(test_systick[0] == 7 && test_systick[1] == 7999);
    }
}
int main(void)
{
    clock_tests();
    lwip_init();
    for (unsigned i = 0; i < sizeof(frame); ++i) frame[i] = (uint8_t)i;
    assert(lm3s_netif_initialize(50000000, UINT32_MAX - 5) == ERR_OK);
    assert(lm3s_netif_initialize(50000000, 0) == ERR_ALREADY);
    assert(!netif_is_up(netif_default) && !netif_is_link_up(netif_default));
    assert(ip4_addr_get_u32(lm3s_netif_address()) == PP_HTONL(0xc0a80702));
    assert(ip4_addr_get_u32(netif_ip4_netmask(netif_default)) == PP_HTONL(0xffffff00));
    assert(ip4_addr_isany_val(*netif_ip4_gw(netif_default)));
    fake_status.phy_link = true;
    fake_status.autonegotiation_complete = true;
    pending = 10;
    lm3s_netif_service(UINT32_MAX - 5);
    assert(polls == 1 && !ready && discards == 8 && pending == 2);
    lm3s_netif_service(3);
    assert(polls == 1 && discards == 10);
    fake_status.mac_ready = true;
    lm3s_netif_service(4);
    assert(polls == 2 && ready && netif_is_up(netif_default) && netif_is_link_up(netif_default));
    netif_default->input = consume_input;
    inputs = 0; reads = 0; pending = 10;
    lm3s_netif_service(5);
    assert(inputs == 8 && reads == 8 && pending == 2);
    lm3s_netif_service(6);
    assert(inputs == 10 && pending == 0);
    allocate_chain = true; pending = 1;
    lm3s_netif_service(7);
    assert(inputs == 11);
    input_result = ERR_IF; pending = 1;
    const mem_size_t used = lwip_stats.mem.used;
    lm3s_netif_service(8);
    assert(inputs == 12 && lwip_stats.mem.used == used);
    allocation_fail = true; pending = 1;
    lm3s_netif_service(9);
    assert(inputs == 12 && pending == 0);
    allocation_fail = false; allocate_chain = false;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, 700, PBUF_RAM);
    struct pbuf *q = pbuf_alloc(PBUF_RAW, 814, PBUF_RAM);
    assert(p && q); pbuf_cat(p, q);
    assert(pbuf_take(p, frame, 1514) == ERR_OK);
    tx_result = ETHERNET_BUSY;
    assert(netif_default->linkoutput(netif_default, p) == ERR_MEM);
    assert(p->ref == 1 && q->ref == 1 && p->next == q);
    assert(sent_length == 1514 && memcmp(sent, frame, 1514) == 0);
    tx_result = ETHERNET_TIMEOUT;
    assert(netif_default->linkoutput(netif_default, p) == ERR_IF);
    tx_result = ETHERNET_OK;
    assert(netif_default->linkoutput(netif_default, p) == ERR_OK);
    pbuf_free(p);
    p = pbuf_alloc(PBUF_RAW, 1515, PBUF_RAM);
    const unsigned before = transmissions;
    assert(netif_default->linkoutput(netif_default, p) == ERR_BUF);
    assert(transmissions == before); pbuf_free(p);
    p = pbuf_alloc(PBUF_RAW, 13, PBUF_RAM);
    assert(netif_default->linkoutput(netif_default, p) == ERR_BUF); pbuf_free(p);
    assert(netif_default->linkoutput(netif_default, NULL) == ERR_BUF);
    /* Install a real stable ARP entry so the cleanup assertion is nonvacuous. */
    const uint8_t arp_reply[28] = {
        0, 1, 8, 0, 6, 4, 0, 2,
        2, 0, 0, 0, 0, 1, 192, 168, 7, 1,
        2, 0, 0, 0x69, 0x65, 2, 192, 168, 7, 2
    };
    p = pbuf_alloc(PBUF_RAW, sizeof(arp_reply), PBUF_RAM);
    assert(p && pbuf_take(p, arp_reply, sizeof(arp_reply)) == ERR_OK);
    etharp_input(p, netif_default);
    ip4_addr_t peer; IP4_ADDR(&peer, 192, 168, 7, 1);
    struct eth_addr *peer_mac; const ip4_addr_t *found_ip;
    assert(etharp_find_addr(netif_default, &peer, &peer_mac, &found_ip) >= 0);
    fake_status.mac_ready = false; pending = 1;
    lm3s_netif_service(14);
    assert(!ready && teardowns == 1 && discards == 11);
    for (size_t i = 0; i < ARP_TABLE_SIZE; ++i)
    {
        ip4_addr_t *ip; struct netif *n; struct eth_addr *eth;
        assert(!etharp_get_entry(i, &ip, &n, &eth));
    }
    lm3s_netif_service(24); assert(teardowns == 1);
    fake_status.mac_ready = true; lm3s_netif_service(34); assert(ready);
    poll_result = ETHERNET_TIMEOUT; lm3s_netif_service(44);
    assert(!ready && teardowns == 2);
    assert(lwip_stats.mem.used == 0);
    assert(lwip_stats.memp[MEMP_PBUF_POOL]->used == 0);
    puts("FTP platform: clock/wrap, PHY cadence/readiness, bounded RX, chain ownership/TX errors, ARP teardown passed");
    return 0;
}
