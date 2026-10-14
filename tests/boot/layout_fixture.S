/* GNU Arm link fixture, independent of BSP/runtime libraries. */
.syntax unified
.cpu cortex-m3
.thumb
#ifndef VECTOR_BYTES
#define VECTOR_BYTES 236
#endif
#ifndef INITIAL_MSP
#define INITIAL_MSP 0x20010000
#endif
#ifndef TEXT_BYTES
#define TEXT_BYTES 16
#endif
#ifndef DATA_BYTES
#define DATA_BYTES 16
#endif
#ifndef BSS_BYTES
#define BSS_BYTES 32
#endif
#ifndef HEAP_BYTES
#define HEAP_BYTES 32
#endif
#ifndef CTOR_BYTES
#define CTOR_BYTES 4
#endif
.section .isr_vector,"a",%progbits
.balign 256
.word INITIAL_MSP
.word Reset_Handler
.space VECTOR_BYTES - 8, 0
.section .text,"ax",%progbits
.global Reset_Handler
.thumb_func
Reset_Handler:
 b Reset_Handler
.space TEXT_BYTES, 0x5a
.section .rodata,"a",%progbits
.word 0x12345678
.section .ARM.extab,"a",%progbits
.balign 4
.word 0x11223344
.section .preinit_array,"aw",%preinit_array
.word Reset_Handler
.section .init_array,"aw",%init_array
.word Reset_Handler
.space CTOR_BYTES - 4, 0
.section .fini_array,"aw",%fini_array
.word Reset_Handler
.section .ctors,"aw",%progbits
.word Reset_Handler
.section .dtors,"aw",%progbits
.word Reset_Handler
/* Exercise section alignment; the gap test additionally pins the data LMA. */
.section .data,"aw",%progbits
.balign 256
.word 0xaabbccdd
.space DATA_BYTES - 4, 0x3c
.section .bss,"aw",%nobits
.space BSS_BYTES
.section .heap,"aw",%nobits
.space HEAP_BYTES
#ifdef EXTRA_AW
.section .extra,"aw",%progbits
.space 16, 0x69
#endif
#ifdef EXTRA_RO
.section .extra_ro,"a",%progbits
.space 16, 0x69
#endif
#ifdef EXTRA_EXEC
.section .extra_exec,"ax",%progbits
.space 16, 0x69
#endif
#ifdef EXTRA_NOBITS
.section .extra_nobits,"aw",%nobits
.space 16
#endif
#ifdef EXTRA_NOTE
.section .note.extra,"a",%note
.space 16, 0x69
#endif
#ifdef EXTRA_METADATA
.section .extra_metadata,"",%progbits
.word 0x76543210
.section .debug_extra,"",%progbits
.word 0x10203040
.section .note.extra,"",%note
.word 0
#endif
#ifdef FORBIDDEN
.section .manifest,"a",%progbits
.word 0
#endif
#ifdef FORBIDDEN_METADATA
.section .metadata,"a",%progbits
.word 0
#endif
