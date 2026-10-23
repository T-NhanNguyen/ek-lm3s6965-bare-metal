#ifndef CALLER_ARCH_CC_H
#define CALLER_ARCH_CC_H
#include <stdint.h>
#include <errno.h>
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(message) do { } while (0)
#define LWIP_PLATFORM_ASSERT(message) do { (void)(message); __builtin_trap(); } while (0)
#define LWIP_DECLARE_MEMORY_ALIGNED(name, size) uint8_t name[size] __attribute__((aligned(MEM_ALIGNMENT)))
#endif
