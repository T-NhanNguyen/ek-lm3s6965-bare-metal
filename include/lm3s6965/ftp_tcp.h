#ifndef LM3S6965_FTP_TCP_H
#define LM3S6965_FTP_TCP_H
#include <stdint.h>
#include "lwip/err.h"
#include "lwip/ip4_addr.h"

/* One singleton server, foreground serialized use only, pinned lwIP 2.2.1.
 * Requires a dedicated lwIP stack: link_down sweeps all stack-owned TCP PCBs,
 * so other TCP services cannot coexist. No ISR/reentrant/multi-server use.
 * Bind core with ftp_core_init before starting. Start never resets storage.
 * ERR_ARG: NULL address; ERR_VAL: missing core binding; ERR_OK: started.
 * Other lwIP allocation/bind/listen errors leave no partial listener; retry
 * once ready. Nonblocking and idempotent while started. Backend lifetime and
 * explicit cold-boot reset policy belong to caller, not this adapter. */
err_t ftp_tcp_start(const ip4_addr_t *address);
/* Called every loop, even down. Unsigned modular milliseconds from sys_now. */
void ftp_tcp_service(uint32_t now);
/* Called after administrative/link down and ARP cleanup. Abort all control,
 * data and listening PCBs, release retained pbufs/state; safe if not started.
 * No network output, waits or stdio. Allows a fresh start on readiness. */
void ftp_tcp_link_down(void);
#endif
