## Toolchain

The build targets ARMv7-M with the official Arm GNU Toolchain, newlib-nano, and a hand-written
startup path, with no Docker and no vendor SDK.

### The official Arm GNU Toolchain, not the Homebrew formula

The LM3S6965 is a 2007-era Cortex-M3. Its vendor tooling (Code Composer Studio, StellarisWare,
LM Flash Programmer) is Windows-only and abandoned, so the build must run on macOS Apple
Silicon. The Homebrew formula `arm-none-eabi-gcc` is not a substitute: it builds GCC and
binutils but **no C library at all**, so it has an empty sysroot and no newlib. GCC's own
`<stdint.h>` ends with `#include_next <stdint.h>`, deliberately delegating to a libc header
that does not exist, and every build fails with `fatal error: stdint.h: No such file or
directory`. Homebrew has no `arm-none-eabi-newlib` formula.

The official Arm GNU Toolchain bundles newlib, `nano.specs`, `nosys.specs`, binutils, and GDB.
This repository installs release `15.3.rel1` from Arm's GitLab package registry as a `.tar.xz`
rather than the Homebrew cask, because the cask requires an interactive `sudo` password, which
breaks non-interactive agent sessions and CI. See [Installing the Arm
toolchain](../README.md#installing-the-arm-toolchain).

### Validating the toolchain

A compiler is a driver that assumes a C library exists. The unit to validate for bare-metal
work is the whole **toolchain**: compiler, binutils, C library, and startup objects. Two probes
do this up front:

```bash
arm-none-eabi-gcc -print-sysroot        # must NOT be empty
arm-none-eabi-gcc -print-file-name=libc.a
```

`-print-file-name` echoes its argument unchanged when the file is not found, so a bare relative
filename such as `libc.a` is the **failure** output. A real answer is always an absolute path.
This is the most misleading probe in the GNU Arm toolchain.

`cmake/toolchain-lm3s6965.cmake` encodes this check, so a libc-less toolchain fails at
**configure** time with a clear message instead of at compile time with a confusing one.
Configure prints the resolved toolchain and libc path.

### Native macOS, no Docker

The toolchain runs natively on macOS; the project does not use a container. Docker could not
host the whole workflow in any case: USB device passthrough is a Linux-host-only capability, so
on macOS, where Docker Desktop runs a Linux VM, a container cannot see the FTDI debug probe and
OpenOCD inside it can never flash the board. A hybrid split would have been the only option.
The trade-off is losing toolchain pinning via a container image, mitigated by pinning the exact
version in the README and by CMake failing loudly on the wrong one.

### Hand-written register headers, not a vendored SDK

The vendor driver libraries (StellarisWare, TivaWare) are abandoned, Windows-oriented in their
packaging, large, and ambiguous in licence. This project instead writes minimal register headers
from the datasheet. They stay small and auditable and avoid a dead dependency. The memory map
was cross-checked against QEMU's `hw/arm/stellaris.c`, which cites the LM3S6965 datasheet
(rev I) inline, so the transcription has independent corroboration.

### `-nostartfiles` and the newlib startup contract

Writing a vector table and linker script means replacing newlib's startup contract. Three link
failures follow, in order:

1. **`undefined reference to _sidata`** — the linker script used `_etext` as the `.data` load
   address but never defined `_sidata`. Fix: let the linker compute it with
   `_sidata = LOADADDR(.data);` and place `.data` with `> SRAM AT> FLASH`.

2. **`section .init LMA overlaps section .data LMA`** — newlib's `crti.o` and `crtn.o`
   contribute `.init` and `.fini`, which land where the custom script put `.data`'s load
   address. (`crt0.o` has neither; it supplies `_start`, which calls `main`.) Fix:
   `-nostartfiles` drops all three, and the custom reset path supplies their role.

3. **`undefined reference to _init`** — `-nostartfiles` also removed `crti.o`, where `_init`
   lives, and `__libc_init_array()` calls it. Fix: define an empty `_init()`.

Keeping `__libc_init_array()` (with the empty `_init` and the `.init_array` output section)
preserves C++ static constructors and `__attribute__((constructor))` for about four lines of
code.

The general shape: removing a startup object can silently break *other* library code that
assumed it was present. The linker script owns `_sidata`, `_sdata`, `_edata`, `_sbss`, and
`_ebss`; `crti.o` owns `_init`, which `__libc_init_array` calls; newlib owns `_sbrk` (for
`malloc`) and `_write` (for `printf`).

### Newlib-nano and `nosys.specs`

`--specs=nano.specs` selects a much smaller `printf`. `--specs=nosys.specs` supplies default
syscall stubs, which `src/syscalls.c` overrides at object-file precedence. `_write` routes
stdout to UART0, so `printf` is the primary diagnostic channel over the serial console. The
cost is about 4.8 KB of flash on a 256 KB part.

### Core versus vendor: what actually decays

The intuition that a 2007 chip needs a 2007 toolchain conflates two layers:

| Layer | Defined by | Contents | Lifespan |
|---|---|---|---|
| Core | ARM | CPU, instruction set, exception model, debug architecture | Very long — a frozen architectural contract |
| Vendor | TI / Luminary | Peripheral inventory, IRQ numbering, SDK, IDE, flash utility | Dies with the product line |

The toolchain targets the core. `-mcpu=cortex-m3` in a current GCC produces the same Thumb-2
encoding it did in 2010, because ARMv7-M is a fixed specification. Only the vendor layer
decayed. The productive question is which layer is unsupported: the generic one or the vendor
one. Almost always it is the vendor layer, and that layer is the smaller, more mechanical piece
— transcribe addresses and bit fields from a PDF. Datasheets are the real dependency, not SDKs.
