#ifndef FTP_TEST_MEMORY_MAP_H
#define FTP_TEST_MEMORY_MAP_H
#include <stdint.h>
#define SYSTICK_BASE_ADDRESS 0u
extern volatile uint32_t test_systick[3];
#define REGISTER32(address) test_systick[(address) / 4u]
#endif
