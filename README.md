# LM3S6965 Bare-Metal Cross-Compilation Toolchain

This repository holds a cross-compilation environment for the TI LM3S6965. The LM3S6965 is an
ARM Cortex-M3 microcontroller. It has 256 KB of flash, 64 KB of SRAM, and a 50 MHz clock. The
environment runs on macOS Apple Silicon. It needs no vendor IDE and no Docker.

The chip dates from 2007. TI no longer supports its tooling. Code Composer Studio, StellarisWare,
and LM Flash Programmer run only on Windows. This repository replaces that stack with open-source
tools that run natively on arm64 macOS.

## Why this works

ARM defines the core. The vendor defines the peripherals. The `arm-none-eabi-gcc` toolchain
targets the core, so it does not care how old the silicon is. Only the vendor layer is dead. This
repository rebuilds that layer from the datasheet.

---

## Required packages

The project verified all versions in this table on macOS 26.6 (arm64).

| Package | Verified version | Purpose | Install |
|---|---|---|---|
| **Arm GNU Toolchain** | **15.3.rel1** | Cross compiler, newlib C library, binutils, and GDB | see [below](#installing-the-arm-toolchain) |
| CMake | 4.4.2 | Builds the project | `brew install cmake` |
| QEMU | 11.1.1 | Emulates the `lm3s6965evb` board. No hardware needed. | `brew install qemu` |
| OpenOCD | 0.12.0 | Programs the flash and serves GDB over the ICDI probe | `brew install openocd` |
| libusb | 1.0.30 | Gives USB access to the FTDI debug probe | `brew install libusb` |
| libftdi | 1.5_2 | Drives the FTDI FT2232 on the Stellaris ICDI | `brew install libftdi` |
| libusb-compat | 0.1.9 | Provides the legacy libusb 0.1 API. Some OpenOCD paths need it. | `brew install libusb-compat` |
| picocom | 2024-07 | Reads the UART0 serial console | `brew install picocom` |
| pyftdi | 0.57.2 | Python FTDI driver. Reads the ICDI SWO channel over libusb. | `pip install pyftdi` |
| arm-none-eabi-gdb | 17.2 | Standalone debugger. The Arm toolchain also bundles it. | `brew install arm-none-eabi-gdb` |

Install all packages except the Arm toolchain with one command:

```bash
brew install cmake qemu openocd libusb libftdi libusb-compat picocom arm-none-eabi-gdb
```

> Homebrew ships OpenOCD under the formula name **`open-ocd`**. The
> `brew install openocd` alias still resolves. The binary is still `openocd`.

Install `pyftdi` into a virtual environment. Python packages do not belong to Homebrew:

```bash
python3 -m venv .venv
.venv/bin/pip install pyftdi
```

`scripts/icdi-console.sh` finds this environment automatically. Set `PYFTDI_PYTHON` to point at a
different interpreter.

### Installing the Arm toolchain

> ### ⚠️ Do not use the Homebrew formula `arm-none-eabi-gcc`
>
> The Homebrew **formula** `arm-none-eabi-gcc` builds GCC and binutils. It ships
> **no C library at all**. The sysroot is empty and there is no newlib. Its own
> `<stdint.h>` starts with `#include_next <stdint.h>`. That directive expects a
> libc header that does not exist. Every build then fails with:
>
> ```
> fatal error: stdint.h: No such file or directory
>     11 | # include_next <stdint.h>
> ```
>
> Homebrew has **no** `arm-none-eabi-newlib` formula. Use only the **official
> Arm GNU Toolchain**, which bundles newlib.
>
> `cmake/toolchain-lm3s6965.cmake` searches for the official toolchain first. If it
> finds a toolchain with no libc, it stops with an actionable message.

**Option A: official cask.** This needs a `sudo` password one time.

```bash
brew install --cask gcc-arm-embedded
```

**Option B: tarball.** This needs no `sudo`. This repository uses this path.

```bash
TC_DIR="$HOME/.toolchains"
ARCHIVE="arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi.tar.xz"
URL="https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/$ARCHIVE"

mkdir -p "$TC_DIR" && cd "$TC_DIR"
curl -sL --fail -o "$ARCHIVE" "$URL"
tar -xf "$ARCHIVE" && rm -f "$ARCHIVE"
```

The toolchain installs to `$HOME/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/`.
The extracted directory is about 1.0 GB. CMake finds it automatically. To use a different
location, pass `-DLM3S6965_TOOLCHAIN_BIN_HINTS=/path/to/toolchain/bin`.

---

## Build

The FreeRTOS kernel is a git submodule. Fetch it before the first build:

```bash
git submodule update --init --recursive
```

Then configure and build:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
cmake --build build
```

Configure prints the resolved toolchain and the libc path. The output shows a misconfigured
environment at once:

```
-- LM3S6965 toolchain: .../arm-none-eabi-gcc
-- LM3S6965 libc     : .../thumb/v7-m/nofp/libc.a
```

The build writes these files to `build/`:

| File | Purpose |
|---|---|
| `examples/baremetal/lm3s6965_firmware` | ELF. GDB and QEMU use this file. |
| `examples/baremetal/lm3s6965_firmware.bin` | Raw binary for flashing |
| `examples/baremetal/lm3s6965_firmware.hex` | Intel HEX for flashing |
| `examples/baremetal/lm3s6965_firmware.map` | Linker map |
| `examples/freertos/lm3s6965_freertos_firmware` | FreeRTOS ELF. GDB and QEMU use this file. |
| `examples/freertos/lm3s6965_freertos_firmware.bin` | FreeRTOS raw binary for flashing |
| `examples/freertos/lm3s6965_freertos_firmware.hex` | FreeRTOS Intel HEX for flashing |
| `examples/freertos/lm3s6965_freertos_firmware.map` | FreeRTOS linker map |

The bare-metal firmware now initializes the OLED, clears it, and draws the 128 x 72 LUMON image.
`scripts/oled-brightness.sh <value>` sets contrast at runtime.

## Key build flags

| Flag | Reason |
|---|---|
| `-mcpu=cortex-m3` | ARMv7-M. This flag also sets Thumb-2 and `-mthumb`. |
| `-mfloat-abi=soft` | The Cortex-M3 has **no FPU**. Hard-float would emit undefined instructions. |
| `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` | Drops unused code to save flash |
| `-nostartfiles` | Suppresses the newlib `crt0`, which would inject a conflicting `.init` section |
| `--specs=nano.specs` | Newlib-nano. The `printf` function becomes much smaller. |
| `--specs=nosys.specs` | Provides default syscall stubs. The stubs in this repository override them. |
| `-Wall -Wextra -Werror` | Shows defects at build time, not on hardware |

## Flash and debug on real hardware

> **Status: verified on hardware.** The firmware runs on a real EK-LM3S6965. The project read
> every clock and UART register back from silicon. All values matched.

The on-board probe is a **Luminary Micro ICDI**, which contains an FTDI FT2232. Its USB identity
is **not** the identity in the config that OpenOCD ships:

| Field | OpenOCD `ftdi/luminary-icdi.cfg` | This board |
|---|---|---|
| Vendor ID | `0x0403` | `0x0403` |
| **Product ID** | `0xbcda` | **`0xbcd9`** |
| **Product string** | `"Luminary Micro ICDI Board"` | **`"Stellaris Evaluation Board"`** |

The shipped file targets the LM3S9B9x kit. `openocd/interface/luminary-icdi-ek-lm3s6965.cfg` in
this repository carries the correct identity. `openocd/board/ek-lm3s6965.cfg` adds the generic
Stellaris target.

### Check the cable first

```bash
scripts/check-connection.sh
```

This script reports the detected probe, its VID and PID, and any serial nodes. It reads `ioreg`.
It does not read `system_profiler`, which does not report this device on recent macOS versions.

### Verify the connection (writes nothing)

```bash
scripts/probe.sh
```

This script halts the core. It prints the device identity (`DID0` and `DID1`), the vector table,
and the flash geometry. It then resumes the core. Run it before you flash.

### Flash

```bash
scripts/flash.sh                      # defaults to build/examples/baremetal/lm3s6965_firmware
scripts/flash.sh path/to/other.elf    # or name the file
```

The script programs, verifies, and resets the target:
`openocd -f openocd/board/ek-lm3s6965.cfg -c "program <elf> verify reset exit"`.

### Flash the FreeRTOS image

1. Build the image.

   ```bash
   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
   cmake --build build
   ```

2. Check the probe and the cable.

   ```bash
   scripts/check-connection.sh
   ```

3. Halt the core and read the device. This step writes nothing.

   ```bash
   scripts/probe.sh
   ```

4. Flash the FreeRTOS image. Run the command from the repository root.

   ```bash
   scripts/flash.sh build/examples/freertos/lm3s6965_freertos_firmware
   ```

   The script programs, verifies, and resets the target. If you work from another directory,
   use an absolute path, because the script calls `realpath`.

5. Capture the console. The banner prints one time after reset, so start the reader first.

   ```bash
   scripts/icdi-console.sh
   ```

   The SWO path needs no extra hardware. Keep the probe attached for the whole capture window.
   For a USB serial adapter on the UART0 header, use `scripts/console.sh` instead.

**Notes:**

- The FreeRTOS image is 11.7 KB of text. It fits the 256 KB flash.
- Flashing overwrites the current firmware. There is no backup.
- To restore the bare-metal image, run `scripts/flash.sh` with no argument.
- The project verified the FreeRTOS image in QEMU only. The hardware path remains unverified.

## Source layout

```
third_party/FreeRTOS-Kernel/     FreeRTOS kernel git submodule, pinned to V11.3.1
cmake/toolchain-lm3s6965.cmake   CMake cross-compilation toolchain file
linker/lm3s6965.ld               Memory layout: 256K flash @ 0x0, 64K SRAM @ 0x20000000
include/lm3s6965/                BSP public headers
src/                             BSP sources and src/CMakeLists.txt
src/startup.c                    Vector table, reset handler, .data/.bss init
src/uart.c                       Polled UART0 driver
src/gpio.c                       GPIO alternate-function, digital enable, and input configuration
src/led.c                        User LED driver
src/switch.c                     Navigation and select switch driver
src/trace.c                      ITM and TPIU setup for the SWO console
src/system_control.c             SYSCTL clock gating and PLL configuration
src/syscalls.c                   newlib syscall stubs (_write routes stdout to UART0 and the ITM)
freertos/                        BSP FreeRTOS support (CMakeLists.txt, FreeRTOSConfig.h, hooks.c)
freertos/CMakeLists.txt          FreeRTOS target build script
freertos/FreeRTOSConfig.h        FreeRTOS configuration and handler mapping
freertos/hooks.c                 FreeRTOS hook and assertion callbacks
examples/baremetal/              bare-metal example (main.c, CMakeLists.txt)
examples/freertos/               FreeRTOS example (main.c, CMakeLists.txt)
openocd/board/ek-lm3s6965.cfg    OpenOCD board config (Stellaris target + self-contained search path)
openocd/interface/               ICDI interface config for this board's 0403:bcd9 probe
scripts/check-connection.sh      Cable and probe detection
scripts/probe.sh                 Read-only connectivity check
scripts/flash.sh                 Program, verify, reset
scripts/console.sh               Read the UART0 console through a USB serial adapter
scripts/icdi-console.sh          Read the SWO console through the on-board ICDI
```

## Known limitations

- **The project verified the UART baud rate by register read-back, not by observed characters.**
  On the attached EK-LM3S6965, `RCC` reads back `0x01CE1380` and `UART0_IBRD`/`FBRD` read 27/8.
  The relation `50 000 000 / (16 × 115200) = 27.1267` therefore holds on the real part. QEMU does
  not time UART output against the baud rate. macOS has no reachable serial console, so no test
  observed UART output end to end.
- **Main-oscillator startup uses a fixed delay, not a status bit.** The code follows the LM3S6965
  datasheet sequence. The delay constant (524288 iterations) is a conservative bound, not a
  measured value.
- **The project verified flashing on hardware.** It programmed the firmware to an EK-LM3S6965
  with `program … verify reset`. The vector table reads `0x20010000 0x000000f1` and `RCC` reads
  `0x01CE1380`. This step **overwrote the factory firmware with no backup**.
- **The ICDI console is SWO, not a serial port.** macOS cannot bind the ICDI virtual COM port,
  because the FTDI VCP extension whitelist excludes `0x0403:0xbcd9`. The console uses the
  Cortex-M3 trace pin instead. The on-board CPLD taps that pin and forwards it to the ICDI's
  second channel, which is readable over raw libusb. The probe must use SWD, because in JTAG mode
  the shared pin carries JTAG data. No serial adapter is needed.
- **The project implements only UART0 among the serial ports.** GPIO, the user LED, the switches,
  SysTick, and the ITM trace block work. The project does not implement UART1, UART2, SSI, I2C,
  PWM, QEI, ADC, the general-purpose timers, or Ethernet.
- **QEMU runs the console slowly.** The firmware paces each ITM write on the stimulus port ready
  bit. QEMU does not model the ITM, so each byte spins the bounded wait loop. A full banner takes
  about 45 seconds. Use a capture window of 45 seconds or more.
- **The FreeRTOS target uses a 32 KB heap.** heap_4 owns one 32 KB block in `.bss`. Task stacks
  come from that block. The bare-metal target does not use this heap.
