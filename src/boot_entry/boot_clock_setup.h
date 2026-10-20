#ifndef BOOT_CLOCK_SETUP_H
#define BOOT_CLOCK_SETUP_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t (*read)(uint32_t address);
    void (*write)(uint32_t address, uint32_t value);
    void (*delay)(uint32_t iterations);
} boot_clock_io_t;
/* Genuine reset only, masked, exclusive clock ownership, EK 8 MHz crystal.
 * False is terminal; never infer a fallback frequency from the partial state. */
bool boot_clock_setup(const boot_clock_io_t *io);
#endif
