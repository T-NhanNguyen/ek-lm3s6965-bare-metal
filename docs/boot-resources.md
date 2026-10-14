# Boot verifier resources — reviewed Release link and isolated QEMU gate

## Approved 8 KiB FTP increment — current state

The user approved **8192 bytes total FTP C heap**, formerly 2048 (+6144),
including collected static `.heap[.*]` inputs. Only the FTP linker reserve and
its fixed-budget assertions change; dynamic base remains `_heap_static_end`.
Production allocator, generic heap policy, crypto/vendor sources, 64-byte
`[0x2000dfc0,0x2000e000)` diagnostics, 8192-byte stack and boot flash remain unchanged.
The formerly pending recommendation and 2 KiB OOM evidence below are retained
history, superseded by this approval and implementation, not deleted.

**Current isolated gate:** actual production FTP linker capacity **8192** verifies
A/B minimum/maximum signed images successfully. Requested peak **3144**, backing
**4632**, written stack **2364**, source-frame allowance **568**, final live **0/0**.
The separate controlled `old_ftp_2k.ld` build-directory copy tests historical
capacity **2048** with the unchanged production FTP allocator: valid images
still fail exactly CRYPTO (5), growth **4096 at break 536**. The production script
is never patched to run that negative fixture. Strict parser negatives reject
missing growth failure and forbid interpreting old OOM as production success.
There are now **81 QEMU cases** (27 boot + 27 actual FTP + 27 historical 2 KiB),
no skips. Debug/Release ASan/UBSan each pass **15/15 CTest**. The uninstrumented
probe remains **25476 flash bytes**. The instrumented successful FTP fixture is
**27636 flash bytes**, `.data` **1560**, `.bss` **2000**, capacity **8192**; the
old controlled fixture remains **27584 flash bytes**.

**Current standalone maps:** `.data` **8**, `.bss` **39976**, total static **39984**,
no static heap inputs; heap **`[0x20009c30,0x2000bc30)`**, spare **9104** below
diagnostics (formerly 15248). Four root Debug/Release × painter OFF/ON builds,
**20 ELFs**, preserve exact dynamic base/end, NOLOAD boundaries and reset/MSP.
FTP flash ends Debug OFF/ON **32816/32860**, Release OFF/ON **27612/27656**.
These standalone whole-device images are **not a real 32 KiB boot-fit proof**.
The 8 KiB fixture has observed backing headroom **3560**, not integrated network
fragmentation/peak assurance. Live publication, network workloads, timing and
full boot/FTP acceptance remain pending. **Independent review is next.**

## Contents

