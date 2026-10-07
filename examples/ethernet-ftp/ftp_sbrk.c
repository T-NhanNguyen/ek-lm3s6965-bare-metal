#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern unsigned char _heap_start;
extern unsigned char _heap_end;
static uintptr_t heap_break;

/* FTP-only, foreground newlib contract; does not use the live stack pointer.
 * The linker reserves the complete 2 KiB heap separately from the stack. */
void *_sbrk(ptrdiff_t increment)
{
    const uintptr_t start = (uintptr_t)&_heap_start;
    const uintptr_t end = (uintptr_t)&_heap_end;
    if (heap_break == 0u)
    {
        heap_break = start;
    }
    const uintptr_t previous = heap_break;
    if (increment >= 0)
    {
        if ((uintptr_t)increment > end - heap_break)
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        heap_break += (uintptr_t)increment;
    }
    else
    {
        /* Avoid signed overflow even for PTRDIFF_MIN. */
        const uintptr_t amount = (uintptr_t)(-(increment + 1)) + 1u;
        if (amount > heap_break - start)
        {
            errno = ENOMEM;
            return (void *)-1;
        }
        heap_break -= amount;
    }
    return (void *)previous;
}
