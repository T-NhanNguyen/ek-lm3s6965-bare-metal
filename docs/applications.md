# Applications and emulation

Standalone and FreeRTOS images use different interrupt and memory owners. QEMU checks software against its model, not physical timing or electrical behavior. Boot-slot applications also need the separate continuity and package contracts.

## Find the contract

- [FreeRTOS integration](#freertos-integration)
- [QEMU procedure and model](#qemu-procedure-and-model)


## FreeRTOS integration

This repository builds a second firmware image that runs FreeRTOS. The kernel is the
official FreeRTOS-Kernel V11.3.1 in `third_party/FreeRTOS-Kernel`, added as a git
submodule. CMake selects the Cortex-M3 GCC port `portable/GCC/ARM_CM3`. The FreeRTOS API
is used directly.

#### Why the kernel is a submodule

| Option | Why it lost |
|---|---|
| Vendored subset of kernel files | The repository must own a hand-written kernel CMakeLists and track upstream file changes by hand. |
| CMake FetchContent | The build needs network access, which breaks the offline build promise. |
| A repo-style wrapper module | The wrapper is only needed for the dead vendor layer. FreeRTOS is an industry standard. |

The submodule keeps the parent repository small (the kernel repository is about 125 MB), pins
the build to one commit, and lets the kernel's own CMake select the port files so the file list
stays correct on update. Fetch it with `git submodule update --init --recursive`.

The image is `build/examples/freertos/lm3s6965_freertos_firmware`. Run it in QEMU:

```bash
qemu-system-arm -M lm3s6965evb -nographic -kernel build/examples/freertos/lm3s6965_freertos_firmware
```

Expected output:

```
LM3S6965 FreeRTOS bring-up
sysclk:50000000 Hz
rcc:   0x01CE1380
tick:  1000 Hz
heap:  32768 bytes
scheduler starting
freertos: tick 0 message 0
freertos: tick 4283 message 1
```

The heartbeat task toggles the user LED. The console task prints one line per second. The
tick count grows between messages, so the SysTick interrupt and the context switch both
work. `freertos/FreeRTOSConfig.h` holds the configuration. The heap is heap_4 with 32 KB
in `.bss`.

#### Integration details

- The kernel CMake expects an INTERFACE target named `freertos_config` holding the include
  directory of `FreeRTOSConfig.h`.
- `FREERTOS_PORT` is `GCC_ARM_CM3` and `FREERTOS_HEAP` is `4`.
- The kernel targets do not set the CPU flags, so `freertos/CMakeLists.txt` applies
  `LM3S6965_ARCH_FLAGS` to `freertos_kernel` and `freertos_kernel_port`.
- Each target has its own map file. The shared link flags no longer carry `-Wl,-Map`.

#### The SVC vector entry name must match the port

The FreeRTOS Cortex-M3 port defines `vPortSVCHandler`, `xPortPendSVHandler`, and
`xPortSysTickHandler`. The vector table in `src/startup.c` names the entries `SVCall_Handler`,
`PendSV_Handler`, and `SysTick_Handler`. `configCHECK_HANDLER_INSTALLATION` defaults to `1`, so
`xPortStartScheduler` reads the vector table and asserts that entry 11 equals `vPortSVCHandler`.
The two names differ, so the assert fires before the first task runs.

Three lines in `FreeRTOSConfig.h` map the port definitions onto the vector table names:

```c
#define vPortSVCHandler     SVCall_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
```

The mapping renames the definitions in `port.c`. The weak alias in `startup.c` then loses to the
strong definition. This repository uses `SVCall_Handler`, the ARM spelling. Most FreeRTOS demos
use `SVC_Handler`, so a copy-pasted config maps to the wrong name and the assert fires.

> **QEMU runs the console slowly.** The firmware paces each ITM write on the stimulus port
> ready bit. QEMU does not model the ITM, so each byte spins the bounded wait loop. A full
> banner takes about 45 seconds. Use a capture window of 45 seconds or more. See
> [Known limitations](limitations.md).


## QEMU procedure and model

QEMU emulates this exact board. You can test the firmware with no silicon attached:

```bash
qemu-system-arm -M lm3s6965evb -nographic -kernel build/examples/baremetal/lm3s6965_firmware
```

Expected output:

```
LM3S6965 bare-metal bring-up
cpu:   cortex-m3 thumb-2, no fpu, no dsp
flash: 262144 bytes
sram:  65536 bytes
xtal:  8000000 Hz
pll:   200000000 Hz / 4
plllck:acquired
sysclk:50000000 Hz
rcc:   0x01CE1380
uart0: 115200 8N1
lm3s6965 bring-up complete
```

The `rcc:` line is a self-check. The firmware reads its own clock register. See
[System clock](peripherals.md).

The firmware never returns. Exit QEMU with `Ctrl-A X`, or kill the process.
`qemu-system-arm` has no timeout flag and macOS has no `timeout(1)`. To script a bounded
run, start the emulator in the background, sleep, then kill it.

#### What the model covers

`hw/arm/stellaris.c` models the Cortex-M3 core, the 256 KB flash, the 64 KB SRAM, the UARTs,
timers, ADC, I²C, SSI, and Ethernet. It is not an approximation built from a similar part:

- It derives the flash and SRAM sizes from the board's documented `DC0` configuration word
  (`flash = ((dc0 & 0xffff) + 1) << 1) * 1024`, `sram = ((dc0 >> 18) + 1) * 1024`), which
  yields exactly 256 KB and 64 KB for this board.
- It cites the LM3S6965 datasheet (rev I) inline as the source of its memory map.

The model is authoritative enough that this project's register headers. Peripheral bases, IRQ
numbers, and the 3-bit NVIC priority width. Were transcribed from it. A hardware-free loop also
catches linker-script and startup-code defects (vector table, `.data` copy, `.bss` zeroing)
before any silicon exists, with sub-second iteration.

#### What a green run proves, and what it does not

QEMU implements `RCC`, `RCC2`, and a read-only `PLLCFG`. `ssys_calculate_system_clock()` computes
the system clock from `SYSDIV`, and `ssys_write` raises `RIS.PLLLRIS` (bit 6) when `PWRDN`
transitions 1 → 0. A QEMU run therefore validates that the clock-configuration writes land and
that the PLL-lock wait terminates.

What QEMU cannot validate is anything downstream of the clock value, or anything outside the
model:

| Gap | Consequence |
|---|---|
| Partial clock model | The UART writes bytes regardless of the baud divisor, so **baud accuracy is untestable**. |
| No oscillator model | Crystal startup time and oscillator tolerance (the IOSC is ±30 %) are absent. A fixed delay is the best you can do. |
| No timing | Interrupt latency, real-time behaviour, and peripheral errata are absent. |
| Peripheral divergence | QEMU models the UART as a Luminary PL011. Silicon documents a "16C550-type" UART. Offsets align, bit semantics may not. |
| More permissive than silicon | QEMU defines `NUM_UART 4` while the LM3S6965 has three, so it will not catch an access to a peripheral that does not exist. |

> **QEMU is a regression harness, not a hardware oracle.**

A green run means the software is self-consistent. It never means the firmware works on
silicon. Every claim about clocks, timing, analog behaviour, or electrical characteristics still
needs real hardware. Passing in QEMU is necessary but not sufficient.

#### GPIO pin muxing is not modeled

A pin is driven by its peripheral only when `GPIOAFSEL` selects the alternate function and
`GPIODEN` enables the digital function (`DEN = 0` tri-states the pin). QEMU's `lm3s6965evb`
model ignores both, so a firmware that enables UART0 but never muxes `PA0`/`PA1` prints a
perfect banner in QEMU and is silent on real silicon. Reproduced here: `UART0 CTL = 0x301` with
`GPIOA AFSEL = 0` and `GPIOA DEN = 0`, while the status registers looked correct.

All GPIO pins are tri-stated after reset (`AFSEL = 0`, `DEN = 0`, `PDR = 0`, `PUR = 0`). The
five JTAG/SWD pins (`PB7`, `PC[3:0]`) are the exception: they default to their debug function,
so Port B `AFSEL` resets to `0x00000080` and Port C to `0x0000000F`. Writes to those five
protected `AFSEL` bits do not commit unless `GPIOLOCK` is unlocked and `GPIOCR` sets the
matching bits. See [Peripherals](peripherals.md).

For every peripheral that leaves the chip, confirm its pins are muxed (`AFSEL`) and
digital-enabled (`DEN`) on silicon. Read the port's registers directly with the debugger:

```
mdw 0x40004420 1   # GPIOA AFSEL -> expect 0x3 for UART0 on PA0/PA1
mdw 0x4000451C 1   # GPIOA DEN   -> expect 0x3
```

A peripheral register read faults (`JTAG-DP STICKY ERROR`, "Failed to read memory") when the
port's clock gate is off. If `RCGC2` shows `0`, the read is a bus fault, not a muxing result.
