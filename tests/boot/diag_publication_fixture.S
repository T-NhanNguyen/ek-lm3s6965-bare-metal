.syntax unified
.cpu cortex-m3
.thumb
.section .isr_vector,"a",%progbits
.word _estack
.word Reset_Handler
.rept 9
.word trap
.endr
.word svc_handler
.rept 47
.word trap
.endr
.text
.thumb_func
.global Reset_Handler
Reset_Handler:
 ldr r0, =_sbss
 ldr r1, =_ebss
 movs r2, #0
1:
 cmp r0, r1
 bhs 2f
 str r2, [r0], #4
 b 1b
2:
 bl fixture_main
 b trap
.thumb_func
trap:
 b trap
