/* r0=VTOR r1=MSP r2=Thumb reset; exclusive quiescent Thread, WDT transferred. */
.syntax unified
.cpu cortex-m3
.thumb
.text
.global boot_startup_terminal
.type boot_startup_terminal, %function
.thumb_func
boot_startup_terminal:
 dsb
 ldr r3, =0xe000ed08
 str r0, [r3]
 dsb
 isb
 adr r3, boot_startup_return_trap
 orr r3, r3, #1
 mov lr, r3
 movs r3, #0
 msr msp, r1
 isb
 msr control, r3
 isb
 msr basepri, r3
 msr faultmask, r3
 msr primask, r3
 bx r2
.size boot_startup_terminal, .-boot_startup_terminal
.global boot_startup_return_trap
.type boot_startup_return_trap, %function
.thumb_func
boot_startup_return_trap:
 b boot_startup_return_trap
.size boot_startup_return_trap, .-boot_startup_return_trap
