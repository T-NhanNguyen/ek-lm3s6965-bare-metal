#include "lm3s6965/boot_startup_producer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_diag.h"
#include "lm3s6965/boot_startup.h"
#include "lm3s6965/boot_startup_gptm.h"
#include "lm3s6965/boot_watchdog.h"
#include "lm3s6965/boot_watchdog_target.h"

static bool overlap(const void *a, size_t n, const void *b, size_t m)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < n : x - y < m;
}

static bool boot_code(uintptr_t p)
{
#if defined(__arm__) || defined(__thumb__)
    return (p & 1u) != 0 && p < 0x8000u;
#else
    return p != 0; /* HOST cannot establish target placement. */
#endif
}

static bool arguments(boot_startup_t *c, volatile uint32_t *diagnostic,
    const boot_watchdog_io_t *io, void (*delay)(void *))
{
    if (c == NULL || diagnostic == NULL || io == NULL ||
        ((uintptr_t)c % _Alignof(boot_startup_t)) != 0 ||
        ((uintptr_t)diagnostic & 3u) != 0 ||
        overlap(c, sizeof(*c), (const void *)diagnostic, BOOT_DIAG_BYTES) ||
        overlap(c, sizeof(*c), io, sizeof(*io)) ||
        overlap((const void *)diagnostic, BOOT_DIAG_BYTES, io, sizeof(*io)) ||
        io->context != (void *)c->context ||
        !boot_code((uintptr_t)io->read_register) ||
        !boot_code((uintptr_t)io->write_register) ||
        !boot_code((uintptr_t)delay) ||
        !boot_code((uintptr_t)boot_startup_gptm_read)) return false;
#if defined(__arm__) || defined(__thumb__)
    uint32_t mask, ipsr, control;
    __asm__ volatile("mrs %0, primask\n mrs %1, ipsr\n mrs %2, control"
        : "=r"(mask), "=r"(ipsr), "=r"(control));
    if ((uintptr_t)c != 0x2000dbc0u ||
        (uintptr_t)diagnostic != 0x2000dfc0u || mask != 1u || ipsr != 0u ||
        (control & 1u) != 0u) return false;
#endif
    return true;
}

static bool failed(boot_startup_t *c)
{
    /* No cleanup through potentially failed/unready peripheral IO. */
    c->watchdog.blocked = 1u;
    c->watchdog.evidence = 0u;
    c->active = 0u;
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("dmb" ::: "memory");
#endif
    c->state = BOOT_STARTUP_FAILED;
    return false;
}

bool boot_startup_producer_begin_io(boot_startup_t *c,
    volatile uint32_t *diagnostic, uint32_t flags, uint32_t hz,
    const boot_watchdog_io_t *io, void (*delay)(void *))
{
    /* Even diagnostic publication is an effect: check arithmetic first. */
    if (hz == 0u || hz > UINT32_MAX / 5u ||
        !arguments(c, diagnostic, io, delay)) return false;
    const uint32_t reload = hz * 5u;
    const boot_watchdog_io_t registers = *io;
    const boot_startup_clock_t clock = {
        .read = boot_startup_gptm_read, .context = c->context,
        .rcc_mask = UINT32_MAX, .rcc2_mask = UINT32_MAX,
        .hz = hz, .source = BOOT_STARTUP_GPTM_SOURCE
    };
    if (!boot_startup_reset(c, diagnostic, flags, &registers, &clock))
        return failed(c);
    if (!boot_watchdog_target_clock_ready(&registers, delay)) return failed(c);
    boot_startup_gptm_t *g = (void *)c->context;
    if (!boot_startup_gptm_claim(g, BOOT_STARTUP_PRODUCER_TIMER, hz,
                               &registers, delay)) return failed(c);
    /* Claim observed and matched RCC/RCC2, not caller expectations. Hz remains
     * caller authority; subsequent backend matches reject register drift. */
    c->clock.rcc = g->rcc;
    c->clock.rcc2 = g->rcc2;
    if (boot_watchdog_configure(&c->watchdog, hz, reload) != BOOT_WATCHDOG_OK)
        return failed(c);
    c->hz = hz;
    c->reload = reload;
    return true;
}

bool boot_startup_producer_begin(boot_startup_t *c,
    volatile uint32_t *diagnostic, uint32_t flags, uint32_t hz)
{
    const boot_watchdog_io_t io = {
        .context = c == NULL ? NULL : (void *)c->context,
        .read_register = boot_startup_gptm_direct_read,
        .write_register = boot_startup_gptm_direct_write
    };
    return boot_startup_producer_begin_io(c, diagnostic, flags, hz, &io,
                                        boot_startup_gptm_direct_delay);
}
