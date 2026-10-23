#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Synthetic nonproduction 2 KiB capacity fixture for allocator boundaries.
 * Production FTP reserves 8192 total bytes; real linker replays test that budget. */
#ifdef __APPLE__
__asm__(".section __DATA,__data\n.balign 8\n.globl __heap_start\n"
        "__heap_start:\n.space 256\n.globl __heap_static_end\n"
        "__heap_static_end:\n.space 1792\n.globl __heap_end\n__heap_end:\n.byte 0\n");
#else
__asm__(".data\n.balign 8\n.globl _heap_start\n"
        "_heap_start:\n.space 256\n.globl _heap_static_end\n"
        "_heap_static_end:\n.space 1792\n.globl _heap_end\n_heap_end:\n.byte 0\n");
#endif
extern unsigned char _heap_start, _heap_static_end, _heap_end;
void *_sbrk(ptrdiff_t increment);
int main(void)
{
    assert((uintptr_t)&_heap_end - (uintptr_t)&_heap_start == 2048);
    assert((uintptr_t)&_heap_static_end - (uintptr_t)&_heap_start == 256);
    assert(_sbrk(0) == &_heap_static_end);
    assert(_sbrk(1792) == &_heap_static_end);
    assert(_sbrk(1) == (void *)-1 && errno == ENOMEM);
    assert(_sbrk(PTRDIFF_MAX) == (void *)-1);
    assert(_sbrk(PTRDIFF_MIN) == (void *)-1);
    assert(_sbrk(0) == &_heap_end);
    assert(_sbrk(-1792) == &_heap_end);
    assert(_sbrk(-1) == (void *)-1 && errno == ENOMEM);
    assert(_sbrk(PTRDIFF_MIN) == (void *)-1);
    assert(_sbrk(0) == &_heap_static_end);
    assert(_sbrk(1024) == &_heap_static_end);
    assert(_sbrk(-512) == (void *)((uintptr_t)&_heap_static_end + 1024));
    assert(_sbrk(0) == (void *)((uintptr_t)&_heap_static_end + 512));
    puts("Synthetic nonproduction FTP allocator fixture: 2 KiB ceiling, shrink/failure preservation and integer boundaries passed");
    return 0;
}
