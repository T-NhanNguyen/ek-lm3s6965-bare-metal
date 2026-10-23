#ifndef FTP_NET_CLOCK_PORT_H
#define FTP_NET_CLOCK_PORT_H
#include <stdbool.h>

/* Foreground core boundary, not a tick source or runtime callback interface.
 * Production uses net_clock_port.c. Native fixtures link explicit definitions
 * of these three functions INSTEAD of that file; MMIO remains in net_clock.c. */

/* True only in privileged Thread mode with PRIMASK=1 and BASEPRI=FAULTMASK=0,
 * CONTROL=0. Must not change core state. Unsupported cores fail closed. */
bool net_clock_port_masked_thread(void);
/* Complete prior MMIO writes and synchronize the instruction stream (DSB/ISB). */
void net_clock_port_barrier(void);
/* Commit the already validated transition: DSB, enable IRQs, ISB. External NVIC
 * sources remain masked; this does not enable any peripheral interrupt. */
void net_clock_port_unmask(void);

#endif
