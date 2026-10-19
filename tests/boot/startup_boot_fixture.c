/* Real verifier/handoff, modeled WDT/time/peripheral source shutdown. */
#include "startup_fixture.h"
#include "startup_image.h"
#include <string.h>

static int read_register(void *context, uint32_t address, uint32_t *value)
{
    #if defined(FOREGROUND_CONTINUITY_TEST)
    startup_model_t *m = startup_model(startup_capsule(context));
#else
    startup_model_t *m = context;
#endif
    ++m->reads;
    switch (address) {
    case BOOT_WATCHDOG_RCGC0: *value = 8; break;
    case BOOT_WATCHDOG_CTL: *value = m->ctl; break;
    case BOOT_WATCHDOG_LOAD: *value = m->load; break;
    case BOOT_WATCHDOG_LOCK: *value = m->lock; break;
    case BOOT_WATCHDOG_RIS: *value = 0; break;
    default: return 1;
    }
    return 0;
}
static int write_register(void *context, uint32_t address, uint32_t value)
{
    #if defined(FOREGROUND_CONTINUITY_TEST)
    startup_model_t *m = startup_model(startup_capsule(context));
#else
    startup_model_t *m = context;
#endif
    ++m->writes;
    switch (address) {
    case BOOT_WATCHDOG_CTL: m->ctl = value; break;
    case BOOT_WATCHDOG_LOAD: m->load = value; break;
    case BOOT_WATCHDOG_LOCK: m->lock = value == 0x1acce551u ? 0 : 1; break;
    case BOOT_WATCHDOG_ICR: ++m->feeds; break;
    default: return 1;
    }
    return 0;
}
#if !defined(FOREGROUND_CONTINUITY_TEST)
static int read_clock(void *context, boot_startup_sample_t *sample)
{
    startup_model_t *m = context;
    /* Explicit modeled observed register reads, NOT physical oscillator proof.
     * Fault scenario changes modeled RCC2 at the third (pre-C entry) sample. */
    ++m->clock_reads;
    if (m->fail_clock_on_read && m->clock_reads == m->fail_clock_on_read) m->rcc2 = 1;
    *sample = (boot_startup_sample_t){m->us, m->rcc, m->rcc2, 100, 1};
    return 0;
}
#endif
#if defined(FOREGROUND_CONTINUITY_TEST)
/* Boot FLASH leaf IO; registers.context points only into retained reserve.
 * TAR advances exactly 100 modeled us per sample, not elapsed physical time. */
