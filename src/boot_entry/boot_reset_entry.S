.syntax unified
.cpu cortex-m3
.thumb
.section .text.Boot_Reset_Handler,"ax",%progbits
.global Boot_Reset_Handler
.type Boot_Reset_Handler,%function
.thumb_func
Boot_Reset_Handler:
    cpsid i
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
1:
    cmp r1, r2
    bhs 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
2:
    ldr r1, =_sbss
    ldr r2, =_ebss
    movs r3, #0
3:
    cmp r1, r2
    bhs 4f
    str r3, [r1], #4
    b 3b
4:
    bl boot_decision_entry
    b Boot_Terminal_Handler
.size Boot_Reset_Handler, .-Boot_Reset_Handler

/* Safe even before ordinary C initialization; no stack, stores or feed. */
.section .text.Boot_Terminal_Handler,"ax",%progbits
.global Boot_Terminal_Handler
.type Boot_Terminal_Handler,%function
.thumb_func
Boot_Terminal_Handler:
    cpsid i
5:
    b 5b
.size Boot_Terminal_Handler, .-Boot_Terminal_Handler
