# Toolchain and dependencies

Use the official Arm GNU Toolchain with newlib. Configure and build use local pinned dependencies and never acquire sources. Run acquisition only as an explicit separate operation.

## Find the contract

- [Compiler and startup contract](#compiler-and-startup-contract)
- [Installing the Arm toolchain](#installing-the-arm-toolchain)
- [Pinned dependency setup](#pinned-dependency-setup)


## Compiler and startup contract

The build targets ARMv7-M with the official Arm GNU Toolchain, newlib-nano, and a hand-written
startup path, with no Docker and no vendor SDK.

#### The official Arm GNU Toolchain, not the Homebrew formula

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
breaks non-interactive agent sessions and CI. Use the [no-sudo tarball
procedure](#installing-the-arm-toolchain) below.

#### Installing the Arm toolchain

This procedure targets Apple Silicon macOS. Download and install only with separate
approval for network and installation access. No `sudo` is needed:

```bash
TC_DIR="$HOME/.toolchains"
ARCHIVE="arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi.tar.xz"
URL="https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/$ARCHIVE"

mkdir -p "$TC_DIR" && cd "$TC_DIR"
curl -sL --fail -o "$ARCHIVE" "$URL"
tar -xf "$ARCHIVE" && rm -f "$ARCHIVE"
```

The toolchain installs to
`$HOME/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/`.
The extracted directory is about 1.0 GB. CMake finds this location automatically.
For shell commands such as the [validation probes](#validating-the-toolchain),
add its `bin` directory to the current shell's PATH:

```bash
export PATH="$HOME/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin:$PATH"
```

For a different location, pass
`-DLM3S6965_TOOLCHAIN_BIN_HINTS=/path/to/toolchain/bin` when configuring CMake.
Return to the repository root before running the README's build commands or
[pinned dependency setup](#pinned-dependency-setup).

#### Validating the toolchain

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

#### Native macOS, no Docker

The toolchain runs natively on macOS. The project does not use a container. Docker could not
host the whole workflow in any case: USB device passthrough is a Linux-host-only capability, so
on macOS, where Docker Desktop runs a Linux VM, a container cannot see the FTDI debug probe and
OpenOCD inside it can never flash the board. A hybrid split would have been the only option.
The trade-off is losing toolchain pinning via a container image, mitigated by pinning the exact
version in the README and by CMake failing loudly on the wrong one.

#### Hand-written register headers, not a vendored SDK

The vendor driver libraries (StellarisWare, TivaWare) are abandoned, Windows-oriented in their
packaging, large, and ambiguous in licence. This project instead writes minimal register headers
from the datasheet. They stay small and auditable and avoid a dead dependency. The memory map
was cross-checked against QEMU's `hw/arm/stellaris.c`, which cites the LM3S6965 datasheet
(rev I) inline, so the transcription has independent corroboration.

#### `-nostartfiles` and the newlib startup contract

Writing a vector table and linker script means replacing newlib's startup contract. Three link
failures follow, in order:

1. **`undefined reference to _sidata`**. The linker script used `_etext` as the `.data` load
   address but never defined `_sidata`. Fix: let the linker compute it with
   `_sidata = LOADADDR(.data);` and place `.data` with `> SRAM AT> FLASH`.

2. **`section .init LMA overlaps section .data LMA`**. Newlib's `crti.o` and `crtn.o`
   contribute `.init` and `.fini`, which land where the custom script put `.data`'s load
   address. (`crt0.o` has neither. It supplies `_start`, which calls `main`.) Fix:
   `-nostartfiles` drops all three, and the custom reset path supplies their role.

3. **`undefined reference to _init`**. `-nostartfiles` also removed `crti.o`, where `_init`
   lives, and `__libc_init_array()` calls it. Fix: define an empty `_init()`.

Keeping `__libc_init_array()` (with the empty `_init` and the `.init_array` output section)
preserves C++ static constructors and `__attribute__((constructor))` for about four lines of
code.

The general shape: removing a startup object can silently break *other* library code that
assumed it was present. The linker script owns `_sidata`, `_sdata`, `_edata`, `_sbss`, and
`_ebss`. `crti.o` owns `_init`, which `__libc_init_array` calls. Newlib owns `_sbrk` (for
`malloc`) and `_write` (for `printf`).

#### Newlib-nano and `nosys.specs`

`--specs=nano.specs` selects a much smaller `printf`. `--specs=nosys.specs` supplies default
syscall stubs, which `src/syscalls.c` overrides at object-file precedence. `_write` routes
stdout to UART0, so `printf` is the primary diagnostic channel over the serial console. The
cost is about 4.8 KB of flash on a 256 KB part.


## Pinned dependency setup

Run from the parent repository root, with Python 3.9+ and Git installed:

```sh
python3 scripts/setup-dependencies.py --check
python3 scripts/setup-dependencies.py
```

The first command is **offline and read-only**. The second is an explicit,
network-capable acquisition command: it populates **all missing pinned dependencies**,
not just the ones selected by a build target. No Python packages are required.
Configure/build never invokes this helper or fetches sources.

### Pins and source ownership

| Dependency | Approved pin | Acquisition |
|---|---|---|
| FreeRTOS-Kernel V11.3.1 | `3a22924e0a9ddbbc8b0758881c33b3422a5cc20d` | Existing parent gitlink and `.gitmodules` |
| lwIP 2.2.1 (`STABLE-2_2_1_RELEASE`) | `77dcd25a72509eb83f72b033d219b1d40cd8eb95` | Existing parent gitlink and `.gitmodules` |
| Mbed TLS 3.6.7 (`mbedtls-3.6.7`) | Archive SHA256 `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` | Official release asset, checksum verified before extraction |

Dependency sources stay **outside the parent repository's committed source
content**, while remaining locally at `third_party/FreeRTOS-Kernel`,
`third_party/lwip`, and `third_party/mbedtls`. The first two are separate Git
repositories referenced by the existing gitlinks. Mbed TLS is an ignored local
release extraction, not a new submodule or gitlink. Existing local source is
retained in place. This setup does not move, delete, patch, reset, or replace it.

Future parent commits should include only project-owned script/configuration,
tests, guide, provenance and ignore rules, plus the existing gitlinks. **not
bulk dependency source**. Do not force-add `third_party/mbedtls` or create a gitlink
for it. This increment does not change `.gitmodules`, the existing gitlink pins,
or the Mbed TLS ignore rule.

### New clone and explicit acquisition

```sh
git clone <parent-repository-url> LM3S6965-IQC50-A2-arm-cortex-m3
cd LM3S6965-IQC50-A2-arm-cortex-m3
python3 scripts/setup-dependencies.py
python3 scripts/setup-dependencies.py --check
```

The helper verifies Git's selected top-level is the intended parent root and
validates both parent **HEAD and index** gitlinks plus `.gitmodules` URLs before
installing. Existing submodules must select their own exact top-level and pinned
HEAD. Git calls discard inherited repository/worktree/index/object redirection
and `GIT_CONFIG*` environment injection before the first command. Ordinary
`HOME`/XDG global configuration, credential helpers, SSH and proxy transport are
preserved. This is not a global Git configuration sandbox. A local URL lookup
accepts only exit 1 as missing. Configuration and subprocess errors fail preflight. Missing FreeRTOS/lwIP checkouts are initialized with the equivalent
of this explicit Git command (not a moving branch/tag update):

```sh
git submodule update --init --checkout -- third_party/FreeRTOS-Kernel third_party/lwip
git -C third_party/FreeRTOS-Kernel rev-parse HEAD
git -C third_party/lwip rev-parse HEAD
```

These raw Git commands are appropriate for a **fresh clone**. Unlike the helper,
`git submodule update` can change an existing checkout. Do not use it as a repair
command over local work. Verify the exact commits in the table. No recursive
upstream dependency fetch is necessary for these project consumers.

The helper separately downloads this approved official artifact:

https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2

It is **not** GitHub's autogenerated source snapshot or a Git checkout: the
release warns that autogenerated snapshots omit submodule/generated content.
Keeping an official archive rather than adding another submodule preserves the
approved artifact and the same principle of keeping vendor source out of the
parent's commits. See [complete Mbed TLS provenance, licenses and authenticity
limits](../third_party/mbedtls-provenance.md). Its original license and evidence
URLs remain authoritative and unchanged.

#### Local official archive

For offline archive acquisition, supply the exact official file:

```sh
python3 scripts/setup-dependencies.py --mbedtls-archive /path/to/mbedtls-3.6.7.tar.bz2
```

The mandatory SHA256 is identical to network acquisition. This is a fully offline
install only when both submodules are already populated (or Git has an explicitly
configured reachable local origin/cache). Missing submodules still require their
Git origins. If Mbed TLS is already present, the supplied archive is not read and
does **not** authenticate that existing tree. `--check` cannot be combined with
`--mbedtls-archive`.

### Preservation, failures and status meaning

1. The helper preflights all existing destinations and metadata before any
   download/submodule update. Matching submodules must have the exact HEAD and
   no tracked modifications, untracked or ignored files. Wrong pins, URL overrides,
   unrecognized/non-directory/symlink destinations, and incomplete/wrong-version
   Mbed TLS trees are refused without changing them. An existing empty Mbed TLS
   directory is also refused. Inspect the reported path manually. Save your work
   before deciding how to repair it. There is no `--force` or automatic cleanup.
2. If Mbed TLS is missing, checksum verification, safe extraction and version/module
   checks finish in an owned temporary directory **before** submodule updates.
   Extraction rejects absolute/traversal paths, symlinks/hardlinks, special members,
   duplicate normalized paths and files outside the expected release root.
   The helper cleans only its own staging directory, including on failure.
3. Acquisition is not transactional across Git repositories. A Git/network failure
   may leave a newly installed earlier submodule or a partial Git initialization.
   these are retained for manual inspection, never destructively rolled back.
   A missing checkout with a retained `.git/modules/...` repository is refused.
   the helper will not silently reuse/reset that clone. Do not run concurrent setup
   or edit destinations during setup. This helper assumes a stable, trusted workspace
   (including ordinary Git configuration). It makes no hostile concurrent filesystem
   safety guarantee. Git's own initialization metadata changes
   are expected for missing submodules, but the parent index is never staged.
4. `--check` exits **0** when all expected checkouts/version/modules are present,
   **1** when some are missing, and **2** on refusal/error. It performs no fetch,
   download or archive extraction and sets `GIT_OPTIONAL_LOCKS=0` for inspection.
   Exact submodule HEAD plus a clean working tree is pin/status evidence, **not an
   independent source-content authenticity audit**. Mbed TLS version macros and
   required modules establish compatibility checks only. They **do not authenticate
   existing source content offline**. No receipt or version string is treated as
   an integrity proof.

A new Mbed TLS installation is extracted from bytes matching the approved
SHA256. The checksum agrees with official HTTPS release metadata. It is not a
verified detached maintainer signature or independently authenticated supply-chain
attestation. Installation does not establish current security/support, target
resource fit, MCU execution or hardware acceptance.
