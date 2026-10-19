#include "lm3s6965/boot_watchdog_target.h"

#include <stddef.h>

#define WDT_CLOCK UINT32_C(8)

bool boot_watchdog_target_clock_ready(const boot_watchdog_io_t *io,
    void (*gate_delay)(void *))
{
    uint32_t gate, observed;
    if (io == NULL || io->read_register == NULL ||
        io->write_register == NULL || gate_delay == NULL)
        return false;
    if (io->read_register(io->context, BOOT_WATCHDOG_RCGC0, &gate) != 0)
        return false;
    const uint32_t expected = gate | WDT_CLOCK;
    if (io->write_register(io->context, BOOT_WATCHDOG_RCGC0, expected) != 0 ||
        io->read_register(io->context, BOOT_WATCHDOG_RCGC0, &observed) != 0 ||
        observed != expected)
        return false;
    gate_delay(io->context);
    return true;
}
