#ifndef CLOCK_TEST_MEMORY_MAP_H
#define CLOCK_TEST_MEMORY_MAP_H
#include <stdint.h>
#define SYSTICK_BASE_ADDRESS UINT32_C(0xe000e010)
volatile uint32_t *clock_test_register(uint32_t);
#define REGISTER32(address) (*clock_test_register(address))
#endif
