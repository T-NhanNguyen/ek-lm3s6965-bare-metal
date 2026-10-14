/* Official installed newlib syscall declarations, not a replacement API. */
#define _LIBC 1
#include <sys/unistd.h>
#include <reent.h>
#include <stddef.h>
_Static_assert(_Generic(&_sbrk, void *(*)(ptrdiff_t): 1, default: 0),
               "newlib _sbrk ABI changed");
_Static_assert(_Generic(&_sbrk_r, void *(*)(struct _reent *, ptrdiff_t): 1, default: 0),
               "newlib _sbrk_r ABI changed");
_Static_assert(_Generic((ptrdiff_t)0, int: 1, default: 0),
               "this Cortex-M3 toolchain's ptrdiff_t is int");
