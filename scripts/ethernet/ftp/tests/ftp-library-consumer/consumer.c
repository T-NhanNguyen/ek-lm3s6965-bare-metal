#include "lm3s6965/ftp_tcp.h"
#include "lm3s6965/ftp_core.h"
#include "lm3s6965/ram_file.h"
#include "lwip/init.h"
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#ifndef CALLER_LWIP_CONTRACT
#error "Missing transitive caller definition"
#endif
_Static_assert(TCP_MSS == 256 && PBUF_POOL_SIZE == 6 && MEM_SIZE == 8192,
               "Example config leaked into caller");
static ram_file_t file;
static uint8_t committed[113], staging[113];
static uint32_t ticks;
u32_t sys_now(void) { return ticks; }
/* Link-only probe, not board firmware: no vector table or hardware startup. */
void consumer_entry(void)
{
    ip4_addr_t address;
    IP4_ADDR(&address, 192, 0, 2, 1);
    if (!ram_file_init(&file, committed, staging, sizeof committed, "owned") ||
        !ftp_core_init(ram_file_storage(&file))) __builtin_trap();
    ftp_core_reset();
    lwip_init();
    (void)ftp_tcp_start(&address);
    ftp_tcp_service(++ticks);
    sys_check_timeouts();
    ftp_tcp_link_down();
    for (;;) { }
}
