# LM3S6965 Bare-Metal Cross-Compilation Toolchain

Cross-compilation environment for the **TI / Luminary Micro LM3S6965** — an ARM
Cortex-M3 microcontroller (256 KB flash, 64 KB SRAM, 50 MHz) — on **macOS
Apple Silicon**, with no vendor IDE and no Docker.

The chip is from 2007 and TI's own tooling (Code Composer Studio, StellarisWare,
LM Flash Programmer) is Windows-only and abandoned. This repo replaces that stack
with portable open-source tooling that runs natively on arm64 macOS.

## Why this works at all

ARM defines the **core**; the vendor defines the **peripherals**. `arm-none-eabi-gcc`
is a generic toolchain that targets the core, so it does not care how old the silicon
is. Only the vendor layer (SDK, IDE, flash utility) is dead — and that is the part
this repo rebuilds from the datasheet.

---

## Required packages

All versions below were verified on macOS 26.6 (arm64).

| Package | Verified version | Purpose | Install |
|---|---|---|---|
| **Arm GNU Toolchain** | **15.3.rel1** | Cross compiler, newlib C library, binutils, GDB | see [below](#installing-the-arm-toolchain) |
| CMake | 4.4.2 | Build orchestration | `brew install cmake` |
| QEMU | 11.1.1 | Emulates the `lm3s6965evb` board — no hardware needed | `brew install qemu` |
| OpenOCD | 0.12.0 | Flash programming + GDB server over the ICDI probe | `brew install openocd` |
| libusb | 1.0.30 | USB access for the FTDI-based debug probe | `brew install libusb` |
| libftdi | 1.5_2 | FTDI FT2232 driver used by the Stellaris ICDI | `brew install libftdi` |
| libusb-compat | 0.1.9 | Legacy libusb 0.1 API, required by some OpenOCD paths | `brew install libusb-compat` |
| picocom | 2024-07 | Serial console for the UART0 output | `brew install picocom` |
| arm-none-eabi-gdb | 17.2 | Standalone debugger (also bundled in the Arm toolchain) | `brew install arm-none-eabi-gdb` |

One-shot install of everything except the Arm toolchain:

```bash
brew install cmake qemu openocd libusb libftdi libusb-compat picocom arm-none-eabi-gdb
```

> Homebrew now ships OpenOCD under the formula name **`open-ocd`**. The
> `brew install openocd` alias still resolves, and the binary is still `openocd`.

### Installing the Arm toolchain

> ### ⚠️ Do NOT use the Homebrew formula `arm-none-eabi-gcc`
>
> The Homebrew **formula** `arm-none-eabi-gcc` builds GCC and binutils but ships
> **no C library at all** — the sysroot is empty and there is no newlib. Its own
> `<stdint.h>` begins with `#include_next <stdint.h>` and expects a libc-provided
> header that does not exist, so any build dies with:
>
> ```
> fatal error: stdint.h: No such file or directory
>     11 | # include_next <stdint.h>
> ```
>
> There is **no** `arm-none-eabi-newlib` Homebrew formula. Only the **official
> Arm GNU Toolchain** (which bundles newlib) is usable.
>
> `cmake/toolchain-lm3s6965.cmake` searches for the official toolchain first and
> aborts with an actionable message if it finds a libc-less one instead.

**Option A — official cask** (one-time `sudo` password required):

```bash
brew install --cask gcc-arm-embedded
```

**Option B — tarball** (no `sudo`; this is the path verified in this repo):

```bash
TC_DIR="$HOME/.toolchains"
ARCHIVE="arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi.tar.xz"
URL="https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/$ARCHIVE"

mkdir -p "$TC_DIR" && cd "$TC_DIR"
curl -sL --fail -o "$ARCHIVE" "$URL"
tar -xf "$ARCHIVE" && rm -f "$ARCHIVE"
```

The toolchain installs to `$HOME/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/`
(~1.0 GB extracted). CMake finds it automatically; to use a different location pass
`-DLM3S6965_TOOLCHAIN_BIN_HINTS=/path/to/toolchain/bin`.

---

## Build

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
cmake --build build
```

Configure prints the resolved toolchain and libc so a misconfigured environment is
obvious immediately:

```
-- LM3S6965 toolchain: .../arm-none-eabi-gcc
-- LM3S6965 libc     : .../thumb/v7-m/nofp/libc.a
```

Artifacts land in `build/`:

| File | Purpose |
|---|---|
| `lm3s6965_firmware` | ELF, used by GDB and QEMU |
| `lm3s6965_firmware.bin` | Raw binary for flashing |
| `lm3s6965_firmware.hex` | Intel HEX for flashing |
| `lm3s6965_firmware.map` | Linker map |

## Verify the build

```bash
TCBIN="$HOME/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin"
$TCBIN/arm-none-eabi-objdump -f build/lm3s6965_firmware   # architecture: armv7
$TCBIN/arm-none-eabi-readelf -A build/lm3s6965_firmware   # Tag_THUMB_ISA_use: Thumb-2
$TCBIN/arm-none-eabi-size   build/lm3s6965_firmware
```

Expected `readelf -A` attributes: `Tag_CPU_name: "7-M"`, `Tag_CPU_arch: v7`,
`Tag_CPU_arch_profile: Microcontroller`, `Tag_THUMB_ISA_use: Thumb-2`.

## Run without hardware (QEMU)

QEMU emulates this exact board, so firmware can be tested with no silicon attached:

```bash
qemu-system-arm -M lm3s6965evb -nographic -kernel build/lm3s6965_firmware
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

The `rcc:` line is a self-check the firmware performs on its own clock register — see
[System clock](#system-clock).

The firmware never returns, so exit QEMU with `Ctrl-A X` (or kill the process).
`qemu-system-arm` has no timeout flag and macOS has no `timeout(1)`; to script a
bounded run, background it, `sleep`, then `kill`.

## System clock

The LM3S6965 is a Fury-class part: its PLL produces a fixed **200 MHz**, which `SYSDIV`
divides down. The EK-LM3S6965 has an **8 MHz** crystal, so 200 / 4 = **50 MHz**, meaning
`SYSDIV` holds `3` (the field encodes `divisor - 1`).

`system_control_configure_pll()` performs TivaWare's canonical sequence:

1. Assert `BYPASS`, clear `USESYSDIV` — keep the core clocked while reconfiguring.
2. Clear `MOSCDIS` to enable the main oscillator, then wait for it to stabilise.
3. Select the crystal (`XTAL` = 14 for 8 MHz) and `OSCSRC` = main oscillator.
4. Clear any stale lock status in `MISC`, then release `PWRDN` to power up the PLL.
5. Wait for `RIS.PLLLRIS` (bit 6) with a bounded timeout.
6. Set `SYSDIV` and `USESYSDIV`, then drop `BYPASS` to hand the core to the PLL.

Order matters: dropping `BYPASS` before the PLL locks would leave the core unclocked.

Verify the result from the banner's `rcc:` line — it should read **`0x01CE1380`**
(`SYSDIV`=3, `USESYSDIV`=1, `PWRDN`=0, `BYPASS`=0, `XTAL`=14, `OSCSRC`=main).

> **What QEMU proves and what it does not.** QEMU models `RCC`/`RCC2`, computes the
> system clock from `SYSDIV`, and raises the PLL-lock status bit — so it *does* verify
> that the configuration writes land and that the lock wait terminates. It does **not**
> time the UART against the configured baud rate, so **baud accuracy remains unverified
> until real hardware is attached.**

Register bit positions were cross-verified against two independent sources: TI's own
`hw_sysctl.h` and QEMU's `hw/arm/stellaris.c` model.

## Flash and debug on real hardware

> **Status: verified on hardware.** Flashed and running on a real EK-LM3S6965; every clock
> and UART register was read back from silicon and matched. See
> `.dev-vault/dev/2026-09-15-hardware-flash-success.md`.

The on-board probe is a **Luminary Micro ICDI** — an FTDI FT2232. **Its USB identity is
not what OpenOCD's shipped config expects:**

| Field | OpenOCD `ftdi/luminary-icdi.cfg` | This board |
|---|---|---|
| Vendor ID | `0x0403` | `0x0403` |
| **Product ID** | `0xbcda` | **`0xbcd9`** |
| **Product string** | `"Luminary Micro ICDI Board"` | **`"Stellaris Evaluation Board"`** |

The shipped file targets the LM3S9B9x kit. `openocd/interface/luminary-icdi-ek-lm3s6965.cfg`
in this repo carries the correct identity, and `openocd/board/ek-lm3s6965.cfg` combines it
with the generic Stellaris target. See `.dev-vault/tips/icdi-product-id-mismatch.md`.

### Check the cable first

```bash
scripts/check-connection.sh
```

Reports the detected probe, its VID:PID, and any serial nodes. Uses `ioreg` — **not**
`system_profiler`, which does not report this device on modern macOS.

### Verify the connection (writes nothing)

```bash
scripts/probe.sh
```

Halts the core, prints the device identity (`DID0`/`DID1`), the vector table, and the
flash geometry, then resumes. Run this before flashing.

### Flash

```bash
scripts/flash.sh                      # defaults to build/lm3s6965_firmware
scripts/flash.sh path/to/other.elf    # or be explicit
```

Programs, verifies, and resets:
`openocd -f openocd/board/ek-lm3s6965.cfg -c "program <elf> verify reset exit"`.

### Serial console

> **Not currently available on macOS.** The ICDI exposes a virtual COM port, but recent
> macOS versions ship no FTDI VCP driver (`kmutil showloaded | grep -i ftdi` is empty),
> so no `/dev/cu.usbserial-*` node appears. **This does not block flashing** — OpenOCD
> drives the chip over raw USB via libusb.

To get a console, either install the FTDI VCP driver from ftdi.com (needs admin approval)
or attach a USB-serial adapter to the board's UART0 header. Then:

```bash
picocom -b 115200 /dev/cu.usbserial-XXXX
```

Expected banner (see [Run without hardware](#run-without-hardware-qemu)) including
`sysclk:50000000 Hz` and `rcc: 0x01CE1380`.

**Baud rate without a console:** the divisor is verified directly. `RCC` reads back
`0x01CE1380` (50 MHz) and `UART0_IBRD`/`FBRD` read `27`/`8`; since
`50 000 000 / (16 × 115200) = 27.1267`, the baud rate is 115200 to within crystal tolerance.
That is an inference from measured registers, not observed characters.

### If it fails

The error tells you which layer is broken:

| Message | Meaning |
|---|---|
| `unable to open ftdi device with vid 0403, pid bcda` | Config expects the wrong probe. This board is `0403:bcd9`. |
| `Error: JTAG scan chain interrogation failed` | Probe found, target not responding. Check target power and the JTAG/SWD ribbon. |
| `Error: unknown command` | Config parse failure — OpenOCD version mismatch. |

`0403:bcda` is claimed by macOS's built-in FTDI driver on some systems. If the probe
enumerates but OpenOCD cannot open it, that driver may need unloading.

---

## Source layout

```
cmake/toolchain-lm3s6965.cmake   CMake cross-compilation toolchain file
linker/lm3s6965.ld               Memory layout: 256K flash @ 0x0, 64K SRAM @ 0x20000000
include/lm3s6965/                Register and peripheral headers
src/startup.c                    Vector table, reset handler, .data/.bss init
src/main.c                       Firmware entry point
src/uart.c                       Polled UART0 driver
src/system_control.c             SYSCTL clock gating and PLL configuration
src/syscalls.c                   newlib syscall stubs (_write routes stdout to UART0)
openocd/board/ek-lm3s6965.cfg    OpenOCD board config (Stellaris target + self-contained search path)
openocd/interface/               ICDI interface config for this board's 0403:bcd9 probe
scripts/check-connection.sh      Cable and probe detection
scripts/probe.sh                 Read-only connectivity check
scripts/flash.sh                 Program, verify, reset
```

### Memory map

| Region | Address | Size |
|---|---|---|
| Flash | `0x00000000` | 256 KB |
| SRAM | `0x20000000` | 64 KB |
| Peripherals | `0x40000000` | — |
| Bit-band alias (SRAM / peripheral) | `0x22000000` / `0x42000000` | — |
| Private peripheral bus (SysTick, NVIC) | `0xE0000000` | — |

Peripheral bases (UART0 `0x4000C000`, GPIO A–G, SSI, I²C, GPTM, ADC, Ethernet) are
in `include/lm3s6965/memory_map.h`.

## Function index

| Function | Description |
|---|---|
| `main` | Initializes UART0 and prints the bring-up banner, then idles |
| `Reset_Handler` | Copies `.data` to SRAM, zeroes `.bss`, runs `__libc_init_array`, calls `main` |
| `Default_Handler` | Catch-all interrupt handler; parks forever |
| `_init` | Empty stub normally supplied by `crti.o`, which `-nostartfiles` omits |
| `uart0_initialize` | Configures the UART0 clock gate, baud divisor, and 8N1 framing |
| `uart0_write_byte` | Blocks until the TX FIFO has room, then writes one byte |
| `uart0_write` | Writes a NUL-terminated string to UART0 |
| `uart0_register_address` | Maps a UART0 register offset to an absolute address |
| `system_control_register_address` | Maps a SYSCTL register offset to an absolute address |
| `spin_delay` | Busy-waits a fixed number of loop iterations |
| `system_control_enable_peripheral_clock` | Sets a clock-gating bit in a SYSCTL `RCGC` register |
| `system_control_configure_pll` | Switches the core to the 50 MHz PLL output; returns false and stays on the oscillator if the PLL never locks |
| `system_control_wait_for_pll_lock` | Polls `RIS.PLLLRIS` for PLL lock, bounded by a timeout; returns iterations remaining |
| `system_control_read_rcc` | Reads back `RCC` so firmware can self-verify its clock configuration |
| `_write` | newlib stub redirecting stdout to UART0 |
| `_sbrk` | Bump allocator growing the heap from `_end` toward the stack |
| `_read`, `_close`, `_fstat`, `_isatty`, `_lseek`, `_exit`, `_kill`, `_getpid` | Remaining newlib stubs |

## Key build flags

| Flag | Reason |
|---|---|
| `-mcpu=cortex-m3` | ARMv7-M, implies Thumb-2 and `-mthumb` |
| `-mfloat-abi=soft` | Cortex-M3 has **no FPU**; hard-float would emit undefined instructions |
| `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` | Drops unused code to save flash |
| `-nostartfiles` | Suppresses newlib `crt0`, which would inject a conflicting `.init` section |
| `--specs=nano.specs` | Newlib-nano: much smaller `printf` |
| `--specs=nosys.specs` | Provides default syscall stubs; ours override them |
| `-Wall -Wextra -Werror` | Surfaces defects at build time rather than on hardware |

## Known limitations

- **UART baud is verified by register read-back, not by observing characters.** On the
  attached EK-LM3S6965, `RCC` reads back `0x01CE1380` and `UART0_IBRD`/`FBRD` read 27/8, so
  `50 000 000 / (16 × 115200) = 27.1267` holds on the real part. QEMU does not time UART
  output against the baud rate, and no serial console is reachable on macOS (no FTDI VCP
  driver), so UART output has not been observed end to end.
- **Main-oscillator startup uses a fixed delay**, not a status bit. The LM3S6965
  datasheet sequence is followed, but the delay constant (524288 iterations) is a
  conservative bound rather than a measured value.
- **Flashing is verified on hardware.** The firmware was programmed to an EK-LM3S6965 with
  `program … verify reset` and confirmed running (vector table `0x20010000 0x000000f1`,
  `RCC` read back as `0x01CE1380`). Note this **overwrote the factory firmware without a
  backup**.
- **The ICDI's virtual COM port is unreachable on macOS.** No FTDI VCP driver is installed,
  so no `/dev/cu.usbserial-*` node appears. JTAG and flashing are unaffected — OpenOCD
  drives the probe over raw USB via libusb.
- Only UART0 is implemented; GPIO, timers, and other peripherals are unimplemented.
