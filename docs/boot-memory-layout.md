# Boot target link-time memory layout

This document covers **link-time reservations, artifact contracts and bounded
`_sbrk` backing**. These increments do not integrate a bootloader, change
Reset_Handler, implement live diagnostic publication, or establish target
runtime resource/timing acceptance.
The partition map remains the approved provisional bench map.

## Approved FTP capacity increment

The user approved **8192 total bytes (+6144 over the former 2048)**, including
collected static `.heap[.*]` inputs; dynamic backing remains
`[_heap_static_end, _heap_end)`. Generic heap policy, production allocators,
64-byte diagnostics, 8192-byte stack and 32768-byte boot flash are unchanged.
Current standalone FTP static RAM is **39984 bytes**; heap
`[0x20009c30,0x2000bc30)` leaves **9104 bytes** below diagnostics. Four root
Debug/Release × painter OFF/ON builds and all **20 ELFs** pass boundary checks.
The Debug standalone FTP flash ends at 32816/32860, Release at 27612/27656;
none is a full 32 KiB boot-fit claim.

Debug and Release each pass **15/15 CTest with ASan/UBSan**, no skips. Real linked
allocator replays now include full 8192-byte static FTP heaps. Synthetic native
and Ethernet allocator tests retaining 2048 are independent capacity fixtures,
not production-budget claims. Historical 2 KiB evidence below remains history.
Isolated full-newlib verification passes at the approved capacity; integrated
network fragmentation/peak, live publication and full acceptance remain pending.
See [boot-resources.md](boot-resources.md); independent review is next.

## Contents

