/* Target-only boundary. Compile separately into boot and authenticated slots. */
#include "lm3s6965/boot_startup.h"

#include <stdint.h>

#include "lm3s6965/boot_handoff.h"
#include "lm3s6965/boot_verify.h"

_Noreturn void boot_startup_terminal(uint32_t, uint32_t, uint32_t);
_Noreturn void boot_startup_stop(void)
{
    __asm__ volatile("cpsid i" ::: "memory");
    for (;;) __asm__ volatile("nop");
}
_Noreturn void boot_startup_dispatch(boot_startup_t *c)
{
    if (!boot_startup_begin_dispatch(c)) boot_startup_stop();
    boot_startup_terminal(c->base, c->intent.msp, c->reset_vector);
}
/* This symbol must be linked in the application, not boot. The executable
 * wrapper address belongs to the same authenticated physical image. */
__attribute__((noinline)) int boot_startup_running(void *context,
                                                   boot_verified_image_t *out)
{
    uint32_t vtor = *(volatile uint32_t *)0xe000ed08u;
    return boot_startup_running_at(context,
        (uint32_t)(uintptr_t)&boot_startup_running, vtor, out) ? 0 : -1;
}
