#include "lm3s6965/boot_handoff_target.h"

#include <stddef.h>
#include <stdint.h>
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_startup_gptm.h"

/* SPMS144I pp106-119,124-127: W1-clear bank masks include HIB IRQ43.
 * ICSR clear bits are WO: test pending SET bits, never RMW ICSR. */
#define STCTRL UINT32_C(0xe000e010)
#define DIS0 UINT32_C(0xe000e180)
#define UNPEND0 UINT32_C(0xe000e280)
#define ACTIVE0 UINT32_C(0xe000e300)
#define ICSR UINT32_C(0xe000ed04)
#define VTOR UINT32_C(0xe000ed08)
#define BANK1_MASK UINT32_C(0xfff)
#define CORE_PENDING UINT32_C(0x94000000) /* NMI, PendSV, SysTick */
#define CORE_CLEAR UINT32_C(0x0a000000)
#define RCGC0 UINT32_C(0x400fe100)
#define RCGC1 UINT32_C(0x400fe104)
#define RCGC2 UINT32_C(0x400fe108)
#define RCC UINT32_C(0x400fe060)
#define RCC2 UINT32_C(0x400fe070)
#define HIBIM UINT32_C(0x400fc014)
#define HIBMIS UINT32_C(0x400fc01c)
#define IMC UINT32_C(0x400fe054)
#define FCIM UINT32_C(0x400fd010)
#define FMC UINT32_C(0x400fd008)
#define WDT_PENDING UINT32_C(0x40000) /* IRQ18 PEND0 bit: live WDT exempt iff IRQ18 disabled */
#define WDTCTL UINT32_C(0x40000008)
#define WDTRIS UINT32_C(0x40000010)
#define TIMER2 UINT32_C(0x40032000)

static int core_valid(const boot_handoff_context_t *o)
{
    return o->ipsr == 0u && (o->control & ~UINT32_C(2)) == 0u &&
        o->primask == 1u && (o->basepri & ~UINT32_C(0xe0)) == 0u &&
        o->faultmask <= 1u;
}

int boot_handoff_target_observe(boot_handoff_context_t *out)
{
    if (out == NULL) return -1;
#if defined(__arm__) || defined(__thumb__)
    boot_handoff_context_t o;
    __asm__ volatile("mrs %0, ipsr\n mrs %1, control\n mrs %2, primask\n"
                     "mrs %3, basepri\n mrs %4, faultmask"
        : "=r"(o.ipsr), "=r"(o.control), "=r"(o.primask),
          "=r"(o.basepri), "=r"(o.faultmask) :: "memory");
    if (!core_valid(&o)) return -1;
    *out = o;
    return 0;
#else
    return -1;
#endif
}

static int direct_observe(void *context, boot_handoff_context_t *out)
{
    (void)context;
    return boot_handoff_target_observe(out);
}
static int direct_read(void *context, uint32_t address, uint32_t *out)
{
    (void)context;
#if defined(__arm__) || defined(__thumb__)
    *out = *(volatile const uint32_t *)(uintptr_t)address;
    return 0;
#else
    (void)address; (void)out;
    return -1;
#endif
}
static int direct_write(void *context, uint32_t address, uint32_t value)
{
    (void)context;
#if defined(__arm__) || defined(__thumb__)
    *(volatile uint32_t *)(uintptr_t)address = value;
    return 0;
#else
    (void)address; (void)value;
    return -1;
#endif
}
static int direct_barrier(void *context, boot_handoff_target_barrier_t kind)
{
    (void)context;
#if defined(__arm__) || defined(__thumb__)
    if (kind == BOOT_HANDOFF_TARGET_EXCLUDE)
        __asm__ volatile("cpsid i" ::: "memory");
    else if (kind != BOOT_HANDOFF_TARGET_SYNC) return -1;
    __asm__ volatile("dsb\n isb" ::: "memory");
    return 0;
#else
    (void)kind;
    return -1;
#endif
}

int boot_handoff_target_init(boot_handoff_target_t *t, boot_startup_t *c,
    const boot_handoff_target_io_t *io, boot_handoff_t *handoff)
{
    const boot_handoff_target_io_t direct = {
        NULL, direct_read, direct_write, direct_observe, direct_barrier
    };
    if (t == NULL || c == NULL || handoff == NULL || t->initialized != 0u)
        return -1;
    const boot_handoff_target_io_t selected = io == NULL ? direct : *io;
    if (selected.read_register == NULL || selected.write_register == NULL ||
        selected.observe == NULL || selected.barrier == NULL) return -1;
    *t = (boot_handoff_target_t){ .io = selected, .continuity = c,
                                  .initialized = 1u };
    const boot_handoff_io_t binding = {
        t, boot_handoff_target_operation, boot_handoff_target_terminal_intent
    };
    return boot_handoff_init(handoff, &binding) == BOOT_HANDOFF_OK ? 0 : -1;
}

