#ifndef FTP_ARCH_CC_H
#define FTP_ARCH_CC_H

#include <stdint.h>
#include <errno.h>

/* Cortex-M3/GCC and the deterministic little-endian native test host. */
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(message) do { } while (0)
/* Fail closed without UART/ITM waits, stdio, malloc or protocol ISR work. */
#define LWIP_PLATFORM_ASSERT(message) do { (void)(message); __builtin_trap(); } while (0)
#define LWIP_DECLARE_MEMORY_ALIGNED(name, size) \
    uint8_t name[size] __attribute__((aligned(MEM_ALIGNMENT)))

#endif
