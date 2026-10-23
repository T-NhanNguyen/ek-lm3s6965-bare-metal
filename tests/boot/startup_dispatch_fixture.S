/* Fixture only: discard boot frame, exercise independent old MSP/PSP + masks. */
.syntax unified
.cpu cortex-m3
.thumb
.text
.global fixture_dispatch
.type fixture_dispatch, %function
.thumb_func
fixture_dispatch:
 tst r1, #2
 beq 1f
 movs r2, #64
 msr basepri, r2
 movs r2, #1
 msr faultmask, r2
1:
 tst r1, #1
 beq 2f
 ldr r2, =0x2000d800
 msr psp, r2
 movs r2, #2
 msr control, r2
 isb
2:
 b boot_startup_dispatch
.size fixture_dispatch, .-fixture_dispatch