- [Scripts and flash ownership](#scripts-and-flash-ownership)
- [SRAM and startup symbols](#sram-and-startup-symbols)
- [Supported GNU slot producer and packaging](#supported-gnu-slot-producer-and-packaging)
- [Reproducible host gate](#reproducible-host-gate)

## Scripts and flash ownership

All scripts are self-contained: existing absolute/out-of-tree `-T` commands do
not acquire a relative `INCLUDE` dependency. The root example build still uses
its established scripts; no root slot-selection policy is introduced.

| Script | Flash-load interval | Separate reserve |
| --- | --- | --- |
| `linker/bootloader.ld` | `[0, 0x8000)` (32768 bytes) | Metadata `[0x8000, 0x8800)` |
| `linker/app_a.ld` | `[0x8800, 0x23C00)` (111616 bytes) | Manifest `[0x23C00, 0x24000)` |
| `linker/app_b.ld` | `[0x24000, 0x3F400)` (111616 bytes) | Manifest `[0x3F400, 0x3F800)` |
| `linker/lm3s6965.ld` | `[0, 0x40000)` | Standalone, not a boot-slot artifact |
| `examples/ethernet-ftp/lm3s6965_ftp.ld` | `[0, 0x40000)` | Standalone FTP, not a boot-slot artifact |

Standalone artifacts intentionally retain whole-device flash capacity. They are
not safe to program over a provisioned bootloader/A/B installation.

`FLASH` includes all ordinary flash sections (vectors, text, constants, ARM
unwind tables, preinit/init/fini arrays, legacy constructors/destructors), plus
`.data`'s flash **LMA**, not just its SRAM VMA. Region overflow and the payload-end
assertion include that initialized-data load. New boot/slot scripts reject
`.manifest[.*]` and `.metadata[.*]` firmware inputs. Reserves are represented by
symbols, not emitted manifest/metadata sections; neither 1 KiB manifest tail nor
metadata bytes are firmware loads. The packager separately produces the signed
160-byte manifest. Its transport is manifest160 + exact payload, not reserve1024
+ payload, and it is not a directly flash-addressed slot binary.

### Supported allocated inputs — no implicit orphan initialization

All five scripts support only the explicitly matched allocated input sections:

| Input sections | Ownership / initialization |
| --- | --- |
| `.isr_vector`, `.text*`, `.rodata*`, `.init`, `.fini` | Flash vectors, code and constants |
| `.ARM.extab*`, `.ARM.exidx*`, `.preinit_array[.*]`, `.init_array[.*]`, `.fini_array[.*]`, `.ctors[.*]`, `.dtors[.*]` | Flash unwind/constructor content; existing startup constructor contract unchanged |
| `.data*` | SRAM initialized by the single `_sidata` → `[_sdata, _edata)` copy |
| `.bss*`, COMMON | SRAM zeroed only in `[_sbss, _ebss)` |
| `.heap[.*]` | Explicit static heap storage in NOLOAD `.c_heap`; not copied/zeroed by startup |

Diagnostic and stack output sections are linker-created NOLOAD reservations,
not permission to supply arbitrary allocated inputs with those names. The new
boot/slot scripts additionally reject manifest/metadata inputs with their
existing specific diagnostic.

After all supported input rules, GNU ld's
`INPUT_SECTION_FLAGS (SHF_ALLOC) *(*)` catches remaining **retained** allocated
inputs in `.unsupported_allocated`; an assertion requires its size to be zero
with **`unsupported allocated input section`**. This is an input-flag filter,
not a blanket `--orphan-handling=error` policy. The catch precedes
`.comment`/`.note*` discard rules: retained allocated content cannot silently
escape through those rules. Nonallocated unknown metadata/debug sections and
`.ARM.attributes` remain permitted; nonallocated comment/note discard is unchanged.

The engineering GC correction removes only the former broad `KEEP` wrapper
from this catch in all five scripts; it had rooted supported allocated inputs
as well and defeated dead-code elimination. Known vector/constructor `KEEP`s,
the catch and assertion are unchanged. The supported policy is **reject every
retained unknown**: with GC, dead unknown PROGBITS/NOBITS may link only when
verified absent from the ELF. GNU Arm ld 15.3.rel1 retains even dead allocated
NOTE; its assertion rejection is intended, never force-discarded. This changes
the prior reject-even-dead regression oracle, not runtime, crypto, memmap or
startup initialization contracts. See [boot-resources.md](boot-resources.md)
for the exact 120 proofs and the actual final Release probe that supersedes the
initial 37112-byte failed placement.

Unknown file-backed writable SRAM (for example `.extra,"aw",%progbits`),
unknown allocated read-only/executable sections, and unknown SRAM NOBITS inputs
are **unsupported and rejected if retained**, not assigned invented copy/zero semantics.
Use the supported `.data*` or `.bss*` families for normal initialized/zeroed
objects. Adding a new allocated family requires a separately reviewed ownership
and startup contract. Merely placing an orphan inside a MEMORY region does not
initialize it: unchanged startup only copies `.data` and clears `.bss`.

Consequently `_flash_payload_end = LOADADDR(.data) + SIZEOF(.data)` is the exact
last file-backed flash load end for supported content: all supported flash
content precedes that last initialized SRAM load, and later supported SRAM
sections/reservations are NOLOAD. The regression independently compares it with
the actual PT_LOAD file extents; extending this symbol alone would not fix an
uninitialized writable orphan. Startup initialization is unchanged; allocator
backing is bounded separately as described below.

## SRAM and startup symbols

All five scripts split SRAM into these disjoint regions:

| Ownership | Half-open interval |
| --- | --- |
| Ordinary `.data`, `.bss`, static heap and remaining dynamic capacity | `[0x20000000, 0x2000DFC0)` |
| `.boot_diagnostic`, 64 bytes, **NOLOAD / SHT_NOBITS** | `[0x2000DFC0, 0x2000E000)` |
| `.stack`, 8192 bytes, **NOLOAD / SHT_NOBITS** | `[0x2000E000, 0x20010000)` |

- `_sidata` is the `.data` flash LMA; `_sdata`/`_edata` delimit only the SRAM copy
  range. `_sbss`/`_ebss` delimit only the `.bss` zero range.
- `_end` and `_heap_start` identify the aligned lower-SRAM heap base.
  `.heap`/`.heap.*` test/static heap inputs are NOLOAD in `.c_heap`;
  `_heap_static_end` identifies their end. Generic/boot/slot `_heap_end` is
  `0x2000DFC0`, preserving all remaining lower-SRAM dynamic capacity rather than
  silently adopting a new 2 KiB limit. Explicit static heap inputs must be
  excluded from dynamic `_sbrk` capacity: dynamic allocation begins at
  `_heap_static_end`, never reusing their storage.
- User-approved FTP now reserves exactly **8192 bytes total** in `.c_heap` between `_heap_start` and
  `_heap_end`. Static heap inputs must fit inside that same fixed reserve.
  `ftp_sbrk.c` uses only the remaining `[_heap_static_end, _heap_end)` capacity.
- `_diagnostic_start`/`_diagnostic_end` are `0x2000DFC0`/`0x2000E000`.
  `_stack_limit`/`_estack` are `0x2000E000`/`0x20010000`. Assertions and lower-SRAM
  region limits reject static data/bss/heap overlap with diagnostics. The
  existing optional FTP painter still paints only `_stack_limit <= p < _estack`.
- `_vectors_start`/`_vectors_end` delimit a separate `.isr_vector` at the flash
  origin, aligned to at least 256 bytes and containing at least 236 bytes (59
  words), with a word-multiple size. `_flash_payload_start`,
  `_flash_payload_limit`, and `_flash_payload_end` identify the flash interval
  and actual initialized-load end. Slots additionally export `_manifest_start`
  and `_manifest_end`; boot exports `_metadata_start` and `_metadata_end`.

GNU ld assertions cannot inspect the actual first vector word. `_estack` is
asserted exact, and the unchanged BSP startup uses that symbol, but arbitrary
producer vector contents must still be inspected. The packager rejects actual
MSP bytes other than `0x20010000` and requires a Thumb reset target inside the
payload. An ELF entry point is not a replacement for validating those vectors.

### Bounded allocator backing — resource acceptance remains pending

Generic `src/syscalls.c` and standalone FTP `ftp_sbrk.c` retain the API
`void *_sbrk(ptrdiff_t increment)`. Both initialize the break at
`_heap_static_end` and enforce the inclusive break interval
`[_heap_static_end, _heap_end]`, with allocated bytes strictly below `_heap_end`.
The old generic `_estack - 1024` guard is removed. Static `.heap[.*]` storage
is never dynamically reused, including when it consumes all available capacity.
The five linker contracts require `_heap_start <= _heap_static_end <= _heap_end`;
the allocator also rejects invalid ordering with `ENOMEM` before subtraction.

A zero increment returns the current break. Successful growth/shrink returns
the previous break without changing errno; exact-end growth succeeds. Overflow,
underflow and invalid bounds return `(void *)-1`, set `errno = ENOMEM`, and do
not change the break. Integer-address subtraction establishes available capacity
before addition/subtraction; negative magnitude handles `PTRDIFF_MIN` without
signed negation overflow. No live stack pointer or unrelated-pointer arithmetic
is used. The contract remains foreground/nonconcurrent, not an ISR/thread-safe
allocator or an initialized/zeroed static-heap-storage promise.

ABI evidence is the installed official **Arm GNU Toolchain 15.3.rel1** newlib
`arm-none-eabi/include/sys/unistd.h:281` (`_sbrk(ptrdiff_t)`) and
`arm-none-eabi/include/reent.h:154` (`_sbrk_r(struct _reent *, ptrdiff_t)`).
`tests/boot/sbrk_abi.c` compiles against those headers and verifies that this
Cortex-M3 target's `ptrdiff_t` is `int`; neither signature is guessed or changed.

`boot_allocator` tests both complete production units with renamed syscalls and
fixed synthetic linker symbols, keeping strict warnings and ASan/UBSan. Cases
cover zero address, native signed min/max, near-address-wrap bounds, empty,
nonempty/full static heap, exact boundaries, rejected growth/shrink, errno and
unchanged-break semantics, and two invalid bound orderings. Fifteen real GNU Arm
fixture links (all five scripts × empty/nonempty/full static heap) independently
check ELF symbols against expected addresses and replay those linked heap bounds
through the native allocator. Allocating diagnostics or stack bytes is rejected;
allowing the break itself at the diagnostic start grants no byte there.

Implementation-side validation: Debug and Release each pass **12/12 CTest**
with ASan/UBSan; package **20/20** and layout **13/13**, no skips. Existing
Ethernet platform tests pass. Four root cross-builds (Debug/Release × FTP painter
OFF/ON, FTP enabled) and all **20 example ELF** memory/reset-vector checks pass.
The existing authorized OpenSSL package workflow uses the external demo key;
this increment introduces no private-key copying or output. Independent review
is the next gate.

These tests establish backing bounds, **not peak/live allocator usage, stack
high-water margin, final integrated 32 KiB boot fit or watchdog timing**. The
separate [verifier resource probe](boot-resources.md) now has an accepted actual
Release ELF (25476 flash bytes, 7292-byte margin); it is not an integrated
bootloader. Runtime resource measurements remain pending. A stack overflow can
still overwrite diagnostics.

## Supported GNU slot producer and packaging

Use the official GNU Arm toolchain with the selected slot script:

```sh
arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -mfloat-abi=soft \
  -nostdlib -T /absolute/project/linker/app_a.ld \
  -Wl,-z,max-page-size=0x100 \
  /absolute/project/tests/boot/layout_fixture.S -o /tmp/slot-a.elf
arm-none-eabi-objcopy --gap-fill=0xff -O binary /tmp/slot-a.elf /tmp/slot-a.bin
```

For B use `app_b.ld`. Real applications supply their usual startup/libraries in
place of the fixture; the slot page-size option remains required. GNU's default
4 KiB page size can put A's header-bearing PT_LOAD at `0x8000`, outside the
payload. Do not loosen packager bounds to accept that producer. Excessive input
section alignment can also change PT_LOAD layout; validate every final ELF.

The layout fixture links with `-z,max-page-size=0x100`; both A and B pass the
existing packager's ELF and raw canonical-payload paths. Separate explicit-LMA
gap fixtures verify GNU objcopy `0xFF` gap filling byte-for-byte against that
packager. Normal default `AT> FLASH` placement is also checked. The existing
`boot_package` suite supplies external-key signing/OpenSSL/C-verifier coverage;
this layout suite adds no key handling or dependency.

## Reproducible host gate

```sh
cmake -S tests/boot -B /tmp/lm3s-layout-host-debug \
  -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build /tmp/lm3s-layout-host-debug -j8
ctest --test-dir /tmp/lm3s-layout-host-debug --output-on-failure
```

Repeat with `Release` and a different build directory. The current host suite
has eighteen CTest entries, including `boot_allocator` and `boot_diag`.
At the earlier fourteen-entry gate, Debug and Release each passed 14/14 with
native ASan/UBSan. `boot_resources` fresh-cross-links the Release Arm fixture
and runs four artifact/parser tests without executing target code.
`boot_layout` runs thirteen Python tests with
real GNU Arm links, no tool-missing skips. `BOOT_ARM_GCC` may select an absolute
compiler path; otherwise official installed bundles are preferred over PATH.
No compiler/dependency is downloaded by the test.

The layout tests check all five scripts' exact symbols, section types/ranges,
PT_LOAD VMAs/LMAs/file spans, constructor loads, initialized data, nonloaded
SRAM, and GNU objcopy output. Negative links cover flash text overflow,
initialized-data LMA and constructor overflow, bss/static-heap overflow into
diagnostics, the FTP fixed-heap boundary (8192 accepted with no dynamic capacity,
8200 rejected),
undersized/misaligned vectors and forbidden manifest/metadata inputs. Negative
fixtures require a specific expected linker diagnostic, not just any link failure.
Allocated orphan regressions now cover six classes (writable/read-only/executable
PROGBITS, writable NOBITS, allocated NOTE and allocated `.comment` PROGBITS)
across all five scripts: rooted `main` references with GC, `-u unknown` with GC,
no GC, and dead GC. All **120/120** satisfy the retained-only property: **90**
live/rooted/no-GC intended assertion failures, **25** dead PROGBITS/NOBITS links
with section/symbol absence verified in final ELFs, and **5** dead allocated
NOTE intended assertion failures because GNU ld retains them. A successful link
is not permitted merely because an input was nominally unreferenced. Additional
positive fixtures preserve nonallocated custom metadata, debug and ARM attributes
and still pass exact PT_LOAD/objcopy checks.
Wrong actual MSP
bytes are rejected by artifact validation, not falsely claimed as a ld check.

### P2 orphan regression evidence and validation (historical reject-even-dead)

The original evidence below describes the former broad-KEEP implementation.
The minimal GC correction above supersedes its 50-failure oracle with the
120-case retained-only proof. Revalidation uses
`build/resource-host-{debug,release}` and
`/tmp/lm3s-gc-root-{Debug,Release}-{OFF,ON}`: host **14/14**, package **20/20**,
layout **13/13**, resources **4/4**, four root FTP configurations and all
**20 final example ELF** memory/vector checks pass. No runtime execution or
memory-map/crypto weakening was used; independent review is next.

Regression-first: before changing any linker script, all 25 new negative
fixtures linked successfully (five allocated section kinds × five scripts;
allocated notes were silently discarded). For the 16-byte writable `.extra`,
all scripts placed its VMA at `0x20000010`, outside the unchanged copy range
`[0x20000000, 0x20000010)`. Advertised versus actual file-backed PT_LOAD ends:
bootloader/standalone/FTP `0x12C` versus `0x13C`, A `0x892C` versus `0x893C`,
B `0x2412C` versus `0x2413C`. After the fix all 50 negative variants, including
`--gc-sections`, fail with the intended unsupported-input assertion.

Revalidated using host directories `/tmp/lm3s-orphan-host-{Debug,Release}` and
root cross-build directories `/tmp/lm3s-orphan-root-{Debug,Release}-{OFF,ON}`.
GNU fixture compiler: official Arm GNU Toolchain **15.3.rel1**. Independent
review remains the next gate; these are implementation-side checks.

Validation for this fix: Debug and Release each pass **10/10 CTest** with
host ASan/UBSan; existing package tests **20/20** and layout tests **13/13**, no
skips. Authorized package tests invoke OpenSSL with the existing external demo
key through the established workflow; this fix adds no private-key handling. Root cross-builds in Debug/Release, with FTP stack instrumentation OFF
and ON, all link existing examples. All **20 example ELFs** were also checked
for exact MSP, diagnostic/stack NOBITS reservations, lower-SRAM data/bss/heap
bounds, the FTP 2048-byte heap, and the correct normal versus painter reset
vector. These small fixture and example links do
**not** prove that a complete verifier/bootloader fits 32 KiB, that an updater
fits either slot, that runtime heap/stack is adequate, that reset retains SRAM,
or that watchdog/crypto/flash/network/handoff timing is safe. Hardware and all
of those memory/timing measurements remain unverified.
