#include "lm3s6965/boot_startup_gptm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_startup.h"
#include "lm3s6965/boot_watchdog.h"

#define RCGC1 UINT32_C(0x400fe104)
#define RCC UINT32_C(0x400fe060)
#define RCC2 UINT32_C(0x400fe070)
#define CLAIMED UINT32_C(0x47505431)
#define RELEASED UINT32_C(0x47505432)
#define CLAIMING UINT32_C(0x47505433)
#define READING UINT32_C(0x47505434)
#define RELEASING UINT32_C(0x47505435)
#define FAILED UINT32_C(0x47505436)
/* SPMS144I GPTMICR: defined W1C bits 10,9,8,3,2,1,0 only. */
#define GPTMICR_DEFINED_W1C_MASK UINT32_C(0x70f)

static bool busy(const boot_startup_gptm_t *g)
{
    return g->state == CLAIMING || g->state == READING ||
        g->state == RELEASING;
}
/* The existing state word is both operation guard and sticky failure latch.
 * Nested operations poison the holder, even if the callback ignores failure. */
static bool reentered(boot_startup_gptm_t *g)
{
    if (g != NULL && busy(g)) { g->state = FAILED; return true; }
    return false;
}

static uint32_t base(const boot_startup_gptm_t *g)
{
    return UINT32_C(0x40030000) + g->timer * UINT32_C(0x1000);
}
static bool read_reg(boot_startup_gptm_t *g, uint32_t a, uint32_t *v)
{
    const uint32_t held = g->state;
    if (!busy(g)) return false;
    const int result = g->registers.read_register(g->registers.context, a, v);
    if (result != 0 || g->state != held) { g->state = FAILED; return false; }
    return true;
}
static bool write_reg(boot_startup_gptm_t *g, uint32_t a, uint32_t v)
{
    const uint32_t held = g->state;
    if (!busy(g)) return false;
    const int result = g->registers.write_register(g->registers.context, a, v);
    if (result != 0 || g->state != held) { g->state = FAILED; return false; }
    return true;
}
static bool equal(boot_startup_gptm_t *g, uint32_t a, uint32_t v)
{
    uint32_t observed;
    return read_reg(g, a, &observed) && observed == v;
}
static bool matched(boot_startup_gptm_t *g, uint32_t held)
{
    uint32_t gate;
    if (g == NULL || g->state != held || !busy(g) || g->timer > 3u ||
        g->hz == 0 || g->hz > UINT32_MAX / 5u ||
        g->registers.read_register == NULL || g->registers.write_register == NULL ||
        !read_reg(g, RCGC1, &gate) || !(gate & (1u << (16u + g->timer))))
        return false;
    const uint32_t b = base(g);
    /* Exact full-width periodic down configuration, no trigger, debug stall,
     * alternate mode, match interrupt or interrupt producer. */
    return equal(g, b, 0u) && equal(g, b + 4u, 2u) &&
        equal(g, b + 12u, 1u) && equal(g, b + 24u, 0u) &&
        equal(g, b + 40u, UINT32_MAX) &&
        equal(g, RCC, g->rcc) && equal(g, RCC2, g->rcc2);
}
bool boot_startup_gptm_claim(boot_startup_gptm_t *g, unsigned timer,
    uint32_t hz, const boot_watchdog_io_t *io, void (*delay)(void *))
{
    uint32_t gate, ctl;
    if (reentered(g) || g == NULL || io == NULL || delay == NULL || timer > 3u ||
        hz == 0 || hz > UINT32_MAX / 5u || g->state != 0u ||
        io->read_register == NULL || io->write_register == NULL)
        return false;
    g->registers = *io;
    g->gate_delay = delay;
    g->hz = hz;
    g->timer = timer;
    g->reader_identity = boot_startup_gptm_read;
    /* Consume even a partially failed claim: no retry/adoption of live state. */
    g->state = CLAIMING;
    if (!read_reg(g, RCGC1, &gate) ||
        !write_reg(g, RCGC1, gate | (1u << (16u + timer))) ||
        !equal(g, RCGC1, gate | (1u << (16u + timer)))) goto failed;
    g->gate_delay(g->registers.context);
    if (g->state != CLAIMING) goto failed;
    const uint32_t b = base(g);
    if (!read_reg(g, b + 12u, &ctl) || ctl != 0u ||
        !read_reg(g, RCC, &g->rcc) || !read_reg(g, RCC2, &g->rcc2) ||
        !write_reg(g, b, 0u) || !write_reg(g, b + 4u, 2u) ||
        !write_reg(g, b + 24u, 0u) ||
        !write_reg(g, b + 40u, UINT32_MAX) ||
        /* Clear defined stale status ONLY at claim; write reserved bits as zero. */
        !write_reg(g, b + 36u, GPTMICR_DEFINED_W1C_MASK) ||
        !write_reg(g, b + 12u, 1u) || !matched(g, CLAIMING)) goto failed;
    g->state = CLAIMED;
    return true;
failed:
    g->state = FAILED;
    return false;
}
int boot_startup_gptm_read(void *context, boot_startup_sample_t *out)
{
    boot_startup_gptm_t *g = context;
    uint32_t before, counter, after;
    if (reentered(g) || g == NULL || g->state != CLAIMED) return -1;
    g->state = READING;
    if (out == NULL || !matched(g, READING)) goto failed;
    const uint32_t b = base(g);
    /* RIS is sticky for the entire claim. Reject expiry before OR racing TAR;
     * never reconstruct arbitrary wraps or clear status on a sample/feed. */
    if (!read_reg(g, b + 28u, &before) || before != 0u ||
        !read_reg(g, b + 72u, &counter) ||
        !read_reg(g, b + 28u, &after) || after != 0u ||
        !matched(g, READING)) goto failed;
    out->us = ((uint64_t)(UINT32_MAX - counter) * UINT64_C(1000000)) / g->hz;
    out->rcc = g->rcc;
    out->rcc2 = g->rcc2;
    out->hz = g->hz;
    out->source = BOOT_STARTUP_GPTM_SOURCE;
    g->state = CLAIMED;
    return 0;
failed:
    g->state = FAILED;
    return -1;
}
bool boot_startup_gptm_supported(const boot_startup_t *c)
{
    if (c == NULL || c->clock.source != BOOT_STARTUP_GPTM_SOURCE ||
        c->clock.context != (const void *)c->context) return false;
    const boot_startup_gptm_t *g = (const void *)c->context;
    /* Boot and signed application link separate copies of this source. Compare
     * the caller-established BOOT identity, never the app-local function PC. */
    return g->state == CLAIMED && g->reader_identity != NULL &&
        c->clock.read == g->reader_identity && c->hz == g->hz;
}
bool boot_startup_gptm_release(boot_startup_t *c)
{
    if (c == NULL) return false;
    boot_startup_gptm_t *g = (void *)c->context;
    if (reentered(g) || !boot_startup_gptm_supported(c) || c->active ||
        c->state != BOOT_STARTUP_C_READY) return false;
    /* Consume before callback validation, not merely before the CTL write.
     * matched explicitly accepts this held release, never a new public op. */
    g->state = RELEASING;
    if (!matched(g, RELEASING) || !equal(g, base(g) + 28u, 0u) ||
        !write_reg(g, base(g) + 12u, 0u) || !equal(g, base(g) + 12u, 0u)) {
        g->state = FAILED;
        return false;
    }
    g->state = RELEASED;
    return true;
}
int boot_startup_gptm_direct_read(void *context, uint32_t address, uint32_t *value)
{
    (void)context;
    *value = *(volatile uint32_t *)(uintptr_t)address;
    return 0;
}
int boot_startup_gptm_direct_write(void *context, uint32_t address, uint32_t value)
{
    (void)context;
    *(volatile uint32_t *)(uintptr_t)address = value;
    return 0;
}
void boot_startup_gptm_direct_delay(void *context)
{
    (void)context;
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("nop\n nop\n nop" ::: "memory");
#endif
}
