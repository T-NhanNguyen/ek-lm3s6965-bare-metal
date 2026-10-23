/* Signed slot prefix. No old stack or ordinary writable state. Capture before
 * clobber, exclude only AFTER observing incoming masks; new MSP is now usable. */
.syntax unified
.cpu cortex-m3
.thumb
.text
.global Application_Reset_Handler
.type Application_Reset_Handler, %function
.thumb_func
Application_Reset_Handler:
 ldr r0, =boot_startup_continuity + 448
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
 str lr, [r0, #24]
 ldr r1, =0xe000ed08
 ldr r1, [r1]
 str r1, [r0, #28]
.global Application_Entry_Witness
Application_Entry_Witness:
 adr r1, Application_Entry_Witness
 str r1, [r0, #32]
 ldr r1, =Application_Reset_Handler
 str r1, [r0, #36]
 cpsid i
 ldr r0, =boot_startup_continuity
 ldr r1, =Application_Reset_Handler
 ldr r2, =Application_Entry_Witness
 bl boot_startup_early
 cmp r0, #0
 beq 1f
 b Reset_Handler
1:
 b boot_startup_stop
.size Application_Reset_Handler, .-Application_Reset_Handler
