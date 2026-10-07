#ifndef FTP_LM3S_NETIF_H
#define FTP_LM3S_NETIF_H
#include <stdbool.h>
#include <stdint.h>
#include "lwip/netif.h"

/* One instance, foreground owner. Call after lwip_init and clock startup. */
err_t lm3s_netif_initialize(uint32_t cpu_clock_hz, uint32_t now);
/* Poll PHY at most once per 10 ms; consume at most eight RX frames per call.
 * Down frames are discarded. Link loss flushes ARP and calls FTP teardown. */
void lm3s_netif_service(uint32_t now);
bool lm3s_netif_ready(void);
const ip4_addr_t *lm3s_netif_address(void);
#endif