/* IO failures and ignored callback reentry poison the current operation before
 * it can issue any further IO. This is a synchronous guard, not a lock. */
static int read_reg(boot_handoff_target_t *t, uint32_t a, uint32_t *v)
{
    if (t->failed || t->io.read_register(t->io.context, a, v) != 0)
        t->failed = 1u;
    return t->failed == 0u;
}
static int write_reg(boot_handoff_target_t *t, uint32_t a, uint32_t v)
{
    if (t->failed || t->io.write_register(t->io.context, a, v) != 0)
        t->failed = 1u;
    return t->failed == 0u;
}
static int matches(boot_handoff_target_t *t, uint32_t a, uint32_t mask,
                   uint32_t expected)
{
    uint32_t v;
    return read_reg(t, a, &v) && (v & mask) == expected;
}
static int observe(boot_handoff_target_t *t, boot_handoff_context_t *o)
{
    if (t->failed || t->io.observe(t->io.context, o) != 0)
        t->failed = 1u;
    return !t->failed && core_valid(o);
}
static int synchronize(boot_handoff_target_t *t,
                       boot_handoff_target_barrier_t kind)
{
    if (t->failed || t->io.barrier(t->io.context, kind) != 0)
        t->failed = 1u;
    return !t->failed;
}
static int core_unchanged(boot_handoff_target_t *t)
{
    boot_handoff_context_t o;
    return observe(t, &o) && o.control == t->original.control &&
        o.basepri == t->original.basepri && o.faultmask == t->original.faultmask;
}
static int banks_idle(boot_handoff_target_t *t)
{
    for (unsigned i = 0u; i < 2u; ++i) {
        uint32_t mask = i == 0u ? UINT32_MAX : BANK1_MASK;
        if (!matches(t, DIS0 + 4u*i, mask, 0u) ||
            !matches(t, UNPEND0 + 4u*i, mask, 0u) ||
            !matches(t, ACTIVE0 + 4u*i, mask, 0u)) return 0;
    }
    return matches(t, ICSR, CORE_PENDING, 0u);
}
/* Pre-clear gate: live WDT (IRQ18, PEND0 0x40000) exempt while IRQ18 stays
 * disabled; enables/active/ICSR remain strict zero. Pending is cleared later
 * via NVIC ICPR (UNPEND0 write), never WDTICR. Post-clear gates stay strict. */
static int banks_idle_pre(boot_handoff_target_t *t)
{
    uint32_t v;
    for (unsigned i = 0u; i < 2u; ++i) {
        uint32_t mask = i == 0u ? UINT32_MAX : BANK1_MASK;
        uint32_t allow = i == 0u ? WDT_PENDING : 0u;
        if (!read_reg(t, DIS0 + 4u*i, &v) || (v & mask) != 0u) return 0;
        if (!read_reg(t, UNPEND0 + 4u*i, &v) || ((v & mask) & ~allow) != 0u)
            return 0;
        if (!read_reg(t, ACTIVE0 + 4u*i, &v) || (v & mask) != 0u) return 0;
    }
    return matches(t, ICSR, CORE_PENDING, 0u);
}

/* Closed boot-only source allowlist, not a general peripheral shutdown.
 * SPMS144I pp214-215,220-222,229-230: defined RCGC fields only;
 * reset HIB gate + owned WDT/Timer2, no demo peripheral clocks.
 * pp191-194,254-257,269-273,377-380: masked sources/completed flash,
 * retained HIB raw wake history permitted, live watchdog explicitly exempt. */