static int gptm_register_read(void *context, uint32_t address, uint32_t *value)
{
    foreground_model_t *f = context;
    startup_model_t *m = &f->startup;
    ++f->timer_reads;
    if (address == 0x400fe104u) { *value = f->gate; return 0; }
    if (address == 0x400fe060u) { *value = m->rcc; return 0; }
    if (address == 0x400fe070u) { *value = m->rcc2; return 0; }
    const uint32_t b = 0x40030000u + f->timer * 0x1000u;
    switch (address - b) {
    case 0: *value = f->cfg; break;
    case 4: *value = f->tamr; break;
    case 12: *value = f->timer_ctl; break;
    case 24: *value = f->imr; break;
    case 28: *value = f->ris; break;
    case 40: *value = f->tailr; break;
    case 72: {
        boot_startup_t *c = (boot_startup_t *)((uint8_t *)context - offsetof(boot_startup_t, reserve));
        ++f->tar_reads; ++m->clock_reads;
        m->us += 100;
        f->final_us = (uint32_t)m->us;
        *value = UINT32_MAX - (uint32_t)m->us * 50u;
        if (m->fail_clock_on_read && m->clock_reads == m->fail_clock_on_read) m->rcc2 = 1;
        if (c->state == BOOT_STARTUP_C_READY) {
            uint32_t mask; __asm__ volatile("mrs %0, primask" : "=r"(mask));
            if ((*(volatile uint32_t *)0xe000e010u & 7u) != 7u ||
                *(volatile uint32_t *)0xe000e014u != 49999u || mask != 1u) return -1;
            ++f->saw_start;
        }
        break;
    }
    default: return -1;
    }
    return 0;
}
static int gptm_register_write(void *context, uint32_t address, uint32_t value)
{
    foreground_model_t *f = context;
    ++f->timer_writes;
    if (address == 0x400fe104u) { f->gate = value; return 0; }
    const uint32_t b = 0x40030000u + f->timer * 0x1000u;
    switch (address - b) {
    case 0: f->cfg = value; break;
    case 4: f->tamr = value; break;
    case 12: f->timer_ctl = value; break;
    case 24: f->imr = value; break;
    case 36: f->ris &= ~value; break;
    case 40: f->tailr = value; break;
    default: return -1;
    }
    return 0;
}
static void gptm_gate_delay(void *context)
{ (void)context; __asm__ volatile("nop\n nop\n nop" ::: "memory"); }
#endif
static size_t stored(void *context, uint32_t address, uint8_t *out, size_t n)
{
    (void)context;
    memcpy(out, (const void *)(uintptr_t)address, n);
    return n;
}
static int metadata_read(void *context, uint32_t address, uint8_t *out, size_t n)
{
    (void)context;
    memcpy(out, (const void *)(uintptr_t)address, n);
    return 0;
}
static int no_erase(void *context, uint32_t address)
{
    (void)context; (void)address;
    CHECK(0); return -1;
}
static int no_program(void *context, uint32_t address, uint32_t value)
{
    (void)context; (void)address; (void)value;
    CHECK(0); return -1;
}
static int progress(void *context, boot_verify_phase_t phase, uint32_t done, uint32_t total)
{
    boot_watchdog_t *w = context;
    CHECK(boot_watchdog_boot_report(w, w->epoch, 1, phase, done, total) == BOOT_WATCHDOG_OK);
    if (phase != BOOT_VERIFY_MANIFEST_BEGIN)
        CHECK(boot_watchdog_service(w, BOOT_WATCHDOG_BOOT, w->epoch) == BOOT_WATCHDOG_OK);
    return 1;
}
static int operation(void *context, boot_handoff_operation_t op, uint32_t value)
{
    (void)context;
    switch (op) {
    case BOOT_HANDOFF_EXCLUDE: __asm__ volatile("cpsid i" ::: "memory"); break;
    case BOOT_HANDOFF_STOP_SYSTICK: *(volatile uint32_t *)0xe000e010u = 0; break;
    case BOOT_HANDOFF_DISABLE_BANK0: *(volatile uint32_t *)0xe000e180u = UINT32_MAX; break;
    case BOOT_HANDOFF_DISABLE_BANK1: *(volatile uint32_t *)0xe000e184u = 0x7ff; break;
    case BOOT_HANDOFF_CLEAR_CORE_PENDING: *(volatile uint32_t *)0xe000ed04u = 0x0a000000; break;
    case BOOT_HANDOFF_CLEAR_BANK0_PENDING: *(volatile uint32_t *)0xe000e280u = UINT32_MAX; break;
    case BOOT_HANDOFF_CLEAR_BANK1_PENDING: *(volatile uint32_t *)0xe000e284u = 0x7ff; break;
    case BOOT_HANDOFF_SET_VTOR: *(volatile uint32_t *)0xe000ed08u = value; break;
    case BOOT_HANDOFF_BARRIER_BEFORE_VTOR:
    case BOOT_HANDOFF_BARRIER_AFTER_VTOR: __asm__ volatile("dsb\n isb" ::: "memory"); break;
    case BOOT_HANDOFF_QUIESCE_SOURCES: break; /* modeled absent peripherals */
    }
    return 0;
}
static int terminal(void *context, const boot_handoff_intent_t *intent)
{
    (void)context;
    CHECK(intent->application_epoch == boot_startup_continuity.watchdog.epoch);
    /* Must RETURN. Physical entry before guards release is prohibited. */
    CHECK(boot_startup_continuity.state == BOOT_STARTUP_EMPTY);
    return 0;
}
_Noreturn void fixture_dispatch(boot_startup_t *, unsigned);
int main(void);
int main(void)
{
    boot_startup_t *c = &boot_startup_continuity;
    boot_watchdog_io_t io = {c->context, read_register, write_register};
    #if defined(FOREGROUND_CONTINUITY_TEST)
    boot_startup_clock_t clock = {boot_startup_gptm_read, c->context, 0, 0,
        UINT32_MAX, UINT32_MAX, STARTUP_HZ, BOOT_STARTUP_GPTM_SOURCE};
#else
    boot_startup_clock_t clock = {read_clock, c->context, 0, 0, UINT32_MAX, UINT32_MAX, 100, 1};
#endif
    boot_verified_image_t image;
    boot_handoff_t h;
    boot_handoff_io_t handoff_io = {NULL, operation, terminal};
    boot_handoff_request_t request = {0};
    boot_handoff_intent_t intent;
    boot_store_snapshot_t authority;
    boot_store_adapter_t store = {NULL, metadata_read, no_erase, no_program};
    startup_model_t *m = startup_model(c);
    char command[32] = {0};
    uintptr_t args[2] = {(uintptr_t)command, sizeof(command)};
    semi(0x15, args);
    unsigned test_case = (unsigned)(command[0] - '0');
    CHECK(test_case < 5);
    CHECK(boot_startup_reset(c, (volatile uint32_t *)0x2000dfc0u, 0, &io, &clock));
    #if defined(FOREGROUND_CONTINUITY_TEST)
    foreground_model_t *f = foreground_model(c);
    f->timer = SLOT_INDEX + 2u; /* Explicit timer2 A / timer3 B. */
    boot_watchdog_io_t registers = {f, gptm_register_read, gptm_register_write};
    CHECK(boot_startup_gptm_claim((void *)c->context, f->timer, STARTUP_HZ,
        &registers, gptm_gate_delay));
#endif
    CHECK(boot_watchdog_configure(&c->watchdog, STARTUP_HZ, STARTUP_LOAD) == BOOT_WATCHDOG_OK);
    CHECK(boot_verify_stored(manifest_wire, sizeof(manifest_wire), SLOT_INDEX,
        stored, NULL, progress, &c->watchdog, &image) == BOOT_VERIFY_OK);
    /* Actual CRC/commit-last authority load from immutable modeled flash. */
    CHECK(boot_store_load(&store, &authority) == BOOT_STORE_OK);
    CHECK(authority.record.current_slot == SLOT_INDEX);
    CHECK(boot_metadata_image_matches(&authority.record, &image.manifest) == BOOT_OK);
    request.image = image; request.selected_slot = SLOT_INDEX;
    request.watchdog_epoch = c->watchdog.epoch;
    CHECK(boot_handoff_init(&h, &handoff_io) == BOOT_HANDOFF_OK);
    CHECK(boot_handoff_run(&h, &c->watchdog, &request, &intent) == BOOT_HANDOFF_OK);
    if (test_case == 4) m->fail_clock_on_read = 3;
    CHECK(boot_startup_prepare_handoff(c, &h, BOOT_HANDOFF_OK, &image, &intent));
    /* Independent IO observations on the original retained controller/context. */
    m->dispatch_writes = m->writes;
    m->dispatch_feeds = m->feeds;
    m->digest = diagnostic_digest(c->publication.words);
    m->us = 1000;
    /* After all verifier/libc/boot BSS consumers return: simulate actual app
     * overwrite, not a shared-BSS fixture or second controller copy. */
    for (volatile uint32_t *p = (volatile uint32_t *)APP_DATA_START;
         (uintptr_t)p < APP_BSS_END; ++p) *p = 0xa5a5a5a5;
    fixture_dispatch(c, test_case);
}