- [Root memory specification](#root-memory-specification)
- [Reproduce the final-artifact gate](#reproduce-the-final-artifact-gate)
- [Minimal GC correction and retained-only proof](#minimal-gc-correction-and-retained-only-proof)
- [Fixture and production allocator closure](#fixture-and-production-allocator-closure)
- [Actual ELF measurements](#actual-elf-measurements)
- [Stack evidence and validation boundaries](#stack-evidence-and-validation-boundaries)
- [Isolated QEMU execution gate](#isolated-qemu-execution-gate)
- [Measured former fixed FTP budget blocker](#measured-former-fixed-ftp-budget-blocker-historical-2-kib-evidence)
- [Former validation and next gate](#former-validation-and-next-gate-historical-superseded-above)

## Root memory specification

The authoritative link-time contract is [boot-memory-layout.md](boot-memory-layout.md)
with `linker/bootloader.ld`: boot flash `[0,0x8000)` is **32768 bytes**;
metadata `[0x8000,0x8800)` is not available for spill. Ordinary SRAM is
`[0x20000000,0x2000dfc0)`, diagnostics are **64-byte NOLOAD**
`[0x2000dfc0,0x2000e000)`, and stack is **8192-byte NOLOAD**
`[0x2000e000,0x20010000)`. Startup copies only `.data` and zeroes only `.bss`.
Dynamic production `_sbrk` begins at `_heap_static_end` and ends at `_heap_end`.

The reviewed **uninstrumented test-only link probe** remains unchanged and fits
the boot partition. The separate opt-in [QEMU gate](#isolated-qemu-execution-gate)
now executes an isolated instrumented fixture; it is not a complete bootloader or
live diagnostic publisher. No partition, crypto configuration, dependency pin,
vendor bytes, production ABI, allocator or startup source was changed. The existing
reset-only painter macro is enabled only in separately compiled fixture startup.
No hardware or live boot-runtime/diagnostic integration is authorized here.
The link-only evidence below is retained as the reviewed baseline, not a claim
that runtime costs are still unmeasured.

## Reproduce the final-artifact gate

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
nano/nosys; these footprints are not interchangeable.

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

## Minimal GC correction and retained-only proof

The initial failed link placed **37112 bytes (`0x90f8`)**, overflowing by **4344**.
That was failed-map evidence, with no final ELF. It is now **superseded by the
actual accepted ELF/PT_LOAD measurements below**, not merely a revised placement
estimate. The broad catch `KEEP(INPUT_SECTION_FLAGS (SHF_ALLOC) *(*))` had rooted
allocated inputs already collected into supported outputs as well as unknown
inputs; the map said there were no discarded inputs despite `--gc-sections`.

Only that broad `KEEP` wrapper was removed in all five scripts. The flag-filtered
catch and `ASSERT(SIZEOF(.unsupported_allocated) == 0, ...)` remain ahead of
comment/note discard rules; all vector, constructor and other known `KEEP`s are
unchanged. This engineering correction changes prior **reject-even-dead** tests
to **reject-retained** tests. It does not weaken runtime, crypto, memory-map,
copy/zero or allocator policy: no unknown retained bytes receive invented
ownership or initialization. It does not force allocated notes into `/DISCARD/`.

The exact **120 proofs** are five scripts × six allocated input classes × four
modes. Classes: writable/read-only/executable PROGBITS, writable NOBITS,
allocated NOTE, and allocated `.comment` PROGBITS. Modes: reference from rooted
`main`/Reset_Handler with GC, `-u unknown` with GC, no GC, and unreferenced GC.

- **90** live/reference, `-u`, and no-GC cases reject with the specific
  `unsupported allocated input section` assertion.
- **25** dead PROGBITS/NOBITS cases link; the unknown section, unknown symbol
  and unsupported output are verified absent from each final ELF.
- **5** dead allocated NOTE cases reject with the same intended assertion:
  GNU ld retains allocated NOTE even when nominally unreferenced.

Thus the scratch result “115 pass / 5 dead NOTE failures” was an incorrect test
oracle, **not a policy failure**. All **120/120 satisfy the retained-only property**:
an unknown input may link only when the linker discards it and it is absent from
the ELF; any retained unknown must reject. No fragile requirement that every
unreferenced allocated input be discarded is imposed. Allocated `.comment` is
checked for both live rejection and actual dead-GC absence.

## Fixture and production allocator closure

`tests/boot/resource_probe/probe.c` retains two real `boot_verify_stored` calls
from `main`, observable result/read sinks, initialized `.data` and zeroed `.bss`.
Production `src/startup.c` vectors/reset copy data, clear bss, call
`__libc_init_array`, and enter `main`. No stack painting, diagnostics publication
or board MMIO is added. The fixture uses the existing public independently
signed A manifests for minimum **236-byte** and maximum **111616-byte** payloads.
Its bounded deterministic chunk reader reconstructs the `i * 17` pattern,
`0xff` gaps/tail, MSP and Thumb reset vector without staging a maximum payload
in SRAM. Host verifier tests validate these patterns/manifests. This original Arm
fixture remains link-only; the separate execution fixture below uses the same
public signed fixtures (including B's `slot * 31` pattern). Final linked `main` disassembly contains two calls to
`boot_verify_stored`; retained final symbols include ECDSA verify, SHA-256
starts/update/finish and `lm3s6965_boot_demo_public_key` of exactly **65 bytes**.

The reviewed production format/verifier and unchanged ten-module Mbed TLS 3.6.7
configuration are used. All ten modules still enter archive dependency closure,
but final garbage collection removes unused signing, signature-writing and
ASN.1-writing code. Presence in archive closure/cross references is not retention
in the final image. The shared fixture array still retains fourteen public
manifests (**2240 bytes**) although only two A modes are called; this overhead is
reported, not concealed or trimmed to make the link pass.

Production `src/syscalls.c` is a direct executable input. The actual map binds:

```text
bignum -> calloc -> _calloc_r -> _malloc_r -> _sbrk_r -> production _sbrk
bignum -> free -> _free_r
```

Full newlib provides `calloc`, `_calloc_r`, `_malloc_r`, `_sbrk_r`, `free`,
`_free_r`; `src/syscalls.c.obj` provides `_sbrk`, not libnosys. Static heap inputs
remain excluded; diagnostics/stack remain protected. No fault allocator,
interception or private pool conceals costs. This baseline contains no live-heap instrumentation. The separate execution
fixture measures requested bytes and production break high-water below, not a
general fragmentation analysis. Newlib reentrancy/stdio still
pulls stdio and reentrant syscall adapters without fixture console calls.
Only test-only `uart0_write_byte`/`trace_write_byte` no-op bindings in
`console_stubs.c` satisfy production `_write`; no errno or allocator stubs exist.
The execution fixture does not use `_write` for reporting.

## Actual ELF measurements

Sources: `build/boot-resource-arm/resource_probe` and `resource_probe.map`,
independent ELF gate, GNU `readelf -l`, `size -A`, `nm -S`, `nm -u` (empty),
and final linked disassembly. Fresh CTest Release builds reproduce the numbers.

| Component | Bytes / range |
|---|---:|
| Vector table | 236 |
| `.text` including constants and alignment | 23664 |
| `.ARM.exidx` | 8 |
| Initialized `.data` (flash LMA and SRAM VMA) | 1568 |
| Actual flash `[0, _flash_payload_end)` | **25476 (`0x6384`)** |
| 32768-byte partition margin | **7292 (`0x1c7c`)** |
| `.bss` | 512 |
| Static `.c_heap` inputs | 0 |
| Dynamic heap capacity, not demand | **55200**, `[0x20000820,0x2000dfc0)` |
| Diagnostic NOLOAD | **64**, `[0x2000dfc0,0x2000e000)` |
| Stack NOLOAD | **8192**, `[0x2000e000,0x20010000)` |

Actual PT_LOADs (file sizes, not ELF disk size):

| VMA | Physical/LMA | File bytes | Memory bytes | File-backed end |
|---|---|---:|---:|---|
| `0x0` | `0x0` | 23908 (`0x5d64`) | 23908 | `0x5d64` |
| `0x20000000` | `0x5d64` | 1568 (`0x620`) | 2080 (`0x820`) | **`0x6384`** flash |
| `0x2000dfc0` | `0x2000dfc0` | 0 | 64 | None (NOLOAD) |
| `0x2000e000` | `0x2000e000` | 0 | 8192 | None (NOLOAD) |

Data counts once in flash at LMA and separately in SRAM at VMA. Diagnostic/stack
NOLOAD do not consume flash. ELF debug sections and headers are not boot payload.
Heap capacity is not allocator usage; stack reservation is not measured depth.

## Stack evidence and validation boundaries

`-fstack-usage`: 16 `.su` files, 249 function records (including later-discarded
functions). Individual static frames include `boot_verify_stored` 568,
`boot_verify_manifest.part.0` 408, SHA-256 block processing 360,
`ecp_mul_restartable_internal.isra` 280, `main` 48, Reset_Handler 8, `_sbrk` 8.
These do not establish worst-case call depth or 8 KiB adequacy. Prebuilt
newlib/libgcc have no corresponding `.su` files here. Interrupt nesting,
recursive paths, heap behavior and watchdog timing remain unmeasured.

Revalidated host gates (`build/resource-host-{debug,release}`):

```sh
for mode in Debug Release; do
  lower=$(printf '%s' "$mode" | tr '[:upper:]' '[:lower:]')
  cmake -S tests/boot -B "build/resource-host-$lower" \
    -DCMAKE_BUILD_TYPE="$mode" -DBOOT_SANITIZERS=ON
  cmake --build "build/resource-host-$lower" --parallel
  ctest --test-dir "build/resource-host-$lower" --output-on-failure
done
python3 scripts/setup-dependencies.py --check
```

Both modes: **14/14 PASS** (existing 13 plus `boot_resources`), native ASan/UBSan
enabled; package **20/20**, layout **13/13** with **120/120** retained-only proof
cases, resources **4/4**, no skips. Four root builds in
`/tmp/lm3s-gc-root-{Debug,Release}-{OFF,ON}` use FTP enabled and
`LM3S6965_FTP_STACK_MEASUREMENT=OFF/ON`; all **20 example ELF** memory/vector
checks pass. Existing offline Ethernet platform tests pass. Dependency status:
exact clean FreeRTOS/lwIP pins, Mbed TLS 3.6.7 required files/modules present.
Vendor bytes remain unchanged/ignored; this offline status is not fresh source
authentication. Acquisition evidence remains in
`third_party/mbedtls-provenance.md`.

This accepted **verifier probe**, including fixture and full-newlib overhead,
is not the footprint or acceptance of a full bootloader: metadata store,
lifecycle/reset integration, hardware adapters, watchdog/LED/handoff and runtime
diagnostics are absent. No integrated boot fit, timing or hardware acceptance follows. The isolated
execution measurements and FTP capacity blocker below supersede only the former
unmeasured-resource boundary. **Independent review is the next gate.**

## Isolated QEMU execution gate

Reproduce without network, new dependencies, private-key handling or hardware:

```sh
cmake -S tests/boot/resource_probe -B build/boot-resource-execution \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DRESOURCE_EXECUTION=ON
cmake --build build/boot-resource-execution --parallel
python3 tests/boot/test_resource_execution.py \
  --build build/boot-resource-execution \
  --json build/boot-resource-execution/measurements.json
```

Without `--build`, the runner fresh-builds and gates all four Arm Release ELFs (baseline, boot, actual FTP, old 2 KiB fixture).
`BOOT_ARM_GCC` and `BOOT_QEMU` can select installed tools. The default probe option
is OFF, so existing reviewed link-only builds are unchanged. Native CTest adds
`boot_resource_execution`, bringing the suite to **15 tests**. Missing QEMU prints **UNSUPPORTED** and
returns 77 (CTest explicitly marks unsupported/skipped, never passed); current
QEMU is available and all runs execute, with **no skips**. Each QEMU invocation
has a **20-second timeout**, with a **180-second outer CTest bound**. This is a
runner safety bound, not a MCU timing or watchdog requirement.

Observed `/opt/homebrew/bin/qemu-system-arm`: **11.1.1**, `-M lm3s6965evb`, genuine
Cortex-M3 MSP and production startup. Arguments use
`--semihosting-config enable=on,target=native,arg=<case>`. Test-only inline
`BKPT 0xAB` implements GET_CMDLINE, WRITE0 and EXIT_EXTENDED; no rdimon/specs,
production stdio modifications, UART/ITM reporting, or dynamic reporting buffer.
The earlier calibration attempt using `-append` with a 32-byte GET_CMDLINE buffer
was rejected by QEMU (kernel pathname was included); it timed out and **was not
accepted as a resource result**. Explicit semihost `arg` is the verified runner.

**Reporting calibration runs before any crypto result is trusted**, in each
configuration and fresh QEMU process:

| Case | Evidence |
| --- | --- |
| 0 | Report parsed, exact exit 0; stack before 476, after explicit 512-byte burn 1024; guards preserved |
| 1 | Same report/stack evidence, intentional report status 1 and exact QEMU exit 1; only this termination calibration accepts nonzero |
| 2 | Genuine newlib allocations of 37 + 73 bytes: requested peak 110, peak count 2, final bytes/count 0; backing 536; report/exit 0 |

The strict parser rejects missing/extra/duplicate/malformed reports, crashes,
timeouts, unexpected/nonmatching exits, wrong error, leaks, overwritten output,
tracking overflow and damaged guards. Negative parser mutations are exercised.
Expected FTP OOM is reported as an **expected error test** with harness exit 0;
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
artificial allocation; the isolated sample fault cases reject the first calloc.

Bookkeeping is a fixed 128-entry pointer/requested-byte table, never mallocs,
and fails closed on overflow/unmatched free. Requested-live counts measure
verifier `calloc` requests, not newlib headers/alignment/slack. Every successful
allocation is really freed, with final tracked bytes/count **0/0**. Backing peak
and final break measure retained newlib arena; they **do not** return to zero
when allocations are freed. Initial/full-newlib demand, page alignment and arena
retention therefore remain visible separately from requested-live bytes.

The painter is enabled via the existing optional startup macro **only in the
fixture**; production source/vectors for the original probe are unchanged. The
64-byte diagnostic reserve is not painted or used as a report ABI. Sixteen
fixture canaries fill it after startup; its canaries and the bottom four painted
stack words survive every run. Scans are taken **before reporting**, are repeated
with identical depth, and have a verified naked register-only body. Reporting
cannot contaminate a later case because every case uses a fresh emulator reset.
Actual post-verification MSP is **0x2000fe00**, not a host/emulator stack estimate.

### Genuine verifier executions and costs

Case indices are stable and recorded in JSON:

| Cases | Public fixtures / expected verifier results |
| --- | --- |
| 3–6 | A-min, A-max, B-min, B-max: OK (0), lengths 236/111616 |
| 7–13 / 14–20 | A / B minimum: corrupt format (2), tampered signed manifest (4), tampered payload/hash (8), independently signed invalid MSP vector (9), reader zero/error (6), authenticated-phase cancel (7), first-allocation failure (5) |
| 21–23 / 24–26 | A / B maximum: payload/hash tamper (8), reader error (6), authenticated-phase cancel (7) |

There are **24 verifier cases + 3 calibrations per configuration (54 fresh QEMU
runs)**. This deliberately avoids exhaustive allocation/signature mutation cost.
Successful output checks slot, length, hash, actual MSP/reset vector and completed
reader length; all failures prove the caller's output remains byte-for-byte
unchanged. The real Mbed TLS 3.6.7 ECDSA/SHA256 paths execute without crypto flags,
module pins, optimization or library changes.

| Boot large-backing measurement | Bytes / count |
| --- | ---: |
| Requested-live peak, all crypto-completing cases | **3144** |
| Peak live allocations | **55** |
| Genuine allocation calls (valid min / max) | **28620 / 28619** |
| Final requested-live bytes / allocations | **0 / 0** |
| Production break high-water / final backing | **4632 / 4632** |
| Genuine allocator failed calls | **0** (except deliberate first-call fault) |
| Painted stack depth, crypto-completing cases | **2364** |
| Format rejection / first-call fault stack | **1640 / 2020** |

The scanner measures lowest **written** stack word, not minimum SP at every
instruction. Static source frames: main 504 (baseline main 48), stored verifier
568, manifest verifier 408, SHA256 block 360, reset 8, calloc wrapper 16, free
wrapper 8, sbrk wrapper 8, scanner 0. The gate conservatively adds the largest
source frame (**568**) to observed 2364, giving **2932**, with **5260 bytes** to
the 8 KiB limit; the bottom 16 bytes also remain guarded. This is a conservative
fixture acceptance allowance, **not a mathematically complete worst-case bound**
for prebuilt newlib/libgcc frames, interrupt nesting, other inputs or integrated
runtime. No subtraction of instrumentation frames is used to claim production
stack consumption. Emulator instruction/wall time is **not 50 MHz MCU latency,
watchdog, crypto deadline or hardware evidence**.

### Former artifact and instrumentation accounting (historical, superseded above)

Same approved Arm Release `-Os`, no LTO, full newlib and separate exact NOLOAD
stack **8192** / diagnostics **64** for every ELF:

| ELF | Flash payload | 32 KiB margin | `.data` | `.bss` | Dynamic backing capacity |
| --- | ---: | ---: | ---: | ---: | ---: |
| Reviewed uninstrumented probe | 25476 | 7292 | 1568 | 512 | 55200 |
| Isolated instrumented boot | **27636** | **5132** | 1560 | 2000 | **53720** |
| Isolated fixed-FTP fixture | **27584** | **5184** | 1560 | 2000 | **2048** |

Direct instrumentation static RAM is **1616 bytes**: slots 1024, report 512,
command 32, twelve counters/control words 48. It is explicitly retained in
ordinary `.bss`, outside diagnostics and stack. Net fixture static-RAM increase
is **1480 bytes**, not 1616, because baseline fixture sinks/image and initialized
data differ. Exact total flash increase is **2160 bytes (boot)** / **2108 (FTP)**;
this includes instrumentation, startup painter, reporting, calibration and the
larger fixture matrix, not merely allocator wrappers. All fourteen public
manifest arrays remain retained (**2240 bytes**) as in the baseline. The checker
still enforces actual flash loads below 32768 even though the reviewed standalone
FTP script itself describes whole-device flash. The instrumented footprint is
**not** substituted for the reviewed uninstrumented production-probe footprint.

## Measured former fixed FTP budget blocker (historical 2 KiB evidence)

**The controlled unchanged 2048-byte FTP heap cannot verify even a valid minimum
image.** Every crypto-entering non-injected case, including four valid A/B
min/max cases, returns exactly **BOOT_VERIFY_ERROR_CRYPTO (5)** before reading
payload. At failure: requested-live peak **448**, count peak **9**, allocation
calls **17**, failed calloc **1**, failed `_sbrk` **1**, break/high-water **536**,
rejected positive `_sbrk` increment **4096**. Real frees leave live bytes/count
**0/0** and final arena break **536**. Format failures remain FORMAT (2); sampled
first-call injection remains CRYPTO (5) with no backing growth.

The allocator needs **536 + 4096 = 4632 bytes** just to admit this observed arena
growth, **2584 bytes above** the fixed 2048 capacity (remaining 1512 cannot admit
4096). Successful boot backing is independently measured at 4632. This is an
observed aligned fixture requirement, not a universal minimum or automatic heap
policy change: newlib break/page alignment changes with static placement, and a
real FTP session may already consume or fragment its heap.

Before the approved increment, production FTP was rebuilt at
`/tmp/lm3s-gc-root-Release-OFF/examples/ethernet-ftp/lm3s6965_ethernet_ftp` with its
map. It was the standalone nano/nosys image, **not** the full-newlib verifier
fixture. Historical ELF evidence (superseded by the current 8192-byte map above):

| Former 2 KiB FTP ownership (historical) | Bytes / addresses |
| --- | --- |
| `.data` / `.bss`, ordinary static RAM | **8 / 39976**, total **39984** |
| Fixed `.c_heap` | **2048**, `[0x20009c30,0x2000a430)`; no static heap inputs |
| Lower-SRAM spare after fixed heap | **15248**, `[0x2000a430,0x2000dfc0)` |
| Diagnostic / stack reservations | **64 / 8192**, unchanged exact bounds |
| Flash payload end | **27612 (`0x6bdc`)**, standalone whole-device image |

Bounded **recommendation only**: separately review an **8 KiB total dynamic-heap
budget** for later integrated verification, then repeat genuine full-newlib
allocation/fragmentation measurements alongside FTP static RAM and traffic.
That would be +6144 over the current reserve and leave **9104** lower-SRAM spare
in the *current standalone* map before any integration/newlib/static-data delta.
Neither 8 KiB nor that spare arithmetic proves integrated sufficiency. The
minimum observed +2584 would leave zero workload headroom at the measured arena
peak; it is not a safe deployment recommendation. **No actual FTP heap was
increased**, no 32 KiB contract relaxed, no pool/new allocator/library switch or
crypto optimization change was made.

## Former validation and next gate (historical, superseded above)

Debug and Release native builds with **ASan/UBSan** each pass **15/15 CTest**, no
skips (including fresh Arm link-only and execution builds). Existing resource
parser **4/4**, execution parser negatives and **54/54** expected QEMU outcomes
pass. Dependency offline check reports clean exact FreeRTOS/lwIP pins and Mbed
3.6.7 required modules; no acquisition or vendor modifications. The original
reviewed Arm ELF remains **25476 flash bytes**. Current FTP rebuild/map is checked
separately; no live FTP, MMIO/hardware, diagnostic publication or boot-runtime
operation occurred.

**Next gate: independent review of the isolated measurement implementation and
results.** Large-backing verifier-resource evidence can support a later separately
authorized diagnostic-publication increment after review, but does not implement
or authorize it. **A resource-policy decision is required before claiming FTP
verification capacity or integrating verifier/FTP runtime:** the unchanged fixed
2 KiB policy is a measured blocker. Publication policy must preserve diagnostics,
8 KiB stack, allocator ownership and the 32 KiB boot limit; it cannot conceal
this OOM finding. No next module is implemented here.
