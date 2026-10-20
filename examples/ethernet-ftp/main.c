#include <stdbool.h>
#include <stdint.h>
#include "system_control.h"
#include "memory_map.h"
#include "net_clock.h"
#include "lm3s_netif.h"
#include "lm3s6965/ftp_tcp.h"
#include "lm3s6965/ftp_core.h"
#include "lm3s6965/ram_file.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
#include "foreground_owner.h"
#include "fixture_led.h"
#include "lm3s6965/boot_startup.h"
#include "lm3s6965/boot_startup_gptm.h"
#endif

#if LWIP_VERSION_MAJOR != 2 || LWIP_VERSION_MINOR != 2 || LWIP_VERSION_REVISION != 1
#error "This platform contract requires lwIP 2.2.1"
#endif

/* These objects and the immutable basename outlive the bound singleton.
 * No other owner accesses either buffer while FTP is running. */
static ram_file_t file;
static uint8_t committed_buffer[4096];
static uint8_t staging_buffer[4096];
static const char basename[] = "hello";

static void fail_closed(void)
{
#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
    boot_startup_stop();
#endif
    for (;;)
    {
        /* No runtime console, protocol work or hardware retry loop. */
    }
}

int main(void)
{
    if (!ram_file_init(&file, committed_buffer, staging_buffer,
                       sizeof committed_buffer, basename) ||
        !ftp_core_init(ram_file_storage(&file)))
    {
        fail_closed();
    }
    /* Explicit cold-boot policy only; link/session cleanup preserves data. */
    ftp_core_reset();

#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
    /* Validate the clock while masked and publish APP; no service evidence
     * or feed. The final transition sample occurs later. */
    if (!boot_startup_c_ready(&boot_startup_continuity))
    {
        fail_closed();
    }
    /* Adopt only the explicit startup GPTM capability. Start application
     * SysTick while masked, sample GPTM AFTER start, commit conservative entry
     * offset, and safely unmask only SysTick. Never restart/retransfer WDT. */
    const uint32_t clock_hz = boot_startup_continuity.hz;
    /* Still masked: PF0 and pattern state must precede SysTick enable. */
    fixture_led_initialize();
    if (!net_clock_initialize(clock_hz))
    {
        fail_closed();
    }
#else
    const bool pll_locked = system_control_configure_pll();
    const uint32_t rcc = system_control_read_rcc();
    const uint32_t clock_hz = pll_locked ? SYSTEM_CLOCK_FREQUENCY_HZ :
        EXTERNAL_CRYSTAL_FREQUENCY_HZ;
    const uint32_t expected = RCC_XTAL_8_000_MHZ | RCC_OSCSRC_MAIN |
        (pll_locked ? (RCC_USESYSDIV |
         RCC_SYSDIV_FOR_DIVISOR(SYSTEM_CLOCK_DIVISOR)) : RCC_BYPASS);
    const uint32_t mask = RCC_XTAL_MASK | RCC_OSCSRC_MASK | RCC_MOSCDIS |
        RCC_BYPASS | RCC_USESYSDIV | (pll_locked ? RCC_SYSDIV_MASK : 0u);
    /* Reject unexpected clock readback (including RCC2 override). This is a
     * source/divider check, not a measurement of physical oscillator rate. */
    if ((rcc & mask) != expected ||
        (REGISTER32(SYSTEM_CONTROL_BASE_ADDRESS + SYSTEM_CONTROL_RCC2_OFFSET) &
         (1u << 31)) != 0u || !net_clock_initialize(clock_hz))
    {
        fail_closed();
    }
#endif
    lwip_init();
    if (lm3s_netif_initialize(clock_hz, sys_now()) != ERR_OK)
    {
        fail_closed();
    }
#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
    /* Networking now uses SysTick only. Release precisely the adapter-named
     * GPTM after lwIP/netif init; never guess Timer0 or stop an unknown source. */
    if (!boot_startup_gptm_release(&boot_startup_continuity))
    {
        fail_closed();
    }
    for (;;)
    {
        if (!foreground_owner_turn())
        {
            fail_closed();
        }
    }
#else
    bool transport_started = false;
    for (;;)
    {
        const uint32_t now = sys_now();
        lm3s_netif_service(now);
        if (!lm3s_netif_ready())
        {
            transport_started = false;
        }
        else if (!transport_started)
        {
            transport_started = ftp_tcp_start(lm3s_netif_address()) == ERR_OK;
        }
        sys_check_timeouts();
        /* Services retained TCP prefixes and at most ONE optional operation
         * step, outside raw callbacks, after Ethernet and timers. This image
         * binds RAM hello only: no boot/controller/running-provenance startup
         * objects or reset actuator exist here. An updater must supply those
         * and bind ftp_boot_operation explicitly; do not enable a fake one. */
        (void)ftp_tcp_service(sys_now());
    }
#endif
}
