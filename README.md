# LM3S6965 Bare-Metal Cross-Compilation Toolchain

This repository holds a cross-compilation environment for the TI LM3S6965. The LM3S6965
is an ARM Cortex-M3 microcontroller with 256 KB of flash, 64 KB of SRAM, and a 50 MHz
clock. The environment runs on macOS Apple Silicon. It needs no vendor IDE and no Docker.

The chip dates from 2007. TI no longer supports its tooling. Code Composer Studio,
StellarisWare, and LM Flash Programmer run only on Windows. This repository replaces that
stack with open-source tools that run natively on arm64 macOS.

## Why this works at all

ARM defines the core. The vendor defines the peripherals. The `arm-none-eabi-gcc`
toolchain targets the core, so it does not care how old the silicon is. Only the vendor
layer is dead. This repository rebuilds that layer from the datasheet.

---

## Required packages

All versions in this table were verified on macOS 26.6 (arm64).

| Package | Verified version | Purpose | Install |
|---|---|---|---|
| **Arm GNU Toolchain** | **15.3.rel1** | Cross compiler, newlib C library, binutils, and GDB | see [below](#installing-the-arm-toolchain) |
| CMake | 4.4.2 | Build orchestration | `brew install cmake` |
| QEMU | 11.1.1 | Emulates the `lm3s6965evb` board. No hardware needed. | `brew install qemu` |
| OpenOCD | 0.12.0 | Flash programming and a GDB server over the ICDI probe | `brew install openocd` |
| libusb | 1.0.30 | USB access for the FTDI debug probe | `brew install libusb` |
| libftdi | 1.5_2 | FTDI FT2232 driver for the Stellaris ICDI | `brew install libftdi` |
| libusb-compat | 0.1.9 | Legacy libusb 0.1 API. Some OpenOCD paths need it. | `brew install libusb-compat` |
| picocom | 2024-07 | Serial console for UART0 output | `brew install picocom` |
| pyftdi | 0.57.2 | Python FTDI driver. Reads the ICDI SWO channel over libusb. | `pip install pyftdi` |
| arm-none-eabi-gdb | 17.2 | Standalone debugger. The Arm toolchain also bundles it. | `brew install arm-none-eabi-gdb` |

Install everything except the Arm toolchain with one command:

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

`scripts/icdi-console.sh` finds this environment automatically. Set `PYFTDI_PYTHON` to
point at a different interpreter.

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

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
cmake --build build
```

Configure prints the resolved toolchain and the libc path. A misconfigured environment is
then obvious at once:

```
-- LM3S6965 toolchain: .../arm-none-eabi-gcc
-- LM3S6965 libc     : .../thumb/v7-m/nofp/libc.a
```

Artifacts land in `build/`:

| File | Purpose |
|---|---|
| `lm3s6965_firmware` | ELF. GDB and QEMU use this file. |
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

Expect these `readelf -A` attributes: `Tag_CPU_name: "7-M"`, `Tag_CPU_arch: v7`,
`Tag_CPU_arch_profile: Microcontroller`, and `Tag_THUMB_ISA_use: Thumb-2`.

## Run without hardware (QEMU)

QEMU emulates this exact board. You can test the firmware with no silicon attached:

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

The `rcc:` line is a self-check. The firmware reads its own clock register. See
[System clock](#system-clock).

The firmware never returns. Exit QEMU with `Ctrl-A X`, or kill the process.
`qemu-system-arm` has no timeout flag and macOS has no `timeout(1)`. To script a bounded
run, start the emulator in the background, sleep, then kill it.

## System clock

The LM3S6965 is a Fury-class part. Its PLL produces a fixed **200 MHz**. `SYSDIV` divides
that value down. The EK-LM3S6965 has an **8 MHz** crystal. 200 / 4 = **50 MHz**, so
`SYSDIV` holds `3`. The field encodes `divisor - 1`.

`system_control_configure_pll()` runs the TivaWare sequence:

1. Set `BYPASS` and clear `USESYSDIV`. The core stays clocked during the change.
2. Clear `MOSCDIS` to start the main oscillator. Wait for it to stabilize.
3. Select the crystal and the main oscillator. Use `XTAL` = 14 for 8 MHz, and `OSCSRC` =
   main oscillator.
4. Clear stale lock status in `MISC`. Then clear `PWRDN` to power up the PLL.
5. Wait for `RIS.PLLLRIS` (bit 6). This wait has a timeout.
6. Set `SYSDIV` and `USESYSDIV`. Then clear `BYPASS` to give the core to the PLL.

The order is important. If you clear `BYPASS` before the PLL locks, the core loses its
clock.

Check the `rcc:` line in the banner. It must read **`0x01CE1380`**. That value means
`SYSDIV`=3, `USESYSDIV`=1, `PWRDN`=0, `BYPASS`=0, `XTAL`=14, and `OSCSRC`=main.

> **What QEMU proves, and what it does not.** QEMU models `RCC` and `RCC2`. It computes the
> system clock from `SYSDIV`. It raises the PLL lock status bit. QEMU therefore proves that
> the configuration writes land and that the lock wait ends. QEMU does **not** time the UART
> against the configured baud rate. See [Known limitations](#known-limitations).

Two independent sources confirm the register bit positions: the TI `hw_sysctl.h` header and
the QEMU `hw/arm/stellaris.c` model.

## Flash and debug on real hardware

> **Status: verified on hardware.** The firmware runs on a real EK-LM3S6965. Every clock and
> UART register was read back from silicon and matched.

The on-board probe is a **Luminary Micro ICDI**, which contains an FTDI FT2232. Its USB
identity is **not** what the OpenOCD shipped config expects:

| Field | OpenOCD `ftdi/luminary-icdi.cfg` | This board |
|---|---|---|
| Vendor ID | `0x0403` | `0x0403` |
| **Product ID** | `0xbcda` | **`0xbcd9`** |
| **Product string** | `"Luminary Micro ICDI Board"` | **`"Stellaris Evaluation Board"`** |

The shipped file targets the LM3S9B9x kit. `openocd/interface/luminary-icdi-ek-lm3s6965.cfg`
in this repository carries the correct identity. `openocd/board/ek-lm3s6965.cfg` combines it
with the generic Stellaris target.

### Check the cable first

```bash
scripts/check-connection.sh
```

This script reports the detected probe, its VID and PID, and any serial nodes. It reads
`ioreg`. It does not read `system_profiler`, which does not report this device on recent
macOS versions.

### Verify the connection (writes nothing)

```bash
scripts/probe.sh
```

This script halts the core. It prints the device identity (`DID0` and `DID1`), the vector
table, and the flash geometry. It then resumes the core. Run it before you flash.

### Flash

```bash
scripts/flash.sh                      # defaults to build/lm3s6965_firmware
scripts/flash.sh path/to/other.elf    # or name the file
```

The script programs, verifies, and resets the target:
`openocd -f openocd/board/ek-lm3s6965.cfg -c "program <elf> verify reset exit"`.

### Console output

The firmware sends every `printf` byte to **two** consoles at the same time. Each one has its
own setup call in `main`.

| Backend | Path out of the chip | Read it with | Rate |
|---|---|---|---|
| UART0 | the `PA0` and `PA1` header pins | a USB serial adapter | 115200, 8N1 |
| ITM and SWO | the trace pin, tapped by the on-board ICDI | the on-board ICDI | 1000000, 8N1 |

`uart0_initialize` configures the UART. `trace_initialize` configures the ITM and the TPIU.
`_write` in `src/syscalls.c` fans each byte out to both.

#### The SWO console, through the ICDI

This path needs no extra hardware. Two conditions apply.

1. **The probe must use SWD.** In JTAG mode the `TDO` pin carries JTAG data, so the ICDI tap
   cannot see SWO. In SWD mode that pin carries SWO instead. OpenOCD asserts the ICDI's
   `SWD_EN` line for `transport select swd`, and the ICDI switches over.
2. **The rate must match.** The firmware sets 1 Mbaud through `SWO_BAUD_RATE`. The TPIU
   prescaler is `system_clock_hz / SWO_BAUD_RATE - 1`.

An SWD probe does not prevent JTAG from working later. This was measured on the board: with
the trace unit fully enabled, a JTAG connection still read `DID0` and every trace register.
The debug port arbitrates the shared pin by transport. There is no mode to forget to change
back.

Capture the banner. Install `pyftdi` first, as in [Required packages](#required-packages).

```bash
scripts/icdi-console.sh
```

The script finds a Python interpreter with `pyftdi`, opens the ICDI second channel, starts
the reader, then resets the target. The order matters, because the firmware prints the
banner one time only. The output ends with `lm3s6965 bring-up complete`, and the script
prints `PASS: console received over SWO through the ICDI`.

| Option | Effect |
|---|---|
| `--seconds N` | Capture window in seconds. The default is 4. |
| `--baud RATE` | SWO rate. It must match `SWO_BAUD_RATE`. The default is 1000000. |
| `--raw` | Also print the raw captured bytes as hex. |

> **The probe must stay attached for the whole capture window.** The board's CPLD routes
> SWO only while the debugger asserts `SWD_EN`. A capture that stops OpenOCD too early
> receives nothing.

#### The UART console

A USB serial adapter on the UART0 header also works. The firmware muxes `PA0` and `PA1` to
UART0, so the header is live.

> **The firmware prints the banner one time only.** It prints from `main()`, directly after
> reset. The reader must therefore listen before the reset. `scripts/console.sh` starts the
> reader first and resets the target second. A manual capture needs the same order.

#### The ICDI virtual COM port is not a serial port

macOS cannot bind the ICDI virtual COM port. The FTDI VCP driver is a system extension. Its
`Info.plist` lists **436** accepted `idVendor:idProduct` pairs. The pair `0x0403:0xbcd9` of
this board is not in that list. The driver therefore never creates a `/dev/cu.usbserial-*`
node. Installing the driver does not help.

That limit does not matter. The SWO console above reaches the ICDI over raw libusb, exactly
as OpenOCD does.

#### Wire the adapter

| Board UART0 header | Adapter |
|---|---|
| PA1 (UART0 TX) | RXD |
| PA0 (UART0 RX) | TXD |
| GND | GND |

Use 115200 baud, 8 data bits, no parity, 1 stop bit, and no flow control.

#### Capture the banner

```bash
scripts/console.sh
```

The script finds the adapter, opens the port, and resets the target.

> **The firmware prints the banner one time only.** It prints from `main()`, directly after
> reset. The reader must therefore listen before the reset. `scripts/console.sh` starts the
> reader first and resets the target second. A manual capture needs the same order.

To read the port by hand, start `picocom` first. Reset the board second.

```bash
picocom -b 115200 /dev/cu.usbserial-XXXX
```

Expect the banner from [Run without hardware](#run-without-hardware-qemu). It includes
`sysclk:50000000 Hz` and `rcc: 0x01CE1380`.

**Baud rate without a console.** The divisor registers confirm the rate directly. `RCC`
reads back `0x01CE1380`, which is 50 MHz. `UART0_IBRD` and `FBRD` read `27` and `8`. The
relation `50 000 000 / (16 × 115200) = 27.1267` therefore holds. The baud rate is 115200
within the crystal tolerance. This result comes from measured registers, not from observed
characters.

### If it fails

The error message tells you which layer is broken:

| Message | Meaning |
|---|---|
| `unable to open ftdi device with vid 0403, pid bcda` | The config expects the wrong probe. This board is `0403:bcd9`. |
| `Error: JTAG scan chain interrogation failed` | The probe is present, but the target does not answer. Check the target power and the JTAG/SWD ribbon. |
| `Error: unknown command` | Config parse failure. The OpenOCD version does not match. |

Some systems give `0403:bcda` to the built-in FTDI driver in macOS. If the probe enumerates
but OpenOCD cannot open it, unload that driver.

---

## Source layout

```
cmake/toolchain-lm3s6965.cmake   CMake cross-compilation toolchain file
linker/lm3s6965.ld               Memory layout: 256K flash @ 0x0, 64K SRAM @ 0x20000000
include/lm3s6965/                Register and peripheral headers
src/startup.c                    Vector table, reset handler, .data/.bss init
src/main.c                       Firmware entry point
src/uart.c                       Polled UART0 driver
src/gpio.c                       GPIO alternate-function selection and digital enable
src/trace.c                      ITM and TPIU setup for the SWO console
src/system_control.c             SYSCTL clock gating and PLL configuration
src/syscalls.c                   newlib syscall stubs (_write routes stdout to UART0)
openocd/board/ek-lm3s6965.cfg    OpenOCD board config (Stellaris target + self-contained search path)
openocd/interface/               ICDI interface config for this board's 0403:bcd9 probe
scripts/check-connection.sh      Cable and probe detection
scripts/probe.sh                 Read-only connectivity check
scripts/flash.sh                 Program, verify, reset
scripts/console.sh               Read the UART0 console through a USB serial adapter
scripts/icdi-console.sh          Read the SWO console through the on-board ICDI
```

### Memory map

| Region | Address | Size |
|---|---|---|
| Flash | `0x00000000` | 256 KB |
| SRAM | `0x20000000` | 64 KB |
| Peripherals | `0x40000000` | — |
| Bit-band alias (SRAM / peripheral) | `0x22000000` / `0x42000000` | — |
| Private peripheral bus (SysTick, NVIC) | `0xE0000000` | — |

The peripheral base addresses (UART0 `0x4000C000`, GPIO A to G, SSI, I2C, GPTM, ADC, and
Ethernet) are in `include/lm3s6965/memory_map.h`.

## Function index

| Function | Description |
|---|---|
| `main` | Initializes UART0 and prints the bring-up banner. The function then idles. |
| `Reset_Handler` | Copies `.data` to SRAM, zeroes `.bss`, runs `__libc_init_array`, and calls `main`. |
| `Default_Handler` | Catches every unhandled interrupt and parks forever. |
| `_init` | Empty stub that `crti.o` normally supplies. The `-nostartfiles` flag omits it. |
| `uart0_initialize` | Configures the UART0 clock gate, the baud divisor, and 8N1 framing. |
| `uart0_write_byte` | Blocks until the TX FIFO has room. The function then writes one byte. |
| `uart0_write` | Writes a NUL-terminated string to UART0. |
| `uart0_register_address` | Maps a UART0 register offset to an absolute address. |
| `system_control_register_address` | Maps a SYSCTL register offset to an absolute address. |
| `gpio_register_address` | Maps a GPIO port register offset to an absolute address. |
| `gpio_select_alternate_function` | Routes a port's masked pins to their alternate hardware function (`GPIOAFSEL`). |
| `gpio_select_protected_alternate_function` | Same, for the guarded PB7/PC[3:0] pins: unlocks `GPIOLOCK`, sets the `GPIOCR` commit bits, writes `GPIOAFSEL`, then re-locks. |
| `gpio_enable_digital_function` | Enables the digital function on a port's masked pins (`GPIODEN`). |
| `trace_register_address` | Maps a trace register offset to an absolute address. |
| `trace_initialize` | Enables the ITM and TPIU so console output also leaves as SWO. |
| `trace_write_byte` | Waits for the ITM stimulus port ready bit, then writes one byte to port 0. |
| `spin_delay` | Busy-waits a fixed number of loop iterations. |
| `system_control_enable_peripheral_clock` | Sets a clock-gating bit in a SYSCTL `RCGC` register. |
| `system_control_configure_pll` | Switches the core to the 50 MHz PLL output. Returns false and stays on the oscillator if the PLL never locks. |
| `system_control_wait_for_pll_lock` | Polls `RIS.PLLLRIS` for PLL lock with a bounded timeout. Returns the iterations remaining. |
| `system_control_read_rcc` | Reads back `RCC`. The firmware uses this value to check its own clock configuration. |
| `_write` | newlib stub that sends stdout to UART0 and to the ITM. |
| `_sbrk` | Bump allocator. The heap grows from `_end` toward the stack. |
| `_read`, `_close`, `_fstat`, `_isatty`, `_lseek`, `_exit`, `_kill`, `_getpid` | Remaining newlib stubs |

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

## Known limitations

- **The UART baud rate is verified by register read-back, not by observed characters.** On
  the attached EK-LM3S6965, `RCC` reads back `0x01CE1380` and `UART0_IBRD`/`FBRD` read 27/8.
  The relation `50 000 000 / (16 × 115200) = 27.1267` therefore holds on the real part. QEMU
  does not time UART output against the baud rate, and macOS has no reachable serial console.
  UART output was not observed end to end.
- **Main-oscillator startup uses a fixed delay, not a status bit.** The code follows the
  LM3S6965 datasheet sequence. The delay constant (524288 iterations) is a conservative
  bound, not a measured value.
- **Flashing is verified on hardware.** The firmware was programmed to an EK-LM3S6965 with
  `program … verify reset`. The vector table reads `0x20010000 0x000000f1` and `RCC` reads
  `0x01CE1380`. This **overwrote the factory firmware with no backup**.
- **The ICDI console is SWO, not a serial port.** macOS cannot bind the ICDI virtual COM
  port, because the FTDI VCP extension whitelist excludes `0x0403:0xbcd9`. The console uses
  the Cortex-M3 trace pin instead. The on-board CPLD taps that pin and forwards it to the
  ICDI's second channel, which is readable over raw libusb. The probe must use SWD, because
  in JTAG mode the shared pin carries JTAG data. No serial adapter is needed.
- **Only UART0 is implemented.** GPIO, timers, and other peripherals are not implemented.
