#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Deterministic native equivalents of the FTP linker boundary symbols. */
#ifdef __APPLE__
__asm__(".section __DATA,__data\n.balign 8\n.globl __heap_start\n"
        "__heap_start:\n.space 2048\n.globl __heap_end\n__heap_end:\n.byte 0\n");
#else
__asm__(".data\n.balign 8\n.globl _heap_start\n"
        "_heap_start:\n.space 2048\n.globl _heap_end\n_heap_end:\n.byte 0\n");
#endif
extern unsigned char _heap_start, _heap_end;
void *_sbrk(ptrdiff_t increment);
int main(void)
{
    assert((uintptr_t)&_heap_end - (uintptr_t)&_heap_start == 2048);
    assert(_sbrk(0) == &_heap_start);
    assert(_sbrk(2048) == &_heap_start);
    assert(_sbrk(1) == (void *)-1 && errno == ENOMEM);
    assert(_sbrk(PTRDIFF_MAX) == (void *)-1);
    assert(_sbrk(PTRDIFF_MIN) == (void *)-1);
    assert(_sbrk(0) == &_heap_end);
    assert(_sbrk(-2048) == &_heap_end);
    assert(_sbrk(-1) == (void *)-1 && errno == ENOMEM);
    assert(_sbrk(PTRDIFF_MIN) == (void *)-1);
    assert(_sbrk(0) == &_heap_start);
    assert(_sbrk(1024) == &_heap_start);
    assert(_sbrk(-512) == (void *)((uintptr_t)&_heap_start + 1024));
    assert(_sbrk(0) == (void *)((uintptr_t)&_heap_start + 512));
    puts("FTP C heap: 2 KiB ceiling, shrink/failure preservation and integer boundaries passed");
    return 0;
}
