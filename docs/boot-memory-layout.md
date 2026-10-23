# Boot memory layout and resource validation

Flash partitions, NOLOAD reservations and bounded allocator backing are ownership contracts. Link fit is not peak runtime usage. Boot/A/B reserve continuity separately from diagnostics and stack. Standalone FTP reserves 8192 total heap bytes, including static heap inputs.

## Find the contract

- [Flash and SRAM ownership](#flash-and-sram-ownership)
- [Slot producer](#slot-producer)
- [Layout validation](#layout-validation)
- [Final-artifact resource gate](#final-artifact-resource-gate)
- [Allocator and verifier fixture](#allocator-and-verifier-fixture)
- [Resource instrumentation](#resource-instrumentation)
- [Resource runner evidence rules](#resource-runner-evidence-rules)


## Flash and SRAM ownership

### Scripts and flash ownership

All scripts are self-contained: existing absolute/out-of-tree `-T` commands do
not acquire a relative `INCLUDE` dependency. The root example build still uses
its established scripts. No root slot-selection policy is introduced.

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
symbols, not emitted manifest/metadata sections. Neither 1 KiB manifest tail nor
metadata bytes are firmware loads. The packager separately produces the signed
160-byte manifest. Its transport is manifest160 + exact payload, not reserve1024
+ payload, and it is not a directly flash-addressed slot binary.

#### Supported allocated inputs. No implicit orphan initialization

All five scripts support only the explicitly matched allocated input sections:

| Input sections | Ownership / initialization |
| --- | --- |
| `.isr_vector`, `.text*`, `.rodata*`, `.init`, `.fini` | Flash vectors, code and constants |
| `.ARM.extab*`, `.ARM.exidx*`, `.preinit_array[.*]`, `.init_array[.*]`, `.fini_array[.*]`, `.ctors[.*]`, `.dtors[.*]` | Flash unwind/constructor content. Existing startup constructor contract unchanged |
| `.data*` | SRAM initialized by the single `_sidata` → `[_sdata, _edata)` copy |
| `.bss*`, COMMON | SRAM zeroed only in `[_sbss, _ebss)` |
| `.heap[.*]` | Explicit static heap storage in NOLOAD `.c_heap`. Not copied/zeroed by startup |

Continuity (boot/A/B only), diagnostic and stack output sections are linker-created NOLOAD reservations,
not permission to supply arbitrary allocated inputs with those names. The new
boot/slot scripts additionally reject manifest/metadata inputs with their
existing specific diagnostic.

After all supported input rules, GNU ld's
`INPUT_SECTION_FLAGS (SHF_ALLOC) *(*)` catches remaining **retained** allocated
inputs in `.unsupported_allocated`. An assertion requires its size to be zero
with **`unsupported allocated input section`**. This is an input-flag filter,
not a blanket `--orphan-handling=error` policy. The catch precedes
`.comment`/`.note*` discard rules: retained allocated content cannot silently
escape through those rules. Nonallocated unknown metadata/debug sections and
`.ARM.attributes` remain permitted. Nonallocated comment/note discard is unchanged.

The engineering GC correction removes only the former broad `KEEP` wrapper
from this catch in all five scripts. It had rooted supported allocated inputs
as well and defeated dead-code elimination. Known vector/constructor `KEEP`s,
the catch and assertion are unchanged. The supported policy is **reject every
retained unknown**: with GC, dead unknown PROGBITS/NOBITS may link only when
verified absent from the ELF. GNU Arm ld 15.3.rel1 retains even dead allocated
NOTE. Its assertion rejection is intended, never force-discarded. This changes
the prior reject-even-dead regression oracle, not runtime, crypto, memmap or
startup initialization contracts. Use the [layout validation](#layout-validation)
and [final-artifact resource gate](#final-artifact-resource-gate) to check actual
retention and flash extents rather than a failed placement estimate.

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
the actual PT_LOAD file extents. Extending this symbol alone would not fix an
uninitialized writable orphan. Startup initialization is unchanged. Allocator
backing is bounded separately as described below.

### SRAM and startup symbols

Bootloader/A/B scripts now reserve continuity. Standalone/generic and standalone
FTP scripts retain their prior ownership (they must not link the new entry path):

| Ownership | Half-open interval |
| --- | --- |
| Boot/A/B ordinary `.data`, `.bss`, static heap and dynamic capacity | `[0x20000000, 0x2000DBC0)` |
| Boot/A/B `.boot_continuity`, 1024 bytes, **NOLOAD / SHT_NOBITS** | `[0x2000DBC0, 0x2000DFC0)` |
| Standalone ordinary SRAM (FTP heap separately fixed at 8192 total) | `[0x20000000, 0x2000DFC0)` |
| `.boot_diagnostic`, 64 bytes, **NOLOAD / SHT_NOBITS** | `[0x2000DFC0, 0x2000E000)` |
| `.stack`, 8192 bytes, **NOLOAD / SHT_NOBITS** | `[0x2000E000, 0x20010000)` |

- `_sidata` is the `.data` flash LMA. `_sdata`/`_edata` delimit only the SRAM copy
  range. `_sbss`/`_ebss` delimit only the `.bss` zero range.
- `_end` and `_heap_start` identify the aligned lower-SRAM heap base.
  `.heap`/`.heap.*` test/static heap inputs are NOLOAD in `.c_heap`.
  `_heap_static_end` identifies their end. Boot/slot `_heap_end` is
  `_continuity_start = 0x2000DBC0`. Generic standalone remains `0x2000DFC0`,
  preserving all remaining lower-SRAM dynamic capacity rather than
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
  and `_manifest_end`. Boot exports `_metadata_start` and `_metadata_end`.

The linked allocator replay independently pins its fixture heap start to
`0x20000040`: maximum static heap is `0xDB80` for boot/A/B, `0xDF80` for generic
standalone, and 8192 for FTP. It replays empty/nonempty/exactly-full static heaps
without deriving expected bounds from allocator or ELF output. Boot/A/B also
check the exact 1024-byte NOLOAD continuity reservation and reject an eight-byte
static-heap spill into it. A smaller boot heap is intentional ownership, not
permission to relax either allocator bounds or the diagnostic/stack endpoints.

GNU ld assertions cannot inspect the actual first vector word. `_estack` is
asserted exact, and the unchanged BSP startup uses that symbol, but arbitrary
producer vector contents must still be inspected. The packager rejects actual
MSP bytes other than `0x20010000` and requires a Thumb reset target inside the
payload. An ELF entry point is not a replacement for validating those vectors.

#### Bounded allocator backing. Resource acceptance remains pending

Generic `src/syscalls.c` and standalone FTP `ftp_sbrk.c` retain the API
`void *_sbrk(ptrdiff_t increment)`. Both initialize the break at
`_heap_static_end` and enforce the inclusive break interval
`[_heap_static_end, _heap_end]`, with allocated bytes strictly below `_heap_end`.
The old generic `_estack - 1024` guard is removed. Static `.heap[.*]` storage
is never dynamically reused, including when it consumes all available capacity.
The five linker contracts require `_heap_start <= _heap_static_end <= _heap_end`.
the allocator also rejects invalid ordering with `ENOMEM` before subtraction.

A zero increment returns the current break. Successful growth/shrink returns
the previous break without changing errno. Exact-end growth succeeds. Overflow,
underflow and invalid bounds return `(void *)-1`, set `errno = ENOMEM`, and do
not change the break. Integer-address subtraction establishes available capacity
before addition/subtraction. Negative magnitude handles `PTRDIFF_MIN` without
signed negation overflow. No live stack pointer or unrelated-pointer arithmetic
is used. The contract remains foreground/nonconcurrent, not an ISR/thread-safe
allocator or an initialized/zeroed static-heap-storage promise.

ABI evidence is the installed official **Arm GNU Toolchain 15.3.rel1** newlib
`arm-none-eabi/include/sys/unistd.h:281` (`_sbrk(ptrdiff_t)`) and
`arm-none-eabi/include/reent.h:154` (`_sbrk_r(struct _reent *, ptrdiff_t)`).
`tests/boot/sbrk_abi.c` compiles against those headers and verifies that this
Cortex-M3 target's `ptrdiff_t` is `int`. Neither signature is guessed or changed.

`boot_allocator` tests both complete production units with renamed syscalls and
fixed synthetic linker symbols, keeping strict warnings and ASan/UBSan. Cases
cover zero address, native signed min/max, near-address-wrap bounds, empty,
nonempty/full static heap, exact boundaries, rejected growth/shrink, errno and
unchanged-break semantics, and two invalid bound orderings. Fifteen real GNU Arm
fixture links (all five scripts × empty/nonempty/full static heap) independently
check ELF symbols against expected addresses and replay those linked heap bounds
through the native allocator. Allocating diagnostics or stack bytes is rejected.
allowing the break itself at the diagnostic start grants no byte there.

These tests check backing bounds, not peak/live allocator usage, stack
high-water margin, integrated boot fit or watchdog timing. A stack overflow can
still overwrite diagnostics. Validate the final linked consumer and its workload
separately from the isolated verifier probe.


## Slot producer

### Supported GNU slot producer and packaging

Use the official GNU Arm toolchain with the selected slot script:

```sh
arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -mfloat-abi=soft \
  -nostdlib -T /absolute/project/linker/app_a.ld \
  -Wl,-z,max-page-size=0x100 \
  /absolute/project/tests/boot/layout_fixture.S -o /tmp/slot-a.elf
arm-none-eabi-objcopy --gap-fill=0xff -O binary /tmp/slot-a.elf /tmp/slot-a.bin
```

For B use `app_b.ld`. Real applications supply their usual startup/libraries in
place of the fixture. The slot page-size option remains required. GNU's default
4 KiB page size can put A's header-bearing PT_LOAD at `0x8000`, outside the
payload. Do not loosen packager bounds to accept that producer. Excessive input
section alignment can also change PT_LOAD layout. Validate every final ELF.

The layout fixture links with `-z,max-page-size=0x100`. Both A and B pass the
existing packager's ELF and raw canonical-payload paths. Separate explicit-LMA
gap fixtures verify GNU objcopy `0xFF` gap filling byte-for-byte against that
packager. Normal default `AT> FLASH` placement is also checked. The existing
`boot_package` suite supplies external-key signing/OpenSSL/C-verifier coverage.
this layout suite adds no key handling or dependency.


## Layout validation

### Reproducible host gate

```sh
cmake -S tests/boot -B /tmp/lm3s-layout-host-debug \
  -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build /tmp/lm3s-layout-host-debug -j8
ctest --test-dir /tmp/lm3s-layout-host-debug --output-on-failure
```

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
no GC, and dead GC. Live/rooted/no-GC unknown inputs must reject. Dead
PROGBITS/NOBITS inputs may link only with section and symbol absence checked
in the final ELF. Allocated NOTE inputs can remain retained even when
unreferenced, so the expected assertion still applies. A successful link
is not permitted merely because an input was nominally unreferenced. Additional
positive fixtures preserve nonallocated custom metadata, debug and ARM attributes
and still pass exact PT_LOAD/objcopy checks.
Wrong actual MSP
bytes are rejected by artifact validation, not falsely claimed as a ld check.


## Final-artifact resource gate

### Reproduce the final-artifact gate

From the repository root:

```sh
cmake -S tests/boot/resource_probe -B build/boot-resource-arm \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/boot-resource-arm --parallel
python3 tests/boot/resource_probe/check.py build/boot-resource-arm/resource_probe \
  --map build/boot-resource-arm/resource_probe.map
```

Observed compiler: official local
`~/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin/arm-none-eabi-gcc`,
**Arm GNU Toolchain 15.3.Rel1 (Build arm-15.149), GCC 15.3.1 20260627**.
PATH Homebrew GCC 16.2.0 was not used. Compilation is `-Os`, Cortex-M3, Thumb,
soft float, function/data sections, no LTO. Link: `-nostartfiles`,
`--gc-sections`, `-z max-page-size=0x100`, production `linker/bootloader.ld`,
map and cross references. `CMakeFiles/resource_probe.dir/link.txt` records the
actual command. The fixture explicitly uses **default full newlib**
`arm-none-eabi/lib/thumb/v7-m/nofp/libc.a` and corresponding `libgcc.a`, not
nano/nosys/rdimon or a substitute allocator. The root BSP separately requests
nano/nosys. These footprints are not interchangeable.

The bounded test-only checker rejects malformed/truncated ELF tables and loads,
compares actual file-backed PT_LOAD ends with `_flash_payload_end`, enforces
32 KiB and lower-SRAM bounds, checks vector/MSP/reset and NOLOAD reservations,
requires retained verifier/crypto/allocator symbols and the 65-byte public-key
symbol, rejects undefined symbols and unnecessary signing symbols, and checks
production `_sbrk` / full-newlib map ownership. `boot_resources` creates a fresh
actual Arm Release probe and runs four positive/negative Python tests, without
executing target code. Negative mutations cover truncation, bad tables, flash
load overflow, file-backed reservations, missing closure symbols and wrong
allocator ownership. This is an engineering artifact gate, not a general ELF
security parser or runtime measurement.


## Allocator and verifier fixture

### Fixture and production allocator closure

`tests/boot/resource_probe/probe.c` retains two real `boot_verify_stored` calls
from `main`, observable result/read sinks, initialized `.data` and zeroed `.bss`.
Production `src/startup.c` vectors/reset copy data, clear bss, call
`__libc_init_array`, and enter `main`. No stack painting, diagnostics publication
or board MMIO is added. The fixture uses the existing public independently
signed A manifests for minimum **236-byte** and maximum **111616-byte** payloads.
Its bounded deterministic chunk reader reconstructs the `i * 17` pattern,
`0xff` gaps/tail, MSP and Thumb reset vector without staging a maximum payload
in SRAM. Host verifier tests validate these patterns/manifests. This original Arm
fixture remains link-only. The separate execution fixture below uses the same
public signed fixtures (including B's `slot * 31` pattern). Final linked `main` disassembly contains two calls to
`boot_verify_stored`. Retained final symbols include ECDSA verify, SHA-256
starts/update/finish and `lm3s6965_boot_demo_public_key` of exactly **65 bytes**.

The reviewed production format/verifier and unchanged ten-module Mbed TLS 3.6.7
configuration are used. All ten modules still enter archive dependency closure,
but final garbage collection removes unused signing, signature-writing and
ASN.1-writing code. Presence in archive closure/cross references is not retention
in the final image. The shared fixture array still retains fourteen public
manifests (**2240 bytes**) although only two A modes are called. This overhead is
reported, not concealed or trimmed to make the link pass.

Production `src/syscalls.c` is a direct executable input. The actual map binds:

```text
bignum -> calloc -> _calloc_r -> _malloc_r -> _sbrk_r -> production _sbrk
bignum -> free -> _free_r
```

Full newlib provides `calloc`, `_calloc_r`, `_malloc_r`, `_sbrk_r`, `free`,
`_free_r`. `src/syscalls.c.obj` provides `_sbrk`, not libnosys. Static heap inputs
remain excluded. Diagnostics/stack remain protected. No fault allocator,
interception or private pool conceals costs. This baseline contains no live-heap instrumentation. The separate execution
fixture measures requested bytes and production break high-water below, not a
general fragmentation analysis. Newlib reentrancy/stdio still
pulls stdio and reentrant syscall adapters without fixture console calls.
Only test-only `uart0_write_byte`/`trace_write_byte` no-op bindings in
`console_stubs.c` satisfy production `_write`. No errno or allocator stubs exist.
The execution fixture does not use `_write` for reporting.


## Resource instrumentation

Run reporting, stack-burn and real-allocation calibrations before interpreting
crypto measurements. Require exact report framing, matching process exit and
intact guards. Only the deliberate termination calibration may accept exit 1.

```sh
python3 tests/boot/test_resource_execution.py --evidence /tmp/new-resource-evidence
```

Use a new evidence destination. The runner builds isolated Arm fixtures and
executes QEMU, not the board. It compares boot and actual FTP capacities against
a separately controlled historical 2048-byte negative fixture. That fixture does
not change the production 8192-byte heap policy.

The strict parser rejects missing/extra/duplicate/malformed reports, crashes,
timeouts, unexpected/nonmatching exits, wrong error, leaks, overwritten output,
tracking overflow and damaged guards. Negative parser mutations are exercised.
Expected FTP OOM is reported as an **expected error test** with harness exit 0.
it does not mean verification succeeded or the 2 KiB policy passed.

`--wrap=calloc/free/_sbrk` forwards to untouched full-newlib `calloc/free` and
production `_sbrk`. The disassembly gate proves wrapper forwarding, newlib
`_sbrk_r` calling the wrapper, a zero-frame scanner without stack/stores/calls,
and the reset painter's exact literals `[0x2000e000,0x20010000)` plus pattern.
Maps independently prove full-newlib and production allocator ownership. FTP
uses unchanged `ftp_sbrk.c` and the reviewed FTP linker. The generic syscall unit
is separately compiled with only its `_sbrk` symbol renamed and discarded, to
supply unchanged stdio adapters without a duplicate allocator. It is not a
substitute allocation algorithm. Positive capacity runs never use injected or
artificial allocation. The isolated sample fault cases reject the first calloc.

Bookkeeping is a fixed 128-entry pointer/requested-byte table, never mallocs,
and fails closed on overflow/unmatched free. Requested-live counts measure
verifier `calloc` requests, not newlib headers/alignment/slack. Every successful
allocation is really freed, with final tracked bytes/count **0/0**. Backing peak
and final break measure retained newlib arena. They **do not** return to zero
when allocations are freed. Initial/full-newlib demand, page alignment and arena
retention therefore remain visible separately from requested-live bytes.

The painter is enabled via the existing optional startup macro **only in the
fixture**. Production source/vectors for the original probe are unchanged. The
64-byte diagnostic reserve is not painted or used as a report ABI. Sixteen
fixture canaries fill it after startup. Its canaries and the bottom four painted
stack words survive every run. Scans are taken **before reporting**, are repeated
with identical depth, and have a verified naked register-only body. Reporting
cannot contaminate a later case because every case uses a fresh emulator reset.
Read actual target-emulated MSP separately from written stack depth. Neither
an emulator stack address nor a host estimate establishes a physical peak.


## Resource runner evidence rules

The resource runner retains a unique `/tmp/boot-resource-execution-*` directory:
per-command argv/timeout/actual exit, separate byte-exact stdout/stderr (bounded
at 1 MiB each, overflow rejects), timeout/launch-error metadata, build artifacts,
ELF/map inputs and raw `.su` files. Frame origins/sizes/modes accompany a complete
measurement. Raw evidence is saved before parsing, including nonzero exits and
partial timeout output. `--evidence /tmp/<new-directory>` selects an explicit
new destination. `--json` is restricted to `/tmp`.

The parser allows only one optional exact offline prefix
`qemu-system-arm: warning: nic stellaris_enet.0 has no peer\n` before the
single newline-terminated `RESOURCE` report. Unknown/duplicate/trailing warnings,
extra reports/output, malformed fields and unexpected exits still fail. Only
deliberate termination calibration case 1 may exit 1. This is not a general
nonzero-exit exemption. Offline NIC/QEMU arguments are unchanged. No measurement
is published until all **81 processes**, capacities, guard/leak/peak assertions
and static-frame checks complete. A parser repair alone does not establish resource fit or classify a production
failure. Require complete fresh evidence for the exact source and configuration.
