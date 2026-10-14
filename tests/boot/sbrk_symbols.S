/* Fixed synthetic addresses only: never mapped or dereferenced. */
#ifdef __APPLE__
#define SYMBOL(name) _ ## name
#else
#define SYMBOL(name) name
#endif
.globl SYMBOL(_heap_start)
.globl SYMBOL(_heap_static_end)
.globl SYMBOL(_heap_end)
.set SYMBOL(_heap_start), TEST_START
.set SYMBOL(_heap_static_end), TEST_BASE
.set SYMBOL(_heap_end), TEST_END
#ifdef __ELF__
.section .note.GNU-stack,"",%progbits
#endif
