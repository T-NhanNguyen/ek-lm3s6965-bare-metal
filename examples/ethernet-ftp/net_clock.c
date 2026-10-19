#include "net_clock.h"
#include "memory_map.h"

#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
#include "lm3s6965/boot_startup.h"
#include "lm3s6965/boot_startup_gptm.h"
#include "net_clock_port.h"
static uint32_t startup_elapsed_ms;
static bool initialized;
#endif
#define TICKS_PER_SECOND 1000u
#define SYSTICK_MAX_PERIOD 0x01000000u
#define ICSR UINT32_C(0xe000ed04)

/* Single ISR writer, aligned atomic word loads on Cortex-M3. Volatile gives
 * compiler visibility, not general synchronization. Initialized while masked. */
static volatile uint32_t milliseconds __attribute__((aligned(4)));

#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
static bool interrupts_prepare(void)
{
    if (!net_clock_port_masked_thread()) return false;
    /* LM3S6965 vectors extend through IRQ42, in two NVIC banks. Keep EVERY external
     * interrupt masked, including level producers not physically quiesced by
     * the still-modeled boot wrapper. Ethernet remains RX-polled/NO_SYS.
     * This is not peripheral producer shutdown or a protected-wrapper proof. */
    for (uint32_t bank = 0; bank < 2u; ++bank) {
        REGISTER32(UINT32_C(0xe000e180) + bank * 4u) = UINT32_MAX;
        REGISTER32(UINT32_C(0xe000e280) + bank * 4u) = UINT32_MAX;
    }
    REGISTER32(ICSR) = (1u << 27) | (1u << 25); /* PendSV/SysTick stale clear */
    net_clock_port_barrier();
    /* Reject enabled/pending configurable faults or NMI, not assumed absent.
     * NMI cannot be masked here; its physical producer is a boot obligation. */
    return (REGISTER32(UINT32_C(0xe000ed24)) & UINT32_C(0x0007f000)) == 0u &&
        (REGISTER32(ICSR) & ((1u << 31) | (1u << 28))) == 0u;
}
static bool interrupts_safe(void)
{
    if (!net_clock_port_masked_thread()) return false;
    for (uint32_t bank = 0; bank < 2u; ++bank) {
        if (REGISTER32(UINT32_C(0xe000e100) + bank * 4u) != 0u ||
            REGISTER32(UINT32_C(0xe000e200) + bank * 4u) != 0u) return false;
    }
    return (REGISTER32(UINT32_C(0xe000ed24)) & UINT32_C(0x0007f000)) == 0u &&
        (REGISTER32(ICSR) & ((1u << 31) | (1u << 28))) == 0u;
}
#endif

bool net_clock_initialize(uint32_t cpu_clock_hz)
{
    const uint32_t period = cpu_clock_hz / TICKS_PER_SECOND;
    if (cpu_clock_hz % TICKS_PER_SECOND != 0u || period == 0u ||
        period > SYSTICK_MAX_PERIOD) return false;
#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
    boot_startup_t *const c = &boot_startup_continuity;
    if (initialized || c->state != BOOT_STARTUP_C_READY || cpu_clock_hz != c->hz ||
        !boot_startup_gptm_supported(c) || !interrupts_prepare()) return false;
#endif
    REGISTER32(SYSTICK_BASE_ADDRESS) = 0u;
    REGISTER32(ICSR) = 1u << 25; /* clear stale pending before start */
    milliseconds = 0u;
    REGISTER32(SYSTICK_BASE_ADDRESS + 4u) = period - 1u;
    REGISTER32(SYSTICK_BASE_ADDRESS + 8u) = 0u;
    /* ENABLE | TICKINT | CLKSOURCE. Never changes RCC/PLL or WDT rate/load. */
    REGISTER32(SYSTICK_BASE_ADDRESS) = 7u;
#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
    net_clock_port_barrier();
    uint64_t us;
    /* Actual final GPTM sample AFTER SysTick start, still masked. Covers
     * pre-C entry through the switch, including the snapshot/start gap. */
    if (!boot_startup_time(c, &us) || us < c->entry_us) return false;
    const uint64_t delta = us - c->entry_us;
    /* Both samples truncate to us: add one us for conservative difference,
     * then ceil to ms. Startup is bounded by one native 32-bit GPTM period.
     * Division form avoids adding 1000 to a potentially large value. */
    const uint64_t offset = delta / 1000u + 1u;
    if (offset > UINT32_MAX) return false;
    startup_elapsed_ms = (uint32_t)offset;
    if (!interrupts_safe()) return false;
    initialized = true;
    net_clock_port_unmask();
#endif
    return true;
}

void SysTick_Handler(void)
{
    /* Time publication ONLY: no retained GPTM, WDT, lwIP or owner access. */
    ++milliseconds;
}

uint32_t sys_now(void)
{
#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
    if (!initialized) boot_startup_stop();
#endif
    return milliseconds;
}

#if defined(LM3S6965_FTP_FOREGROUND_OWNER)
bool net_clock_startup_elapsed(uint32_t *ms)
{
    if (!initialized || ms == NULL) return false;
    *ms = startup_elapsed_ms;
    return true;
}
#endif

bool net_clock_elapsed(uint32_t now, uint32_t since, uint32_t interval)
{
    return interval < UINT32_C(0x80000000) &&
        (uint32_t)(now - since) >= interval;
}
