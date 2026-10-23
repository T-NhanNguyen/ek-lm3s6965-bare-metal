/* Synthetic link root, not a production producer or runtime timing test. */
#include "lm3s6965/boot_startup_gptm.h"
#include "lm3s6965/boot_watchdog_target.h"
#include <stddef.h>
#include <stdint.h>
_Static_assert(sizeof(boot_startup_gptm_t) == 40, "ARM40");
_Static_assert(offsetof(boot_startup_gptm_t, registers) == 0, "registers");
_Static_assert(offsetof(boot_startup_gptm_t, gate_delay) == 12, "delay");
_Static_assert(offsetof(boot_startup_gptm_t, hz) == 16, "hz");
_Static_assert(offsetof(boot_startup_gptm_t, reader_identity) == 36, "identity");
_Noreturn void Reset_Handler(void);
__attribute__((used, section(".isr_vector")))
const uintptr_t wdt_setup_vectors[59] = {UINT32_C(0x20010000), (uintptr_t)Reset_Handler};
_Noreturn void Reset_Handler(void) {
    const boot_watchdog_io_t io = {NULL, boot_startup_gptm_direct_read, boot_startup_gptm_direct_write};
    if (!boot_watchdog_target_clock_ready(&io, boot_startup_gptm_direct_delay))
        for (;;) __asm__ volatile("nop");
    for (;;) __asm__ volatile("nop");
}