static int sources_idle(boot_handoff_target_t *t)
{
    boot_startup_t *c = t->continuity;
    boot_startup_sample_t sample;
    if (c->magic != BOOT_STARTUP_MAGIC || c->abi != BOOT_STARTUP_ABI ||
        c->state != BOOT_STARTUP_EMPTY || c->active != 0u ||
        !boot_startup_gptm_supported(c)) return 0;
    const boot_startup_gptm_t *g = (const void *)c->context;
    if (g->timer != 2u || c->clock.read == NULL ||
        c->clock.rcc_mask != UINT32_MAX || c->clock.rcc2_mask != UINT32_MAX ||
        c->clock.rcc != g->rcc || c->clock.rcc2 != g->rcc2 ||
        !matches(t, RCGC0, UINT32_C(0x00110348), UINT32_C(0x48)) ||
        !matches(t, RCGC1, UINT32_C(0x030f5317), UINT32_C(0x40000)) ||
        !matches(t, RCGC2, UINT32_C(0x5000007f), 0u)) return 0;
    if (!matches(t, HIBIM, 15u, 0u) || !matches(t, HIBMIS, 15u, 0u) ||
        !matches(t, IMC, UINT32_C(0x42), 0u) ||
        !matches(t, FCIM, 3u, 0u) || !matches(t, FMC, 15u, 0u) ||
        !matches(t, WDTCTL, 3u, 3u) || !matches(t, WDTRIS, 1u, 0u) ||
        !matches(t, TIMER2, UINT32_MAX, 0u) ||
        !matches(t, TIMER2 + 4u, UINT32_MAX, 2u) ||
        !matches(t, TIMER2 + 12u, UINT32_MAX, 1u) ||
        !matches(t, TIMER2 + 24u, UINT32_MAX, 0u) ||
        !matches(t, TIMER2 + 28u, UINT32_MAX, 0u) ||
        !matches(t, TIMER2 + 40u, UINT32_MAX, UINT32_MAX) ||
        !matches(t, RCC, UINT32_MAX, c->clock.rcc) ||
        !matches(t, RCC2, UINT32_MAX, c->clock.rcc2)) return 0;
    /* Retained backend checks RIS around TAR and rechecks configuration/clock.
     * Its dispatch context is retained only, never this boot-local adapter. */
    if (c->clock.read(c->clock.context, &sample) != 0 || t->failed ||
        sample.rcc != c->clock.rcc || sample.rcc2 != c->clock.rcc2 ||
        sample.hz != c->hz || sample.source != BOOT_STARTUP_GPTM_SOURCE)
        return 0;
    return matches(t, WDTRIS, 1u, 0u);
}

static int capture(boot_handoff_target_t *t)
{
    if (!observe(t, &t->original)) return 0;
    /* Capture every bank before any destructive clear; even rejected inherited
     * activity remains visible in boot-local diagnostics. Only live WDT pending
     * (IRQ18, PEND0 0x40000, IRQ18 disabled) is exempt; all other IRQs zero. */
    for (unsigned i = 0u; i < 2u; ++i) {
        if (!read_reg(t, DIS0 + 4u*i, &t->enables[i]) ||
            !read_reg(t, UNPEND0 + 4u*i, &t->pending[i]) ||
            !read_reg(t, ACTIVE0 + 4u*i, &t->active[i])) return 0;
    }
    if (!read_reg(t, ICSR, &t->icsr) || !read_reg(t, STCTRL, &t->systick))
        return 0;
    if (t->enables[0] || (t->pending[0] & ~WDT_PENDING) || t->active[0] ||
        ((t->enables[1] | t->pending[1] | t->active[1]) & BANK1_MASK) ||
        (t->icsr & CORE_PENDING) || (t->systick & 3u)) return 0;
    boot_watchdog_t *w = &t->continuity->watchdog;
    if (!sources_idle(t) || !w->configured || w->blocked ||
        w->owner != BOOT_WATCHDOG_BOOT || w->epoch == 0u ||
        w->epoch == UINT32_MAX) return 0;
    t->epoch = w->epoch + 1u;
    return synchronize(t, BOOT_HANDOFF_TARGET_EXCLUDE) && core_unchanged(t);
}

