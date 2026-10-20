/* Real newlib calloc/free backing, boot-local and bounded below continuity.
 * Single-threaded masked boot only. No BSP syscalls or fabricated allocation. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern unsigned char _heap_start, _heap_static_end, _heap_end;
static uintptr_t heap_break;

void *_sbrk(ptrdiff_t increment)
{
    const uintptr_t start = (uintptr_t)&_heap_start;
    const uintptr_t base = (uintptr_t)&_heap_static_end;
    const uintptr_t end = (uintptr_t)&_heap_end;
    const uintptr_t previous = heap_break ? heap_break : base;
    uintptr_t next;
    if (start > base || base > end || previous < base || previous > end)
        goto exhausted;
    if (increment >= 0) {
        if ((uintmax_t)increment > end - previous) goto exhausted;
        next = previous + (uintptr_t)increment;
    } else {
        const uintmax_t amount = (uintmax_t)(-(increment + 1)) + 1u;
        if (amount > previous - base) goto exhausted;
        next = previous - (uintptr_t)amount;
    }
    heap_break = next;
    return (void *)previous;
exhausted:
    errno = ENOMEM;
    return (void *)-1;
}
