## FreeRTOS target

This repository builds a second firmware image that runs FreeRTOS. The kernel is the
official FreeRTOS-Kernel V11.3.1 in `third_party/FreeRTOS-Kernel`, added as a git
submodule. CMake selects the Cortex-M3 GCC port `portable/GCC/ARM_CM3`. The FreeRTOS API
is used directly.

### Why the kernel is a submodule

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

### Integration details

- The kernel CMake expects an INTERFACE target named `freertos_config` holding the include
  directory of `FreeRTOSConfig.h`.
- `FREERTOS_PORT` is `GCC_ARM_CM3` and `FREERTOS_HEAP` is `4`.
- The kernel targets do not set the CPU flags, so `freertos/CMakeLists.txt` applies
  `LM3S6965_ARCH_FLAGS` to `freertos_kernel` and `freertos_kernel_port`.
- Each target has its own map file. The shared link flags no longer carry `-Wl,-Map`.

### The SVC vector entry name must match the port

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

The mapping renames the definitions in `port.c`; the weak alias in `startup.c` then loses to the
strong definition. This repository uses `SVCall_Handler`, the ARM spelling. Most FreeRTOS demos
use `SVC_Handler`, so a copy-pasted config maps to the wrong name and the assert fires.

> **QEMU runs the console slowly.** The firmware paces each ITM write on the stimulus port
> ready bit. QEMU does not model the ITM, so each byte spins the bounded wait loop. A full
> banner takes about 45 seconds. Use a capture window of 45 seconds or more. See
> [Known limitations](../README.md#known-limitations).
