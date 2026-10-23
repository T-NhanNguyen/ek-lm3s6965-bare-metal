#include "net_clock_port.h"

#include <stdbool.h>
#include <stdint.h>

bool net_clock_port_masked_thread(void)
{
#if defined(__arm__) || defined(__thumb__)
    uint32_t mask, ipsr, basepri, faultmask, control;
    __asm__ volatile("mrs %0, primask\n mrs %1, ipsr\n mrs %2, basepri\n"
                     "mrs %3, faultmask\n mrs %4, control"
        : "=r"(mask), "=r"(ipsr), "=r"(basepri), "=r"(faultmask), "=r"(control));
    return mask == 1u && ipsr == 0u && basepri == 0u &&
        faultmask == 0u && control == 0u;
#else
    return false; /* No implicit native simulation of privileged core state. */
#endif
}

void net_clock_port_barrier(void)
{
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("dsb\n isb" ::: "memory");
#endif
}

void net_clock_port_unmask(void)
{
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile("dsb\n cpsie i\n isb" ::: "memory");
#endif
    /* Unsupported cores cannot pass masked_thread, so initialization never
     * reaches this operation when linked with this production port. */
}
