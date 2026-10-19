#ifndef FTP_TEST_MEMORY_MAP_H
#define FTP_TEST_MEMORY_MAP_H
#include <stdint.h>
#include <stdlib.h>
#define SYSTICK_BASE_ADDRESS 0u
extern volatile uint32_t test_systick[3];
/* Bounded standalone decoder: the only extra core register used is ICSR.
 * No foreground interrupt-admission behavior is claimed by this shim. */
static inline volatile uint32_t *ftp_test_register(uint32_t address)
{
    static volatile uint32_t icsr;
    if (address <= 8u && address % 4u == 0u) return &test_systick[address / 4u];
    if (address == UINT32_C(0xe000ed04)) return &icsr;
    abort();
}
#define REGISTER32(address) (*ftp_test_register(address))
#endif
