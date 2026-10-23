#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lm3s6965/boot_watchdog.h"

#define REG(address) (*(volatile uint32_t *)(uintptr_t)(address))

uint32_t captured[8];
extern const unsigned char terminal_return_trap[];
uint32_t case_control, case_basepri, case_faultmask, case_primask;
volatile uint32_t svc_slot;
void fixture_main(void);
void fixture_application(void);
void fixture_shutdown(void);
_Noreturn void fixture_launch(uint32_t base, uint32_t msp, uint32_t vector);

/* Deliberately preserved fixture BSS, not production startup evidence. */
static boot_watchdog_t watchdog;
static boot_watchdog_t transferred;
static uint32_t model_load, model_ctl, model_lock, io_calls;
static uint32_t handoff_io_calls;
static unsigned current_case;
static bool ok;
static bool producer;
static unsigned stage;

static _Noreturn void finish(bool success)
{
    if (!success) {
        char diagnostic[] = "case=00 stage=0\n";
        const char digits[] = "0123456789abcdef";
        diagnostic[5] = digits[(current_case >> 4) & 15u];
        diagnostic[6] = digits[current_case & 15u];
        diagnostic[14] = digits[stage & 15u];
        char values[] = "ctrl=00000000 current=00000000\n";
        uint32_t ctrl = REG(0xe000e010);
        uint32_t value = REG(0xe000e018);
        for (unsigned i = 0; i < 8; ++i) {
            values[5 + i] = digits[(ctrl >> (28u - 4u * i)) & 15u];
            values[22 + i] = digits[(value >> (28u - 4u * i)) & 15u];
        }
        register uint32_t d0 __asm__("r0") = 4;
        register const char *d1 __asm__("r1") = diagnostic;
        __asm__ volatile("bkpt 0xab" : "+r"(d0) : "r"(d1) : "memory");
        d0 = 4;
        d1 = values;
        __asm__ volatile("bkpt 0xab" : "+r"(d0) : "r"(d1) : "memory");
    }
    register uint32_t r0 __asm__("r0") = 4;
    register const char *r1 __asm__("r1") =
        success ? "HANDOFF PASS\n" : "HANDOFF FAIL\n";
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    uint32_t args[2] = {0x20026, success ? 0u : 1u};
    r0 = 0x20;
    register const uint32_t *exit_arg __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(exit_arg) : "memory");
    for (;;) {}
}
/* Synthetic watchdog IO only; never touches physical WDT registers. */
static int model_read(void *context, uint32_t address, uint32_t *value)
{
    (void)context;
    ++io_calls;
    switch (address) {
    case BOOT_WATCHDOG_RCGC0: *value = 8; break;
    case BOOT_WATCHDOG_LOAD: *value = model_load; break;
    case BOOT_WATCHDOG_CTL: *value = model_ctl; break;
    case BOOT_WATCHDOG_LOCK: *value = model_lock; break;
    case BOOT_WATCHDOG_RIS: *value = 0; break;
    default: return -1;
    }
    return 0;
}
static int model_write(void *context, uint32_t address, uint32_t value)
{
    (void)context;
    ++io_calls;
    switch (address) {
    case BOOT_WATCHDOG_LOAD: model_load = value; break;
    case BOOT_WATCHDOG_CTL: model_ctl = value; break;
    case BOOT_WATCHDOG_LOCK:
        model_lock = value == 0x1acce551 ? 0u : 1u;
        break;
    case BOOT_WATCHDOG_ICR: break;
    default: return -1;
    }
    return 0;
}
static _Noreturn void next_case(void)
{
    case_control = (current_case & 1u) ? 2u : 0u;
    case_primask = (current_case >> 1) & 1u;
    case_faultmask = (current_case >> 2) & 1u;
    case_basepri = ((current_case >> 3) & 7u) * 32u;
    uint32_t base = (current_case & 64u) ? 0x24000u : 0x8800u;
    uint32_t vector = *(const uint32_t *)(uintptr_t)(base + 4u);
    /* Table plus reset stub must remain within this synthetic image. */
    ok &= (vector & 1u) != 0 && (vector & ~1u) >= base + 236u;
    ok &= (vector & ~1u) < base + 256u;
    if (!ok) finish(false);
    handoff_io_calls = io_calls;
    fixture_launch(base, 0x20010000, vector);
}
void fixture_main(void)
{
    /* No initialized data copy in this synthetic startup. */
    ok = true;
    boot_watchdog_io_t io = {0, model_read, model_write};
    ok &= boot_watchdog_init(&watchdog, &io) == BOOT_WATCHDOG_OK;
    ok &= boot_watchdog_configure(&watchdog, 1, 5) == BOOT_WATCHDOG_OK;
    uint32_t before = io_calls;
    ok &= boot_watchdog_transfer(&watchdog, 1) == BOOT_WATCHDOG_OK;
    ok &= io_calls == before && watchdog.epoch == 2;
    if (!ok) finish(false);
    transferred = watchdog;
    next_case();
}
void fixture_shutdown(void)
{
    stage = 1;
    uint32_t control, primask, basepri, faultmask;
    __asm__ volatile("mrs %0, control" : "=r"(control));
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("mrs %0, basepri" : "=r"(basepri));
    __asm__ volatile("mrs %0, faultmask" : "=r"(faultmask));
    ok &= control == case_control && primask == case_primask;
    ok &= basepri == case_basepri && faultmask == case_faultmask;
    if (!ok) finish(false);
    __asm__ volatile("cpsid i" ::: "memory");
    stage = 2;
    /* Real QEMU core registers, seeded only under exclusion. */
    REG(0xe000e014) = 0xffffff;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(0xe000e100) = 0xffffffff;
    REG(0xe000e104) = 0xfff;
    REG(0xe000e010) = 0;
    REG(0xe000e018) = 0;
    __asm__ volatile("dsb" ::: "memory");
    ok &= REG(0xe000e100) != 0 && REG(0xe000e104) != 0;
    if (!ok) finish(false);
    stage = 3;
    /* Injected software producer, NOT an emulated Ethernet/DMA device. */
    producer = true;
    if (producer) {
        REG(0xe000e200) = 0xffffffff;
        REG(0xe000e204) = 0xfff;
        REG(0xe000ed04) = 0x14000000;
    }
    ok &= REG(0xe000e200) != 0 && REG(0xe000e204) != 0;
    ok &= (REG(0xe000ed04) & 0x14000000) == 0x14000000;
    if (!ok) finish(false);
    stage = 4;
    producer = false;
    /* PRIMASK excludes delivery until source shutdown and final disable/clear. */
    REG(0xe000e180) = 0xffffffff;
    /* Data sheet defines bank 1 bits 11:0, including unused IRQ 43. */
    REG(0xe000e184) = 0xfff;
    /* Final clear follows producer shutdown, not merely delivery disable. */
    REG(0xe000ed04) = 0x0a000000;
    REG(0xe000e280) = 0xffffffff;
    REG(0xe000e284) = 0xfff;
    __asm__ volatile("dsb" ::: "memory");
    ok &= !producer && REG(0xe000e200) == 0 && REG(0xe000e204) == 0;
    ok &= (REG(0xe000ed04) & 0x14000000) == 0;
}
void fixture_application(void)
{
    stage = 5;
    ok &= captured[0] == 0x20010000;
    ok &= captured[6] == ((current_case >> 6) & 1u);
    ok &= captured[7] == ((uint32_t)(uintptr_t)terminal_return_trap | 1u);
    ok &= io_calls == handoff_io_calls;
    for (unsigned i = 1; i < 6; ++i) ok &= captured[i] == 0;
    if (!ok) finish(false);
    stage = 7;
    ok &= REG(0x2000bff0) == 0x13579bdf;
    ok &= REG(0x2000cff0) == 0x2468ace0;
    ok &= REG(0xe000ed08) ==
        ((current_case & 64u) ? 0x24000u : 0x8800u);
    if (!ok) finish(false);
    stage = 8;
    /* QEMU keeps CLKSOURCE=1; only stop/interrupt/count effects matter. */
    ok &= (REG(0xe000e010) & 0x10003u) == 0;
    ok &= REG(0xe000e018) == 0;
    if (!ok) finish(false);
    stage = 9;
    ok &= REG(0xe000e100) == 0 && REG(0xe000e104) == 0;
    ok &= REG(0xe000e200) == 0 && REG(0xe000e204) == 0;
    ok &= (REG(0xe000ed04) & 0x14000000) == 0;
    if (!ok) finish(false);
    stage = 6;
    svc_slot = 0;
    __asm__ volatile("svc 0" ::: "memory");
    ok &= svc_slot == captured[6] + 1u;
    ok &= memcmp(&watchdog, &transferred, sizeof watchdog) == 0;
    ok &= watchdog.owner == BOOT_WATCHDOG_APPLICATION && watchdog.epoch == 2;
    uint32_t before = io_calls;
    ok &= boot_watchdog_transfer(&watchdog, 2) == BOOT_WATCHDOG_STATE;
    ok &= io_calls == before;
    if (!ok) finish(false);
    if (++current_case == 128) finish(true);
    next_case();
}
