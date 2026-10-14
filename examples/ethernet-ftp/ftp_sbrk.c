#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern unsigned char _heap_start;
extern unsigned char _heap_static_end;
extern unsigned char _heap_end;
static uintptr_t heap_break;
static int heap_initialized;

/* FTP-only, foreground newlib contract; does not use the live stack pointer.
 * The linker reserves _heap_start .. _heap_end separately from the stack. */
void *_sbrk(ptrdiff_t increment)
{
    const uintptr_t start = (uintptr_t)&_heap_start;
    const uintptr_t base = (uintptr_t)&_heap_static_end;
    const uintptr_t end = (uintptr_t)&_heap_end;
    /* Static .heap inputs occupy _heap_start .. _heap_static_end. */
    if (start > base || base > end)
    {
        errno = ENOMEM;
        return (void *)-1;
    }
    const uintptr_t previous = heap_initialized ? heap_break : base;
    if (previous < base || previous > end)
    {
        errno = ENOMEM;
        return (void *)-1;
    }
    uintptr_t next;
    if (increment >= 0)
    {
        if ((uintmax_t)increment > (uintmax_t)(end - previous))
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        next = previous + (uintptr_t)increment;
    }
    else
    {
        /* Avoid signed overflow even for PTRDIFF_MIN. */
        const uintmax_t amount = (uintmax_t)(-(increment + 1)) + 1u;
        if (amount > (uintmax_t)(previous - base))
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        next = previous - (uintptr_t)amount;
    }
    heap_break = next;
    heap_initialized = 1;
    return (void *)previous;
}