static int valid_value(boot_handoff_operation_t op, uint32_t value)
{
    switch (op) {
        case BOOT_HANDOFF_SET_VTOR:
            return value == BOOT_SLOT_A_BASE || value == BOOT_SLOT_B_BASE;
        default: return value == 0u;
    }
}
static int apply(boot_handoff_target_t *t, boot_handoff_operation_t op,
                 uint32_t value)
{
    uint32_t v;
    switch (op) {
        case BOOT_HANDOFF_EXCLUDE: return capture(t);
        case BOOT_HANDOFF_STOP_SYSTICK:
            return read_reg(t, STCTRL, &v) &&
                write_reg(t, STCTRL, v & ~UINT32_C(3)) &&
                matches(t, STCTRL, 3u, 0u);
        case BOOT_HANDOFF_DISABLE_BANK0:
        case BOOT_HANDOFF_DISABLE_BANK1: {
            unsigned bank = op == BOOT_HANDOFF_DISABLE_BANK1;
            uint32_t mask = bank ? BANK1_MASK : UINT32_MAX;
            /* Pre/post full-bank gates allow live WDT pending: ICER writes do
             * not clear pending; ICPR clear comes later. Per-bank strict zero
             * is still enforced via the matches() read-back. */
            return banks_idle_pre(t) && write_reg(t, DIS0 + 4u*bank, mask) &&
                matches(t, DIS0 + 4u*bank, mask, 0u) && banks_idle_pre(t);
        }
        case BOOT_HANDOFF_CLEAR_BANK0_PENDING: {
            /* Pre-clear allows WDT pending; NVIC ICPR write clears it, then
             * post-clear is strict zero including WDT. Never touches WDTICR. */
            return banks_idle_pre(t) && write_reg(t, UNPEND0, UINT32_MAX) &&
                matches(t, UNPEND0, UINT32_MAX, 0u) && banks_idle(t);
        }
        case BOOT_HANDOFF_CLEAR_BANK1_PENDING: {
            /* WDT (bank0) already cleared; both gates strict. */
            return banks_idle(t) && write_reg(t, UNPEND0 + 4u, BANK1_MASK) &&
                matches(t, UNPEND0 + 4u, BANK1_MASK, 0u) && banks_idle(t);
        }
        case BOOT_HANDOFF_QUIESCE_SOURCES:
            return sources_idle(t) && banks_idle_pre(t);
        case BOOT_HANDOFF_CLEAR_CORE_PENDING:
            return banks_idle_pre(t) && write_reg(t, ICSR, CORE_CLEAR) &&
                matches(t, ICSR, CORE_PENDING, 0u);
        case BOOT_HANDOFF_BARRIER_BEFORE_VTOR:
        case BOOT_HANDOFF_BARRIER_AFTER_VTOR:
            return synchronize(t, BOOT_HANDOFF_TARGET_SYNC) &&
                core_unchanged(t) && sources_idle(t) && banks_idle(t) &&
                (op != BOOT_HANDOFF_BARRIER_AFTER_VTOR ||
                 matches(t, VTOR, UINT32_MAX, t->vtor));
        case BOOT_HANDOFF_SET_VTOR:
            if (!write_reg(t, VTOR, value) ||
                !matches(t, VTOR, UINT32_MAX, value)) return 0;
            t->vtor = value;
            return 1;
        default: return 0;
    }
}
static int enter(boot_handoff_target_t *t)
{
    if (t == NULL) return 0;
    if (t->initialized != 1u || t->busy || t->failed || t->recorded) {
        t->failed = 1u;
        return 0;
    }
    t->busy = 1u;
    return 1;
}
static int leave(boot_handoff_target_t *t, int ok)
{
    if (!ok) t->failed = 1u;
    t->busy = 0u;
    return t->failed ? -1 : 0;
}
int boot_handoff_target_operation(void *context, boot_handoff_operation_t op,
                                  uint32_t value)
{
    boot_handoff_target_t *t = context;
    if (!enter(t)) return -1;
    if ((unsigned)op > (unsigned)BOOT_HANDOFF_BARRIER_AFTER_VTOR ||
        (unsigned)op != t->next || !valid_value(op, value)) return leave(t, 0);
    if (op != BOOT_HANDOFF_EXCLUDE && !core_unchanged(t)) return leave(t, 0);
    if (!apply(t, op, value) || t->failed) return leave(t, 0);
    ++t->next;
    return leave(t, 1);
}
int boot_handoff_target_terminal_intent(void *context,
                                      const boot_handoff_intent_t *intent)
{
    boot_handoff_target_t *t = context;
    if (!enter(t)) return -1;
    if (intent == NULL || t->next != (unsigned)BOOT_HANDOFF_BARRIER_AFTER_VTOR + 1u)
        return leave(t, 0);
    const boot_handoff_intent_t i = *intent;
    const boot_watchdog_t *w = &t->continuity->watchdog;
    uint32_t pc = i.reset_vector & ~UINT32_C(1);
    if (i.vtor != t->vtor || i.msp != BOOT_INITIAL_MSP ||
        !(i.reset_vector & 1u) || pc < i.vtor || pc - i.vtor >= BOOT_PAYLOAD_MAX ||
        i.control || i.primask || i.basepri || i.faultmask ||
        i.application_epoch != t->epoch || !w->configured || w->blocked ||
        w->owner != BOOT_WATCHDOG_APPLICATION || w->epoch != t->epoch ||
        !core_unchanged(t) || !sources_idle(t) || !banks_idle(t) ||
        !matches(t, STCTRL, 3u, 0u) ||
        !matches(t, VTOR, UINT32_MAX, i.vtor)) return leave(t, 0);
    t->intent = i;
    t->recorded = 1u;
    return leave(t, 1);
}
