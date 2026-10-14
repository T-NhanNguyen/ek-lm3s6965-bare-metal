.syntax unified
.cpu cortex-m3
.thumb
.section .isr_vector,"a",%progbits
.word 0x20010000
.word Reset_Handler
.rept 57
.word trap
.endr
.section .slot_a,"ax",%progbits
.word 0x20010000
.word entry_a
.rept 9
.word trap
.endr
.word svc_a
.rept 47
.word trap
.endr
.section .slot_b,"ax",%progbits
.word 0x20010000
.word entry_b
.rept 9
.word trap
.endr
.word svc_b
.rept 47
.word trap
.endr
.text
.global Reset_Handler
.thumb_func
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
.global fixture_launch
.thumb_func
fixture_launch:
 /* Disposable inherited stacks; all C work finishes before terminal. */
 mov r5, r0
 mov r6, r1
 mov r7, r2
 ldr r3, =case_control
 ldr r3, [r3]
 ldr r4, =0x2000c000
 msr msp, r4
 isb
 ldr r4, =0x2000d000
 msr psp, r4
 isb
 msr control, r3
 isb
 ldr r4, =case_basepri
 ldr r4, [r4]
 msr basepri, r4
 ldr r4, =case_faultmask
 ldr r4, [r4]
 msr faultmask, r4
 ldr r4, =case_primask
 ldr r4, [r4]
 msr primask, r4
 bl fixture_shutdown
 /* Distinct witnesses at old stack tops, after the last C return. */
 ldr r3, =0x2000bff0
 ldr r4, =0x13579bdf
 str r4, [r3]
 ldr r3, =0x2000cff0
 ldr r4, =0x2468ace0
 str r4, [r3]
 mov r0, r5
 mov r1, r6
 mov r2, r7
 b handoff_terminal
/* Reset entry stubs reside inside their selected image slot. */
.section .slot_a,"ax",%progbits
.thumb_func
entry_a:
 movs r2, #0
 b.w capture
.section .slot_b,"ax",%progbits
.thumb_func
entry_b:
 movs r2, #1
 b.w capture
.text
capture:
 /* Capture before any stack use or C prologue. */
 ldr r0, =captured
 mrs r1, msp
 str r1, [r0, #0]
 mrs r1, control
 str r1, [r0, #4]
 mrs r1, ipsr
 str r1, [r0, #8]
 mrs r1, primask
 str r1, [r0, #12]
 mrs r1, basepri
 str r1, [r0, #16]
 mrs r1, faultmask
 str r1, [r0, #20]
 str r2, [r0, #24]
 str lr, [r0, #28]
 bl fixture_application
 b trap
.thumb_func
svc_a:
 ldr r0, =svc_slot
 movs r1, #1
 str r1, [r0]
 bx lr
.thumb_func
svc_b:
 ldr r0, =svc_slot
 movs r1, #2
 str r1, [r0]
 bx lr
trap:
 b trap
