/* Fixture-only terminal ABI: r0=VTOR, r1=MSP, r2=original Thumb vector.
 * Privileged Thread, PRIMASK=1, sources stopped, final pending clears done,
 * watchdog already transferred. No validation, C frame or return path.
 */
.syntax unified
.cpu cortex-m3
.thumb
.text
.global handoff_terminal
.type handoff_terminal, %function
.thumb_func
handoff_terminal:
 dsb
 ldr r3, =0xe000ed08
 str r0, [r3]
 dsb
 isb
 adr r3, terminal_return_trap
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
.global terminal_return_trap
.thumb_func
terminal_return_trap:
 b terminal_return_trap
.size handoff_terminal, .-handoff_terminal
