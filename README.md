# LM3S6965 cross-compilation cheatsheet

Build firmware for the TI EK-LM3S6965 without a vendor IDE or Docker.
The Cortex-M3 has 256 KiB flash, 64 KiB SRAM and a nominal 50 MHz system clock.
The host workflow targets Apple Silicon macOS with the official Arm GNU
Toolchain, newlib, CMake, QEMU and OpenOCD. The BSP reconstructs the peripheral
layer from the datasheet rather than depending on retired vendor tooling.

The [manual](docs/README.md) holds the contracts and detailed procedures.
Commands below assume the repository root. They are usage instructions, not
claims about the image installed on a board or permission for live operations.

## Packages and installation

| Package | Purpose |
|---|---|
| Official Arm GNU Toolchain 15.3.rel1 | GCC, newlib, binutils and GDB |
| CMake 3.20+ and Make | Configure and build, including A/B presets |
| QEMU | Offline `lm3s6965evb` model |
| OpenOCD, libusb, libftdi, libusb-compat | ICDI debug probe access |
| Python 3 and OpenSSL | Dependency setup, tooling and authorized signing |
| picocom and optional pyftdi | UART adapter and ICDI SWO console |

On macOS, install the host tools and the official toolchain cask:

```sh
brew install cmake qemu open-ocd libusb libftdi libusb-compat picocom openssl
brew install --cask gcc-arm-embedded
python3 -m venv .venv
.venv/bin/pip install pyftdi
```

Do **not** use the Homebrew `arm-none-eabi-gcc` formula: it lacks newlib.
The [toolchain chapter](docs/toolchain.md) gives the no-sudo tarball alternative,
search paths, exact dependency pins and offline archive setup. For a different
installed compiler, configure with
`-DLM3S6965_TOOLCHAIN_BIN_HINTS=/absolute/path/to/toolchain/bin`.

Populate pinned dependencies explicitly before configuring:

```sh
python3 scripts/setup-dependencies.py
python3 scripts/setup-dependencies.py --check
```

Setup can access the network. Configure and build never fetch dependencies.
Use only separately approved installation and dependency access.

