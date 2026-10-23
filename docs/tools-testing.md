# Tools and validation

Choose validation by the claim it can establish. Static inspection and native models do not prove physical timing. Build-only modes execute no behavioral tests. Signing tests can access keys, so they are not part of a key-free documentation check. Live scripts need separate approval.

## Find the contract

- [Inspect an ELF](#inspect-an-elf)
- [Offline Ethernet suites](#offline-ethernet-suites)
- [Real lwIP memory-peer scenarios](#real-lwip-memory-peer-scenarios)
- [Host boot checks](#host-boot-checks)
- [Stack watermark procedure](#stack-watermark-procedure)
- [Stack visualization](#stack-visualization)


## Inspect an ELF

```bash
TCBIN="$HOME/.toolchains/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin"
$TCBIN/arm-none-eabi-objdump -f build/examples/baremetal/lm3s6965_firmware   # architecture: armv7
$TCBIN/arm-none-eabi-readelf -A build/examples/baremetal/lm3s6965_firmware   # Tag_THUMB_ISA_use: Thumb-2
$TCBIN/arm-none-eabi-size   build/examples/baremetal/lm3s6965_firmware
```

Expect these `readelf -A` attributes: `Tag_CPU_name: "7-M"`, `Tag_CPU_arch: v7`,
`Tag_CPU_arch_profile: Microcontroller`, and `Tag_THUMB_ISA_use: Thumb-2`.


## Offline Ethernet suites

```sh
./scripts/ethernet/test.sh --list
./scripts/ethernet/test.sh core tcp lwip platform
./scripts/ethernet/test.sh consumer
```

The consumer is compile/link only. Core, fake TCP, real lwIP, RAM, platform/heap, driver, RAW self-tests and saved-link fixtures cover different boundaries. See [suite launchpad](../scripts/ethernet/README.md). Default and `all` are offline. RAW default sends live traffic and link live capture resets the board. See [Ethernet diagnostics](../scripts/ethernet-diagnostics.md).


## Real lwIP memory-peer scenarios

From the repository root, run `./scripts/ethernet/test.sh lwip`. It compiles the populated lwIP 2.2.1 core,
IPv4 and Ethernet sources together with production `ftp_core.c` and
`ftp_tcp.c`, using ASan/UBSan. No sockets, BPF, device, wall clock or external
network are used. The existing native options shim changes only alignment to
8 bytes. The build disables lwIP byteorder function aliases for Darwin.
Firmware TCP/heap/pool capacities remain unchanged.

### Asserted scenarios

- Valid Ethernet ARP request/reply and IPv4/TCP checksummed SYN/SYN-ACK/ACK.
  outgoing IPv4 and TCP checksums independently verified by the test peer.
- Anonymous login split across TCP packets, including a split CRLF. PASS and
  TYPE I coalesced with the preceding command's terminating LF.
- EPSV and PASV reply parsing, including exact PASV address, and actual passive
  TCP handshakes.
- STOR/RETR `hello`, exact byte comparison and `226 Transfer complete` for
  0, 17 and 4096 bytes (nontext patterns include NUL). Both passive modes used
  for uploads and downloads. Upload EOF and server download FIN observed.
- Withheld download ACKs cause real timer-driven retransmission. Duplicate
  sequence ranges do not duplicate reconstructed bytes. No FIN/226 while
  ACKs lag, including after all bytes have arrived but the final ACK is absent.
- Data RST after partial upload and a 4097-byte overflow produce 426/552.
  subsequent RETR still returns the previous complete 4096-byte file.
- 100 coalesced NOOP commands with linkoutput returning ERR_MEM exercise
  output retry, reply-ring pressure and retained receive cursor. All 100 exact
  replies arrive once and in order after output resumes.
- Exhaust the real fixed TCP_SEG pool after upload EOF. FIN cannot enqueue:
  no server FIN, no 226, no replacement of prior bytes, and callbacks/PCB remain
  owned with CLOSEPEND cleared. Inject RST before resource recovery, test the
  exact 4999/5000-ms close boundary with real timers, then test pool recovery
  producing FIN/226/commit. No test-only allocator hook or stack patch is used.
- QUIT with a held input chain and lwIP `refused_data`, then newly arriving
  trailing input: all are credited/discarded. Actual TCP segment exhaustion
  and linkoutput pressure delay closure. On recovery exactly 221 and FIN arrive
  without RST. Indefinite output failure aborts at 5 s even after the core reply
  ring drains. Fake-API tests additionally cover download output expiry after
  every file byte is enqueued, and control/link loss before close acceptance.
- Core whole-span EPSV/NOOP success and failure return the EPSV accepted prefix,
  block the suffix while opening, then queue 229/425 before 200.
- Control RST followed by a fresh handshake/login/download.
- Link-down during a partial upload, offline adapter teardown, fake-time
  advancement, link-up, fresh session and download of the retained file.
- Teardown must leave no active, TIME_WAIT, bound or listening TCP PCBs, zero
  lwIP heap use and zero PBUF_POOL/PBUF/TCP_PCB/TCP_PCB_LISTEN/TCP_SEG pool use.

The memory peer uses fixed receive windows and in-order injected traffic. It
checks checksums, retransmission, close ownership and quiescent pool recovery,
not arbitrary loss, IP fragmentation, TCP options, congestion or physical service
timing. Link-down directly invokes netif state and adapter/ARP cleanup, not the
hardware driver. TCP_SEG exhaustion covers FIN and QUIT write allocation, not
all allocator failures. Pool checks establish quiescent teardown, not every
allocation. Accepted FIN enqueue is the success boundary, not a remote FIN ACK.
Private CLOSEPEND/refused-data contracts require re-review on lwIP upgrades.


## Host boot checks

Documentation can participate in source identity. The sequence fingerprint profile
requires `docs/boot-startup.md`. Checked-handoff support also requires
`docs/boot-reset-image.md`. Keep both chapter paths stable. Consolidation
changes its bytes, so retained manifests and previously accepted source hashes
are historical until their owners deliberately renew the relevant evidence.
Do not relabel old proof by updating hashes alone. Launchpad and test-local
identity path migrations must be reviewed alongside the chapter mapping.

### Build and regression

```sh
cmake -S tests/boot -B /tmp/boot-flash-check -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/boot-flash-check --target test_flash test_store
ctest --test-dir /tmp/boot-flash-check -R '^(boot_flash|boot_store)$' --output-on-failure
```

Host target `boot_flash` / test `boot_flash` use the existing sanitizers and
strict warning policy. The root opt-in archive is `lm3s6965_boot_flash`.
the standalone Arm archive target is `boot_flash`. Neither binds MMIO or links
an integrated boot image. Tests inject failures before/after effects at every
callback boundary for program/erase, both store authority pages and the first
two inactive erase steps. They also check last-poll completion, indefinite
busy/recovery, stale status, ordering/keys, alignment/range/overflow, clock
mismatch, 0→1 rejection, readback corruption, commit-last/UNCERTAIN/reload,
reentry rejection and both complete 110-page erase orders. This is a bounded
mock fault model, not arbitrary silicon/power-loss behavior or an independent
metadata codec oracle.

### Host and Arm gates

Standalone host project (existing local dependencies/toolchain/emulator only):

```sh
cmake -S tests/boot -B /tmp/boot-watchdog/debug -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build /tmp/boot-watchdog/debug --parallel 8
ctest --test-dir /tmp/boot-watchdog/debug --output-on-failure
# Repeat with /tmp/boot-watchdog/release and -DCMAKE_BUILD_TYPE=Release.
```

`test_watchdog.c` models real CTL write-once, LOCK status, ICR and LOAD semantics.
it tests callback failures at every configure/feed boundary before/after effects,
ignored writes/useful readback faults, persistent cleanup failure, reentry,
configuration/reconfiguration, stream replay, ownership, all application
predicate subsets, idle service and finite budget/epoch limits. Release assertions
remain enabled (`-UNDEBUG`), with ASan/UBSan in the host suite. Standalone Arm
archive `boot_watchdog` and root opt-in `lm3s6965_boot_watchdog` compile without
crypto linkage, startup, BSP, lwIP or linker scripts. Including verifier types
is not verifier composition. Archive/header compilation is **not** final boot
link/32 KiB fit, hardware timing, full handoff or integrated FTP acceptance.


## Stack watermark procedure

Run the offline fixtures first:

```sh
./scripts/ethernet/test.sh platform
```

### Opt-in build (no device access)

Use the populated pinned lwIP submodule and the official Arm GNU/newlib toolchain.
Both FTP and its measurement option default to OFF. From the repository root:

```sh
cmake -S . -B build/ftp-stack \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_ETHERNET_FTP=ON \
  -DLM3S6965_FTP_STACK_MEASUREMENT=ON
cmake --build build/ftp-stack --target lm3s6965_ethernet_ftp --parallel 4
```

ELF: `build/ftp-stack/examples/ethernet-ftp/lm3s6965_ethernet_ftp`.
The 8 KiB linker reservation is unchanged: `[_stack_limit, _estack)` =
`[0x2000e000, 0x20010000)`. The reset **vector** points to an FTP-only naked
basic-assembly shim, painting ascending words with `0xa5c39e71`, preserving
PRIMASK in a register, masking interrupts, then restoring PRIMASK and branching
to the unchanged C `Reset_Handler`. There are no pushes/calls, stack loads,
or global RAM accesses before painting. This uses the C compiler. It does not
require CMake's ASM language. The ELF entry symbol remains `Reset_Handler`:
starting at that symbol directly bypasses painting. Only a real reset-vector
boot establishes the baseline. Never call the painter on a running target.

Inspect the **final linked** vector, painter, literal pool and normal startup
with the matching toolchain's `arm-none-eabi-objdump` before accepting a new
compiler/build. Ordinary C startup is unchanged when measurement is OFF, as
is the independently compiled generic BSP even when FTP measurement is ON.
PRIMASK cannot mask NMI/HardFault. This is a reset-only baseline with valid SRAM.
those default handlers do not return. A fault/NMI during startup is not a valid
measurement run. No runtime repaint, target scan, or target printing is added.

Release retains `-Os`. Only the instrumented platform, lwIP, FTP TCP/core and
RAM-file targets receive `-fstack-usage`. Find their `.su` files under
`build/ftp-stack/**/CMakeFiles/`. These are compiler per-function frames, **not
summable whole-program peaks**: prebuilt newlib/libgcc, hardware exception frames
(32 bytes plus possible alignment), nesting, indirect calls and unwritten frame
slots are not established by them. The naked painter's reported zero is only
its software frame size.

### Authorized no-reset capture. Not part of offline validation

**Do not execute this section during offline validation.** Obtain separate
approval for a specific reset-vector boot, workload and no-reset readout.
Previous experiment approval is not permission for a new run. This recipe neither
flashes nor establishes which image is installed. Record its hash, workload and
idle duration. Additional firmware or hardware known-write controls require
separate approval. Stop any other debug server/console helper first. Do not use
`scripts/probe.sh`, link/console reset helpers, `program`, `reset`, target calls,
or target-side algorithms. Reset destroys the historical watermark.

1. Audit the installed OpenOCD `target/stellaris.cfg` and interface script before
   attaching. The example below uses the repository board config, disables all
   server ports, overrides its working area to zero **before init**, and replaces
   attach/detach, examine and reset hooks with explicit nonempty no-op bodies.
   In particular, retain the `reset-start` override: the installed Stellaris hook
   can read/write peripheral registers and change reset policy. Empty `{}` attach
   bodies are insufficient: `init_target_events` installs a default `halt 1000`
   when the body is empty. GDB remains disabled. Do not add connect-under-reset options.
   If a different OpenOCD/config needs reset or RAM writes to attach, stop.
2. Within the approval above, capture from the repository root into a **new**
   directory (not an existing dump). This halts without resetting and leaves the
   target halted. It changes debug state and compromises tick/timing evidence:

   ```sh
   (
   set -eu
   capture=$(mktemp -d "$PWD/build/ftp-stack/capture.XXXXXX")
   openocd -f tools/openocd/board/ek-lm3s6965.cfg \
     -c 'gdb_port disabled; tcl_port disabled; telnet_port disabled' \
     -c 'reset_config none' \
     -c 'lm3s6965.cpu configure -work-area-size 0; foreach event {gdb-attach gdb-detach examine-start examine-end reset-start reset-end reset-init} {lm3s6965.cpu configure -event $event {# no-op}}' \
     -c "set capture {$capture}" \
     -c 'init; halt; wait_halt 5000; reg; foreach name {msp psp control primask xPSR pc sp lr basepri faultmask} {reg $name force}; dump_image $capture/stack.bin 0x2000e000 0x2000; shutdown' \
     > "$capture/openocd.log" 2>&1
   # Require OpenOCD exit 0 before scanning; retain the complete log.
   python3 scripts/ethernet/platform/stack_watermark.py "$capture/stack.bin"
   )
   ```

   Bare `reg` discovers/logs available register names and cache status. It is
   **not acquisition evidence**. The following loop explicitly uses `reg NAME
   force` to bypass the cache for MSP, PSP, CONTROL, PRIMASK, xPSR, PC and the
   additional SP/LR/BASEPRI/FAULTMASK context. Names are case-sensitive: use
   `xPSR`, not `xpsr`. Any unavailable name or failed forced read aborts the Tcl
   command before `dump_image`. Nonzero OpenOCD exit aborts the shell before
   scanning. Do not substitute guessed numeric indices, ignore errors, or accept
   cache-only values. Retain the complete log, including all forced-read values.

   Installed OpenOCD 0.12.0 documentation (`share/info/openocd.info-2`, `reg`)
   verifies forced-read semantics. Its executable's ARMv7-M register strings
   corroborate `msp`, `psp`, `control`, `primask`, `xPSR`, `basepri`, `faultmask`.
   the installed docs use `pc`/`sp`, and the executable also contains `lr`.
   These are offline checks, not proof that this target exposes every name:
   discovery and successful forced reads at capture time are mandatory. The
   executable's `set_default_target_event` checks for an empty string. Nonempty
   comment handlers survive `init_target_events` (verified without `init`).
   The command/config was audited offline, **not tested against a probe**.
   Audit changed versions again. Record whether xPSR's IPSR field (bits 8:0)
   indicates an ISR/exception. CONTROL and both stack pointers provide context.
   An attach/reset failure invalidates the capture. Do not retry via reset.
3. Retain raw dump, forced registers, ELF hash, exact bounds, workload and scanner
   output as a **qualified written-watermark baseline**, not a validated end-to-end
   hardware capture path. Synthetic scanner fixtures and linked disassembly
   verification are offline evidence only. An on-device known-write positive
   control has **not been performed**. This limitation must accompany the result,
   but it is not a prerequisite to recording the approved qualified baseline and
   does not authorize an extra firmware image or workload.

### Meaning and limits

The scanner requires **exactly 8192 bytes and the exact bounds**, comparing
explicit little-endian bytes `71 9e c3 a5` from low to high addresses. The first
mismatch determines `0x20010000 - first_mismatch`, the **written watermark**.
It handles unaligned writes, isolated mismatches and sentinel holes above them.
it does not count changed bytes or stop at a hole scanning downward.

This is **not exact peak stack use or historical minimum MSP**. Startup, idle
and any SysTick activity since reset are included. Allocated-but-unwritten slots,
sentinel collisions and unexercised paths can conceal deeper use. A pristine
dump says no mismatches, not that the stack never moved. An untouched prefix is
not a proven safe margin. A mismatch at the bottom reports the full reservation,
not proof that overflow occurred (nor assurance that it did not). No measured
runtime margin, hardware capture, traffic result or worst-case safety is claimed
by this offline baseline.


## Public source fingerprints

Generate source-only manifests without a compiler, Git, network or private
inputs. Python 3.9+ standard library is sufficient:

```sh
python3 scripts/boot_fingerprints.py --output /tmp/boot-fingerprints.json
python3 scripts/boot_fingerprints.py --profile sequence --output /tmp/sequence-fingerprints.json
```

The default root is anchored to the script. `--root` selects another checkout.
Explicit relative arguments resolve from the caller. Output parents must exist.
Omit `--output` for stdout. JSON is sorted, indented and newline-terminated, with
no timestamp. Success exits 0, invalid or unreadable input exits 1.

`physical-root` is the default profile. It recursively selects `.c`, `.h`, `.S`,
`.cmake`, `.txt` and `.ld` under the source, public include, config, CMake,
Mbed TLS include/library, physical-root fixture, FreeRTOS and example trees,
plus the script's exact `REQUIRED_FILES`. `sequence` selects public/config/
Mbed TLS C and header files, direct `src/boot_*.c` and `src/boot_*.h`, and its
explicit required files including `docs/boot-startup.md`. Read
[`scripts/boot_fingerprints.py`](../scripts/boot_fingerprints.py) for exact scopes.
Required inputs must exist. Absent glob directories retain empty-match behavior.
Generated JSON is not implicitly selected. Known private paths and source
symlinks are rejected, not followed. Output cannot overwrite hashed inputs,
including hardlink aliases, or become an allowlisted source.

Optional repeatable `--tool NAME=PATH_OR_COMMAND` resolves unique labels through
PATH without executing tools. Repeatable `--tool-bundle PATH` hashes every file
in explicitly selected public installations, not just executables. No tools are
discovered by default. Missing/nonexecutable tools, missing/empty bundles,
repository-containing bundles, directory symlinks, escaping file symlinks and
private paths fail closed. In-bundle file symlinks hash resolved bytes. Output
cannot reside inside a requested bundle. Review disclosed local paths before
publication. Never select a home directory, repository or secret store.

Importing `source_fingerprints(root, profile="physical-root")` from
`scripts.boot_fingerprints` returns the sorted source mapping directly, not the
CLI outer object. The one-argument API preserves default scope. Unknown profiles
raise `ValueError`. `tool_fingerprints(tools=None, bundles=())` returns the
`tools` and `tool_bundles` mappings. `sha(path)` streams file hashes. APIs raise
`OSError`/`ValueError` on invalid inputs and imports have no generation or tool
discovery side effects.

A manifest identifies selected bytes, not test success or trust in tools.
Optional hashes cannot replace runner-owned compiler/version/bundle checks,
mandatory host tools or before/after continuity. Keep baseline equality checks.
The utility has no `--check` mode. Documentation byte changes do not authorize
hash-only proof renewal. Both `docs/boot-startup.md` and
`docs/boot-reset-image.md` remain stable required paths for their respective
runner scopes. Existing accepted identities are historical after this cleanup.

## HOST integrity runner

From the repository root, with installed dependencies and tools only:

```sh
python3 tests/harness/runner.py --all --budget-seconds 900 --failures-only
```

This is offline but performs builds and QEMU execution. No device, flash,
live peer or download mode exists. Use repeatable `--case target:name` instead
of `--all` for exact selection. The [manifest](../tests/harness/manifest.json)
is the authority for the 37 CTest, 6 native and 13 Python case universe.
`--all` runs 55 representatives: `lwip-boot` includes the RAM assertions of
`lwip`, whose redundant execution is merged but remains explicitly selectable.
Selection is validated before build/artifact setup. Do not routinely run both
this full roster and overlapping Ethernet rosters.

Ordinary stdout is JSONL. `--failures-only` hides passing stdout records, not
complete reports. `--human` sends readable logs to stderr. Direct CTest, Python
and legacy shell commands remain raw developer interfaces. Each top-level case
has its own process group. Product failures permit later cases. Infrastructure
failures, missing tools and skips invalidate remaining execution. One deadline
includes preparation and kills/reaps timed-out process groups. No retries occur.

A fresh Release HOST build uses ASan/UBSan and test-only `-UNDEBUG`. Libraries
retain Release `NDEBUG`. Native peers remain O1 and linked boot libraries are
Release. Preparation batches native binaries via the build-only shell path,
including an incremental fixture build. Python methods run separately, except
nested CTest suites such as workflow's 23 methods and mocked FTP transport,
which are not double-counted. Debug is not reverified or declared equivalent.
The startup target has 20 fresh QEMU processes, not 20 independent CTests,
covering A/B, Os/Og, positive continuity and clock rejection. Model assertions
check incomplete evidence preservation and exactly one feed on completion.

The runner checks the exact CTest roster, commands, compiler flags and fixtures
before behavior. Fresh build/native directories are used on every invocation.
Artifacts live under ignored `.dev-vault/tests/runs/<timestamp>-host-integrity/`:
complete logs/reports, plans, provenance, CTest/JUnit and budget validity.
Only marked runner-owned runs are pruned, retaining five. Source drift marks
`INVALIDATED` and withdraws current-tree credit, not historical execution facts.
Provenance reads exclude `resume.tex`, private vault, `.pi`/Git, build trees
and Python caches. Symlinks are hashed as link text, never followed into private
files. Git status still detects excluded-path drift. Exclusions grant no
publication permission.

Offline configuration and command-evidence regression, without builds or QEMU:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/harness -p 'test_config.py' -v
```

This checks roster/selection, exclusions before reads, symlink privacy and
mocked startup command evidence, partial output, return codes and overflow.
It is a separate gate, not a product-manifest case or a syntax-only substitute.

A pass means the top-level execution completed its assertions. Crashes do not
prove later scenarios. Nested methods/subtests are not separate rollup cases.
Failure records retain the first recognizable assertion/sanitizer diagnostic,
or an evidence-log location when no source assertion exists. Startup commands
persist metadata before launch, separate stdout/stderr (1 MiB per stream),
combined logs and return/timeout/launch-error state. Overflow is invalid, never
silently accepted. `STARTUP INFRASTRUCTURE:` is invalid, not product failure.
A timeout has no fabricated return code. Skipped unittest methods invalidate
even a successful enclosing CTest. Workflow's `Ran 23 tests` completeness check
applies only to zero exit. Nonzero assertion exits remain failures. Missing or
malformed JUnit, truncation, missing tools and skips prevent acceptance. Target
and rollup summaries retain pass/fail/invalid counts, verbose CTest capture and
10 MiB passed/failed output limits. Historical runs do not verify a revised
runner. Mock storage/network, synthetic heaps and QEMU are not physical timing,
live transport, target-fit or hardware acceptance.

## Public-only signed verifier fixtures

[`verify_fixtures.h`](../tests/boot/verify_fixtures.h) contains 14 canonical
160-byte manifests independently signed with the external demonstration key
whose public root is described in [trust roots](boot-package.md). No private
key, nonce or signing implementation is embedded. Production verification uses
`config/boot_demo_public_key.h`, with no test-key override API.

Fixture index is `slot * 7 + mode`, A=0 and B=1. Base is A=`0x8800`,
B=`0x24000`. Length is 111616 for mode 1, otherwise 236. Reconstruct bytes:

1. For offset `i` in `[0,length)`, byte is `(i * 17 + slot * 31) & 255`.
2. Set `[100,108)` and `[length-4,length)` to `0xFF`.
3. Store the vector words below at offsets 0 and 4 as little-endian uint32.

| Mode | Initial MSP | Reset vector | Stored validation |
|---:|---|---|---|
| 0 | `0x20010000` | `base+9` | Valid minimum |
| 1 | `0x20010000` | `base+9` | Valid maximum |
| 2 | `0x2000FFFC` | `base+9` | MSP error |
| 3 | `0x20010000` | `base+8` | Thumb error |
| 4 | `0x20010000` | `base-1` | Target below payload |
| 5 | `0x20010000` | `base+length+1` | Target at exclusive end |
| 6 | `0x20010000` | `base+length-1` | Valid last halfword |

All signatures are valid, even for invalid vectors. Release is `UINT32_MAX`
(label only), flags/reserved zero, product/version 1, manifest length 160.
The hash is standard-library SHA-256 over the exact public payload. Prefix
fields use Python `struct.pack('<8I',...)`, independently of the project codec.
Historical independent generation used OpenSSL 3.6.3 and Python in an external
temporary directory, with no network. Only OpenSSL read the external key.
Python hashed the canonical 96-byte prefix once. Signing and independent public
verification used `openssl pkeyutl` with `-pkeyopt digest:sha256`, without
`-rawin` or another message hash. DER integers became unsigned 32-byte big-endian
`r || s`. Only public prefixes and signatures were retained. Re-signing need not
reproduce ECDSA bytes. These stable fixtures are not executable firmware,
transport packages or permission to sign or flash.

`test_verify.c` covers A/B min/max, invalid vectors, every manifest-byte mutation,
scalar boundaries, short manifests, malformed geometry/domain/version/reserved,
physical mismatch, payload corruption, reader bounds, progress/cancellation,
and unchanged outputs. Admission can accept signed invalid vectors but stored
validation rejects them. Invalid admission performs no reader calls, and
structurally invalid admission performs no precrypto progress events.
A separate host-only crypto archive substitutes allocation symbols without
changing production configuration. It counts outstanding allocations and samples
96 failures per slot: first/last 16 plus 64 evenly spaced positions, with
intentional endpoint repetition. Each failure must return CRYPTO, preserve
output, read no payload and free successful allocations. This is bounded
sampling, not exhaustive allocation failure or MCU heap/stack evidence.
Apple ASan lacks LeakSanitizer, so allocation accounting complements ASan/UBSan.
Linux additionally enables LeakSanitizer through existing configuration.
Storage must remain stable during verification until use. No lock, transaction
or TOCTOU protection is supplied here. Progress cannot interrupt ECDSA or
justify unconditional watchdog feeds, latency or 32 KiB boot fit. Arm archive
compilation is not final boot linking or hardware evidence.

## Focused boot owner gates

Owner-selected cases are not complete production acceptance. Consult each
runner's `CASES` and use a bounded budget where required:

```sh
python3 tests/boot/run_wdt_setup.py --case gate-order --case gate-ignored --case gate-exact --case arm-closure
python3 tests/boot/run_producer.py --case producer-success --case producer-overflow --case producer-admitted-failure --case producer-arm-closure --budget-seconds 600
python3 tests/boot/run_publication.py --all --budget-seconds 600 --failures-only
```

Sequence and physical-root runners support repeatable `--case` and require
`--budget-seconds` in 1..600. Examples include `capture-fullword-mismatch` and
`fresh-clear` for sequence, `physical-map`, `root-rebuild` and `verifier-roots`
for physical-root. Their `--all` rosters remain unchanged.
Shared-sequence, checked-handoff and physical-root runners default to the
current user's official 15.3.rel1 toolchain under `~/.toolchains/`.
`BOOT_ARM_GCC=/absolute/path/to/bin/arm-none-eabi-gcc` selects another installed
official bundle. Empty, relative, absent or nonexecutable overrides fail without
fallback. Sibling tools come from the compiler directory, not PATH. Existing
version/bundle and `/usr/bin/clang` checks remain mandatory. No dotenv or download
is used. The focused regression is
`python3 -m unittest discover -s tests/boot -p test_arm_compiler.py`.
Foreground clock/owner cases live in
[`tests/ethernet/foreground/runner.py`](../tests/ethernet/foreground/runner.py).

## Stack visualization

Open [the stack measurement visualization](stack-measurement.html) for the distinction between reserved space, written watermark and minimum stack pointer. It does not contain a current measurement result.
