#include "lm3s_netif.h"
#include "ethernet.h"
#include "ftp_tcp.h"
#include "net_clock.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"
#include <string.h>

static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x69, 0x65, 0x02};
static struct netif interface;
static uint32_t last_phy_poll;
static bool initialized;
static bool ready;
/* Separate bounded scratch buffers: ethernet_input can generate TX during RX.
 * Driver copies synchronously; neither buffer is retained by lwIP/hardware. */
static uint8_t rx_frame[ETHERNET_FRAME_MAX_BYTES];
static uint8_t tx_frame[ETHERNET_FRAME_MAX_BYTES];

static err_t link_output(struct netif *netif, struct pbuf *packet)
{
    (void)netif;
    if (!ready)
    {
        return ERR_IF;
    }
    if (packet == NULL || packet->tot_len < ETHERNET_FRAME_HEADER_BYTES ||
        packet->tot_len > sizeof(tx_frame))
    {
        return ERR_BUF;
    }
    const u16_t length = packet->tot_len;
    if (pbuf_copy_partial(packet, tx_frame, length, 0) != length)
    {
        return ERR_BUF;
    }
    /* Caller keeps pbuf ownership on every outcome, including BUSY. */
    switch (ethernet_try_transmit(tx_frame, length))
    {
        case ETHERNET_OK: return ERR_OK;
        case ETHERNET_BUSY: return ERR_MEM;
        default: return ERR_IF;
    }
}

static err_t interface_init(struct netif *netif)
{
    netif->name[0] = 'l';
    netif->name[1] = 'm';
    netif->hwaddr_len = sizeof(mac);
    memcpy(netif->hwaddr, mac, sizeof(mac));
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    return ERR_OK;
}

err_t lm3s_netif_initialize(uint32_t cpu_clock_hz, uint32_t now)
{
    ip4_addr_t address, mask, gateway;
    if (initialized)
    {
        return ERR_ALREADY;
    }
    if (ethernet_initialize(cpu_clock_hz, mac) != ETHERNET_OK)
    {
        return ERR_IF;
    }
    IP4_ADDR(&address, 192, 168, 7, 2);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    ip4_addr_set_zero(&gateway);
    if (netif_add(&interface, &address, &mask, &gateway, NULL,
                  interface_init, ethernet_input) == NULL)
    {
        return ERR_IF;
    }
    netif_set_default(&interface);
    initialized = true;
    ready = false;
    last_phy_poll = now - 10u; /* First foreground pass polls immediately. */
    return ERR_OK;
}

void lm3s_netif_service(uint32_t now)
{
    if (!initialized)
    {
        return;
    }
    if (net_clock_elapsed(now, last_phy_poll, 10u))
    {
        ethernet_status_t status = {0};
        const ethernet_result_t result = ethernet_poll(&status);
        const bool next_ready = result == ETHERNET_OK && status.mac_ready;
        last_phy_poll = now;
        if (ready && !next_ready)
        {
            ready = false; /* Prevent teardown callbacks from submitting TX. */
            netif_set_down(&interface);
            netif_set_link_down(&interface);
            /* netif_set_down also cleans ARP in 2.2.1; explicit for contract. */
            etharp_cleanup_netif(&interface);
            ftp_tcp_link_down();
        }
        else if (!ready && next_ready)
        {
            ready = true;
            netif_set_link_up(&interface);
            netif_set_up(&interface);
        }
    }
    for (unsigned count = 0; count < 8u; ++count)
    {
        size_t length = 0;
        const ethernet_result_t result = ethernet_read_frame(
            ready ? rx_frame : NULL, ready ? sizeof(rx_frame) : 0u, &length);
        if (result == ETHERNET_EMPTY)
        {
            break;
        }
        if (result == ETHERNET_DROPPED)
        {
            continue;
        }
        if (result != ETHERNET_OK)
        {
            break;
        }
        if (!ready || length < ETHERNET_FRAME_HEADER_BYTES ||
            length > sizeof(rx_frame))
        {
            continue;
        }
        struct pbuf *packet = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
        if (packet == NULL)
        {
            continue; /* Driver already consumed frame; no ownership to leak. */
        }
        if (pbuf_take(packet, rx_frame, (u16_t)length) != ERR_OK ||
            interface.input(packet, &interface) != ERR_OK)
        {
            pbuf_free(packet); /* Successful input takes ownership. */
        }
    }
}

bool lm3s_netif_ready(void)
{
    return ready;
}

const ip4_addr_t *lm3s_netif_address(void)
{
    return netif_ip4_addr(&interface);
}