## Bare-metal and FreeRTOS build

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
cmake --build build --target lm3s6965_firmware lm3s6965_freertos_firmware
```

| Application | ELF path under `build/` |
|---|---|
| Bare-metal OLED, switches, LED and console | `examples/baremetal/lm3s6965_firmware` |
| FreeRTOS application | `examples/freertos/lm3s6965_freertos_firmware` |

Each target also emits `.bin`, `.hex` and `.map` siblings. GDB and QEMU use the
ELF without a suffix. See [applications and emulation](docs/applications.md)
for FreeRTOS configuration and QEMU commands.

### Flash one application

**Programming erases flash and overwrites installed firmware without a backup.**
Ordinary application images are not A/B slot packages. Do not use this path to
update a provisioned A/B system unless deliberately replacing its boot setup.
Obtain separate approval for probe access, halt, programming and reset.

```sh
scripts/check-connection.sh
scripts/probe.sh
scripts/flash.sh build/examples/baremetal/lm3s6965_firmware
```

`check-connection.sh` checks host USB enumeration, not working probe access or
flash programming, regardless of its optimistic verdict.
`probe.sh` resets and halts the core, reads identity/vectors/flash geometry, then
resumes it without flash programming. It is not passive observation.
`flash.sh` programs, verifies and resets through the repository's board config
for the `0403:bcd9` FTDI ICDI. To select FreeRTOS instead:

```sh
scripts/flash.sh build/examples/freertos/lm3s6965_freertos_firmware
```

For console capture and reset sequencing, use [board bring-up](docs/bring-up.md).
The ICDI console is SWO over raw USB, not a macOS serial device:

```sh
scripts/icdi-console.sh
```

Use `scripts/console.sh` for a separate UART0 serial adapter. The banner prints
once after reset, so follow the manual's reader-before-reset procedure.

### Restart the installed firmware (no programming)

With separate authorization for a live reset, run with **no arguments**:

```sh
scripts/board_software_reset.sh
```

This requests SYSRESETREQ and `reset run` through the board's OpenOCD config to
reset/restart the current firmware; it does not program flash. OpenOCD target
reset hooks can govern the actual reset sequence. `-h` or `--help` alone prints
help without accessing hardware and exits successfully. Unsupported arguments,
including `--dry-run` and extra arguments, are rejected with exit status 2 before
hardware access.
Start any console reader before resetting; use the coordinated capture procedures
in [board bring-up](docs/bring-up.md#console-paths-and-capture) for SWO capture.

## A/B boot build and flash

A/B has three separate artifacts: a dispatch bootloader with a selected public
root, signed slot applications, and authenticated initial metadata. Building or
flashing only the bootloader does **not** bootstrap a blank board.
See [update and boot images](docs/boot-reset-image.md) and
[signed packages and factory metadata](docs/boot-package.md).

### Configure an identity and build

**Existing identity first:** retain the approved external key pair; do not run
key setup for a rebuild or an existing board. Restore or deliberately configure
a working ignored root `.env` pointing to that pair. Both build wrappers require
`.env`; `.env.bak` alone is not consumed, even with a public-key argument.
Preserve backups and do not overwrite a working config. Store paths, not private
key bytes. The package wrapper requires exactly:

- `LM3S6965_BOOT_PUBLIC_KEY_FILE`: raw 65-byte uncompressed P-256 public file.
- `LM3S6965_BOOT_PRIVATE_KEY_FILE`: absolute external private-key path, not bytes.
- `LM3S6965_BOOT_RELEASE_A` and `LM3S6965_BOOT_RELEASE_B`: decimal uint32 releases.

Values are literal, never shell-sourced or inherited from the environment.
Review releases before signing. Defaults 1/2 are not uniqueness or anti-downgrade
policy. See [identity setup and wrapper contract](docs/boot-package.md#local-identity-and-build-wrappers)
for parser limits, failure recovery and direct unsigned CMake commands.

After reviewing the existing identity and releases, build with:

```sh
scripts/build_ab_boot_demo
scripts/build_boot_dispatch
```

**Alternative: new, separately authorized local identity only**, with no existing
`.env` or key pair at the selected location:

```sh
python3 scripts/setup-boot-demo-keys.py --init-env
```

Then review `.env` before using the build commands above. Setup refuses existing
`.env` and key files; it creates external keys with private file mode 0600 and
parent mode 0700. Do not regenerate a lost key: recover its backup or plan an
explicit identity migration. A new public root requires a matching bootloader
on the device. FTP cannot rotate that root.

The package wrapper builds only FTP A/B applications, forces diagnostics OFF,
and signs their exact binaries using the existing packager. It publishes
`slot-a.pkg` and `slot-b.pkg` in a new
`build/ab-demo-packages/release-<A>-<B>/`, refusing an existing directory.
If it already exists, leave it intact: review and approve new release values in
`.env`, or explicitly preserve the old artifacts at a separate archival path
before rebuilding the same pair. Do not automatically overwrite or delete them;
reusing release numbers does not establish anti-downgrade protection.
The dispatch wrapper separately builds `build/src/lm3s6965_boot_dispatch`.
Neither wrapper flashes a device. Existing build artifacts and unrelated cache
settings are retained. Resolve incompatible generator/toolchain caches manually.

**Programming gate:** an A/B build produced unresolved linker warnings about
variable-size enums mixed with 32-bit enums (possible cross-object ABI mismatch).
Successful linking and signing do not validate ABI compatibility. Stop before
programming these new images until the warnings have been reviewed; they do not
by themselves prove corrupted layout or require rebuilding the toolchain.

### Bootstrap versus an existing confirmed application

**Blank-board/bootstrap:** first plan the matching bootloader, authenticated
slot payloads/manifests and both metadata pages using the manual. There is no
single blank-board provisioning shortcut here. The following command writes
only the built bootloader and erases its programmed flash region. It does not
install applications or initialize metadata, and may make an existing system
unbootable if its root no longer matches:

```sh
scripts/flash.sh build/src/lm3s6965_boot_dispatch
```

`scripts/ab-boot-provision-recovery.sh` is a separately authorized one-slot
debug programmer, **not** a complete bootstrap or automatic FTP fallback.
It preserves bootloader, public root and metadata.

**Existing confirmed production FTP app:** only after it serves `boot-status`
and reports a permitted update slot, use the signed pair on an approved isolated
bench. Replace the example directory with the exact published release path:

```sh
scripts/ab-boot-firmwareflash-ftp.sh status
scripts/ab-boot-firmwareflash-ftp.sh download build/ab-demo-packages/release-1-2
```

The client defaults to `192.168.7.2:21`. It uploads only the permitted slot,
prompts for manual reset, checks the running report and healthy trial, then
requests and resolves durable confirmation. At that prompt, a separately
authorized `scripts/board_software_reset.sh` may be run in another terminal;
the FTP client does not invoke it automatically. This describes the workflow,
not evidence of a successful trial. Firmware reception erases the
inactive destination. Transfer completion alone is not update success.
Do not retry firmware after an ambiguous transfer. Inspect read-only status.
LED-only fixtures cannot serve this workflow. See the
[PC workflow](docs/boot-reset-image.md#pc-demo-and-transport) for exact boundaries.

## Source layout

| Path | Responsibility |
|---|---|
| `include/lm3s6965/`, `src/` | BSP, boot policy, storage, FTP libraries and startup |
| `linker/`, `config/`, `cmake/` | Memory ownership, crypto configuration and build rules |
| `examples/`, `freertos/` | Bare-metal, RTOS, Ethernet and A/B consumers |
| `third_party/` | Explicitly populated pinned dependencies |
| `scripts/`, `tools/`, `tests/` | Operational wrappers, offline PC tooling and fixtures |

Start validation with [tools and testing](docs/tools-testing.md) or the
[test launchpad](tests/README.md). Ethernet procedures live in
[scripts/ethernet-diagnostics.md](scripts/ethernet-diagnostics.md).
The consolidated offline suite lists its coverage without running it:

```sh
./scripts/ethernet/test.sh --list
```

## Limitations

- QEMU and native models do not prove physical timing, flash power-loss behavior,
  live IP/FTP, watchdog margin or whole-system acceptance.
- Clock register readback is not measured frequency or end-to-end UART evidence.
  Oscillator startup uses a fixed conservative delay.
- UART0, GPIO, LED, switches, OLED, SysTick and trace are supported, not a full
  vendor peripheral SDK. FreeRTOS uses a separate 32 KiB heap.
- RAW defaults to live traffic and link capture resets the board. Neither live
  path belongs to the consolidated offline runner.
- Documentation bytes participate in boot source identity. This cleanup does
  not renew fingerprints, historical test evidence or installed-image claims.

Read [limitations and proof boundaries](docs/limitations.md) before interpreting
an old result as current acceptance.
