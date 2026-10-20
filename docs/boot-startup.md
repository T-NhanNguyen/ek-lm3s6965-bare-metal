# Bounded application startup continuity — opt-in boundary

This is reusable startup source, **not a production boot caller or updater**.
The default standalone Ethernet/FTP path retains its cold PLL setup and
reinitializable SysTick clock. The opt-in foreground path now switches from a
startup-only GPTM to application SysTick as described below. A setup-only
physical WDT/Timer2 producer and physical RESC/flash/reader adapters now exist.
An experimental [boot-only reset image](boot-reset-image.md) invokes the shared
sequence once and stops at the decision, without dispatch. It is default-OFF,
not deployable: integration still requires authorized physical layout/write
inputs, deployment-grade trust and timing evidence. The compact-crypto Release
image now has bounded **32 KiB** static fit (31100 bytes, 1668 spare); the older
overflow remains historical, not current. These
adapters do not establish production readiness. Complete producer shutdown,
candidate 60-second deadline intent/reset actuation, and accepted-reset
actuation/grace remain **DEFERRED**. There is no new grace constant or enabled
inert deadline API. QEMU executes real ARM/core/startup code with explicitly
modeled WDT, RCC/RCC2/time and absent peripheral sources, not physical timing.

## Offline LED payload fixture (default OFF)

The [slot-A breathing LED fixture](../examples/boot-breathing-led/README.md)
uses the existing inherited application entry and retained-region linker
contract without changing launch/handoff or enabling interrupts. It does not
provide the Ethernet/timer/FTP progress required by the foreground watchdog
owner, so it deliberately does not feed and is **not hardware ready**. Current
decision-only boot cannot dispatch it. The corrected offline gate retained
**7 passing outcomes, five logical representatives**: actual 2784-byte application
producer fidelity, pure host PWM envelope, release-42 selected-root signature,
native real C acceptance/two tamper rejects, and fresh 31100-byte boot static
closure. These are not physical tests or sustained breathing acceptance.
The rebuilt root SHA256 `960d06c1aaa1206cf37ce8e8b0cd002dfa441b9324e25c8c5a0735651f12c389`
is now installed with the corrected FTP foreground A44/B45 pair and metadata,
not the historical standalone A42 payload. One SYSRESETREQ/continue captured
state7 and successful verified A44 RUN decision; see the
[installed outcome](boot-fixture-metadata.md). This is not dispatch or retained
startup/application-initialization acceptance. Physical LED/network execution,
B fallback and watchdog timing remain pending. No startup continuity or
watchdog-owner contract was relaxed; no manual feed/freeze was added. Bounded
application dispatch is next, not implemented by this documentation. See the
fixture README for retained historical A42 artifacts and producer correction.

## Owned native SRAM ABI

Boot/A/B linkers reserve exactly `[0x2000DBC0,0x2000DFC0)` as 1024-byte NOLOAD
`.boot_continuity`. Ordinary SRAM/remaining-capacity heap shrinks **57280 → 56256
bytes**, a 1024-byte cost (1.5625% of 64KiB). Diagnostic64 at DFC0..E000,
stack8192 at E000..10000 and MSP `0x20010000` are unchanged. `_heap_end` is
`_continuity_start`; production generic `_sbrk` enforces it. Unknown retained
allocated inputs still reject; no arbitrary `.noinit*` is accepted. The linker
creates an **absolute symbol**, not a defined C object or file-backed input.
GNU ARM accesses that symbol through the declared `boot_startup_t` lvalue.

| Offset | Native ownership |
| --- | --- |
| 0..63 | Header, state, immutable entry epoch, clock budget, dispatch/entry origins, callback reentry guard |
| 64..127 / 128..159 | Original watchdog56 / original publication24, slot padding |
| 160..287 / 288..319 | Authenticated image120 / handoff intent32 |
| 320..383 / 384..447 | Clock descriptor / retained IO context (64-byte maximum) |
| 448..511 / 512..1023 | Entry observations / reserved zero512 |

ARM `_Static_assert`s pin every offset, size, alignment, pointer32, enum32 and
lock-free guard assumption. Compile shared native consumers with
`-fno-short-enums`. **Measured correction to design estimate:** manifest112 +
vectors8 = image120, not116; the allocated128-byte slot/overall1024 ABI is
unchanged. HOST objects use native host sizes and do not emulate pointer32.
Version/ABI mismatch rejects; there is no migration/adoption of live objects.

## Setup-only physical producer (pre-verifier)

`boot_startup_producer_begin(c, diagnostic, reset_flags, system_hz)` uses the
existing direct volatile read/write/delay helpers and explicitly reserves full
Timer2 (`BOOT_STARTUP_PRODUCER_TIMER == 2`). Its `_begin_io` seam accepts the
existing IO descriptor plus gate delay, not a new backend framework. Both are
registered in the opt-in `lm3s6965_boot_startup` archive; neither is called by
startup, main or a launch wrapper.

Call **once per genuine reset, before verification**, in interrupt-excluded
privileged Thread mode, with exclusive reset/default WDT and Timer2 ownership.
No live-controller adoption or retries, including after failure. Reset/default
WDT debug STALL policy is a caller precondition; this setup does not change it.
The caller supplies actual unchanged Hz and trusted captured reset flags, not a
nominal clock macro or frequency inferred from RCC. On ARM the capsule and
64-byte diagnostics must be at their exact reserved linker addresses.

The bounded order is: reject zero/overflow Hz and structural arguments before
any effect; `boot_startup_reset` clears retained context and publishes fresh
diagnostics; establish WDT gate readiness; claim Timer2; publish the claim's
observed RCC/RCC2 with full-width masks; configure **the same `c->watchdog` once**
with checked `5 * Hz`. Success leaves state EMPTY and boot owner/epoch1; pass
`&c->watchdog` unchanged to the real verifier/runtime wrapper. Subsequent real
verification/service advances its original history and epoch. There is no
sampling loop, feed, reconfiguration, verifier, prepare or dispatch here.
Existing GPTM matches reject later RCC/RCC2 drift; frequency truth remains a
caller obligation.

Structural/Hz rejection intentionally leaves all bytes, including potentially
stale diagnostics, untouched: false is never permission to use retained state.
Any admitted failure quarantines FAILED/blocked with evidence cleared, without
peripheral cleanup, rollback, retry or reset actuation. Partial hardware effects
may remain. Caller must stop on every false. One-shot use is a genuine-reset
call-site contract, not a retained magic/reset classifier.

The IO context is exactly `c->context`; GPTM occupies its prefix (ARM40/HOST64),
while direct WDT IO is stateless and shares the same base without extra storage.
Descriptors/delay arguments are copied, never retained as stack pointers. Any
injected mutable state must fit outside the GPTM prefix (no room on HOST64).
All transitive callbacks/helpers/literals, including compiler/libc helpers,
require immutable boot-FLASH placement and retained-only mutable dependencies;
no old-stack or ordinary boot/app data/BSS dependency may survive entry. Final
consumers must audit link/map/disassembly closure and fit. This bounded API is
**not a full production boot entry or resource/physical timing guarantee**.

## Shared boot sequence — terminal RUN only

The opt-in `lm3s6965_boot_sequence` archive exposes `boot_sequence_init` and
`boot_sequence_run` in `lm3s6965/boot_sequence.h`. A trusted boot-only caller
initializes one fresh sequence per genuine/model boot, not the continuity or
watchdog, and supplies the existing producer, reset-register, store, image and
reader contracts. Simulation and physical IO use the same runtime/selector/
verifier decisions; no simulated authentication or alternate policy is added.
No generic reset handler, application entry or main calls this sequence.

The sequence validates known spans/alignment, callbacks, context starting
addresses and checked Hz before IO, snapshots descriptors, and consumes itself
before the first callback. It performs a read-only full reset-word observation,
then producer `_begin_io`, then `boot_runtime_watchdog_run` with **the same
`&c->watchdog`** and a private checked reset adapter. The first runtime capture
performs a **new underlying read** and compares the entire word with the early
observation. Read error or any mismatch follows existing capture failure before
clear/store, without publishing captured flags. Subsequent reads are fresh and
uncompared, preserving existing fresh-read clear semantics and failure reports.

`ARGUMENT`/stage `NONE` permits structural correction; every admitted result is
terminal, including inner preflight rejection. Reentry is `BUSY`, duplicate use
is `CONSUMED`, both without IO. All failures leave output unchanged; do not use
possibly stale continuity after early failure. The nested runtime result is
valid **only at stage `RUNTIME`**, and its own valid bits still apply: zero-valued
OK fields do not prove execution. No cleanup, retry, watchdog copy/reinit,
reconfiguration, extra feed or budget restart is introduced.

Caller establishes privileged masked Thread mode, genuine-reset/exclusive
ownership, truthful stable clock and stable metadata/images. Sequence/bridge and
checked-reset context are boot-local outside continuity and live through the
synchronous call. Opaque context sizes/nonaliasing remain caller obligations;
retained callbacks still require boot-FLASH closure and retained-only mutable
dependencies. The shared64 context is not wrapper scratch. Success leaves
continuity EMPTY and publishes only a terminal owned RUN decision, **not**
launch/handoff/prepare/dispatch permission. Do not chain `_launch` afterward.

Physical RESC/flash/reader adapters and descriptor assembly are available in
`src/boot_target_io.c`. Wiring a production reset caller, authorizing the public
root and board clock/layout, deployment, physical timing and complete protected
32 KiB link/fit remain separate work.
Default verifier builds still use the explicitly nonproduction demo public root;
the opt-in reset image requires an explicit selected public root. The retained
breathing-root build is offline evidence only, not the installed board identity.
Archive compilation is not verification acceptance or a production boot image.
Test execution/acceptance is deferred to test-merit-gate.

## Reset versus entry, and consumable provenance

On every **genuine boot reset**, call `boot_startup_reset` before retained use.
It invalidates magic, explicitly clears fresh storage, constructs the original
watchdog in-place and invokes fresh `boot_diag_publication_boot`: stale valid
health7/availability is not adopted. Establish truthful hardware clock/gating,
then configure this watchdog once **before real verification**. Never use magic
as a reset classifier. Application entry does not reset either object/sidecar or
diagnostic words.

The trusted boot call site executes real `boot_runtime_watchdog_launch`, requires
actual returned OK, then prepares from the published owned output/report. The
logical terminal callback MUST return. Physical PRIMASK exclusion remains held
although software guards have been released; release is a caller postcondition,
not an atomic_flag peek. Authentication, current authority, exclusive stable
image storage and actual successful-return provenance are trusted-firmware
contracts, not forgeable C-struct attestation.

`EMPTY → PREPARED → DISPATCHING → ENTERED → C_READY` is one-shot. Every admitted
error, duplicate, callback reentry or failed publication consumes to FAILED and
stops without feed/retry/adoption. PREPARED selection alone never supplies running
evidence. `prepare_handoff` is a deliberately narrower minimized-composition seam:
only after real stored verification, authoritative metadata identity match and
successful real `boot_handoff_run` return. It is NOT full reset-accounting/runtime
link evidence. A transferred APPLICATION owner with failed terminal is rejected.
No metadata/reset IO, replayed quiescence, second transfer or service is introduced.

The physical tail completes VTOR/barriers, installs a boot-flash return trap,
switches MSP, and then performs **only ISB, CONTROL/mask register writes and BX**:
no memory, stack, calls, old frames or return after switch. App vector word1 names
`Application_Reset_Handler`; signed payload includes this stub. It captures all
incoming core observations, exact local `Application_Entry_Witness` PC, local
Thumb stub identity and live VTOR before excluding interrupts/using the new stack.
Only DISPATCHING, exact stub/witness/core state and inherited clock may enter.
Early faults/NMI use a retained-only stackless failure handler, never app BSS.

The ordinary real data-copy, BSS-clear and `__libc_init_array` constructor path
then runs. Caller performs bounded app initialization and calls `boot_startup_c_ready`
only on actual success in masked privileged Thread mode. Foreground main orders
`ram_file_init` → `ftp_core_init` → `ftp_core_reset` → `c_ready` →
`net_clock_initialize`. Readiness validates the inherited clock/owner and
`sample.us >= entry_us`; this is not the final clock-switch sample.
APP=1/C_READY commits only after successful original-owner publication; no
Ethernet/FTP bits or WDT progress are synthesized. Initialization gets no blind feed.

## Lifetime, clock and running identity

Watchdog callbacks and read-only clock snapshot code, helpers/literals/tables and
all mutable dependencies must close over **boot flash `[0,0x8000)` and retained
context only**, not old stack/boot or app data/BSS/heap/newlib/TLS. Their contexts
must equal the capsule's64-byte context base. Address checks cannot prove
transitive lifetime: final consumers must audit closure/maps/disassembly, keep
boot flash immutable while callable, and bound callback storage use. The fixture
callbacks are closed leaf functions with model state entirely in that context.
No IO table/context replacement or watchdog copy/reinitialization occurs at entry.
Publication's words pointer is exactly DFC0 on ARM; unused word/barrier/user
pointers must be NULL, and the same associated owner/availability survives.

The retained clock callback must return **observed RCC/RCC2**, monotonic us,
source identity and established Hz, not an acceptance boolean or expectations
masquerading as reads. Masks/expected registers/source are trusted boot config.
Entry validates observed registers, nonzero source/masks, exact Hz and checked
`reload == 5*Hz` before C initialization, PLL/SysTick/peripheral/WDT mutations or
health publication. Oscillator frequency truth and startup timer validity remain physical boot
obligations; the setup producer is not wired to a production caller. App inherits the unchanged
system frequency/RCC/RCC2 and must not call the PLL routine. Application SysTick
configuration and named startup GPTM release are permitted clock-consumer changes,
not system-frequency changes.

`dispatch_us` precedes the terminal; `entry_us` is separate pre-C evidence.
These immutable origins are not restarted by the switch. The separate startup
offset below is entry-relative; future deadline policy must explicitly select
its applicable origin and include any required dispatch prefix. Backwards/failed
clock samples stop. No timer, intent commit or actuator is advertised as enforcing
60 seconds. Accepted update reset is a distinct sticky event: the approved
provisional policy is fixed 1000 ms after durable READY plus readback, without
traffic/reconnect extension or early sent-reply reset; actuation remains deferred.

`boot_startup_running` is an **app-linked executable wrapper** (snapshot callback:
zero success), checking its own function address in the entered physical payload
and live VTOR, backed by consumed exact entry binding and retained real verifier
result. A boot helper PC, metadata role, selected struct or VTOR alone cannot
establish execution. `_at` is a policy seam; trusted app wrapper supplies its own
address, never caller-supplied external claims. Failed startup is unavailable;
read-only output stays unchanged on rejection. Immutable `entry_epoch` is checked
through entry only; later successful service uses evolving controller epoch.
Identity survives service and metadata role changes. `boot_snapshot_capture`
loads current real authority and independently matches the actual running image.
This is trusted firmware consistency, not attestation against malicious signed
firmware, DMA or debugger modification.

## Startup-only GPTM → application SysTick

`include/lm3s6965/boot_startup_gptm.h` exposes the optional backend:
`boot_startup_gptm_claim`, `boot_startup_gptm_read`, identity-only
`boot_startup_gptm_supported`, and one-shot `boot_startup_gptm_release`.
Call claim **after** `boot_startup_reset` clears the shared context and **before**
verification. Select physical timer **0..3 explicitly** and supply actual system
Hz, exclusive ownership and trusted boot-FLASH/retained IO and delay closure.
There is **no Timer0 default**. The boot reader identity is retained across
separately linked app/boot copies; comparing it with the app-local address is not
provenance. Ordinary boot BSS/stack is not callback lifetime. Direct volatile IO
helpers are used by the setup-only Timer2 producer above; a production boot
caller remains unwired.

The backend uses a native 32-bit periodic down counter, with no retained 64-bit
wrap extension or ongoing/feed-time sampling. Claim clears stale status once;
uncleared sticky timeout RIS brackets TAR reads. Expiry before/racing the sample,
configuration/frequency drift or callback failure rejects without publishing time.
Synchronous callback reentry poisons the held operation; failures are terminal,
not retryable. Release consumes exact named ownership before validation and never
stops a guessed timer; it validates status/configuration without another TAR read.

The foreground transition in `examples/ethernet-ftp/net_clock.c` is:

1. Require masked privileged Thread C_READY and supported GPTM provenance; disable
   both external NVIC banks, clear pending external/PendSV/stale SysTick state,
   and reject unsafe faults/masks/pending sources.
2. Start a clean **1 kHz SysTick while masked**, barrier, then take the **last
   validated GPTM sample** with `boot_startup_time`. Commit the separate offset
   `floor((final_us - entry_us) / 1000) + 1` milliseconds, including conservative
   truncation allowance. Start-to-final-sample time deliberately overlaps the
   application ticks: user-approved early bias, not an omitted handoff gap.
3. Recheck masked state and safe pending-source exposure, then unmask. External
   NVIC sources remain disabled; only the initialized tick consumer is exposed.
   This is not proof of complete physical producer shutdown or NMI race freedom.
4. Initialize `lwip_init` and the netif, explicitly release the named GPTM, then
   enter the serialized foreground owner loop. No consumer reads GPTM after release.

`SysTick_Handler` only increments an aligned modular millisecond word; `sys_now`
is application-origin time, not inherited GPTM uptime. `net_clock_startup_elapsed`
returns the separate immutable startup offset for future deadline composition.
Modular elapsed comparisons require interval/observation gap below 2^31 ms;
arbitrarily long masking coalesces ticks and is not recovered. The initial tick
phase is below 1 ms; no strict physical reset deadline is promised.

The **same watchdog object, physical Hz/load (`5 * Hz`), history and live epoch**
survive the transition; immutable entry provenance stays distinct from evolving
service epochs. No WDT configure/reinit/retransfer/feed occurs in the switch or
ISR. `boot_startup_running_at` remains clock-free; only genuine foreground
progress permits service. NO_SYS/polled Ethernet is unchanged.

Claim/read/release remain a small extension point for future GPTM reuse under
new ownership, not revival of the old startup origin. `net_clock_port.{c,h}`
provides ordinary compile/link masked-thread, barrier and unmask operations;
actual MMIO/admission/time policy remain in `net_clock.c`. Future tick/task ports
must preserve serialization and ownership. There are no RTOS dependencies or
scheduler framework; standalone reinitialization remains supported.

### Current bounded verification, not full runtime acceptance

Final clock-switch gate: **28 CHECK records**, not 28 tests; **20 logical
representatives** = 13 native clock/GPTM + 1 HOST startup + 1 actual-main STATIC
+ 1 signed representative (2 configurations, 10 nested offline QEMU scenarios)
+ 4 root configuration pairs (8 ELFs). Thirteen other native owner/netif/FTP/TCP/
platform representatives were dependency-identity-reused historical evidence,
not new passes. Main order is static evidence, not execution of main/full services;
signed fixtures use modeled WDT/GPTM IO and idle service doubles, `-nic none`.

Valid minimized deployment fixtures are `bootOs/appOs-A` (boot load 32080/32768,
margin 688) and `bootOs/appOg-B` (32084, margin 684), all boot TUs final `-Os`.
The original B/`-Og` boot requesting 33988 bytes, overflow 1220, remains
**FAILED/unaccepted**; no all-Debug boot-fit or full production boot-fit claim.
The earlier 41-pass-check/32-representative gate predates the main-order
correction and is not full current acceptance. Physical timing/WDT/reset,
NMI races, missed ticks, full network runtime, peak resources and protected-wrapper
fit remain unproven. The historical startup evidence below retains its original
source/configuration scope and is not a current foreground matrix result.

## Historical startup source / test / evidence (STE)

| Contract | Source and intended gate |
| --- | --- |
| One-shot, provenance, terminal failure, no replay | `src/boot_startup.c`; `boot_startup`, extended `boot_runtime_watchdog`/`_fault` actual-wrapper return seam |
| Exact memory/native ownership | three linkers, header assertions; extended `boot_layout`, resource parser |
| Signed independent A/B entry, real C init, retained original callbacks | `src/startup.c`, entry/terminal assembly; `boot_startup_target` |
| Entry-backed running after feed and original-owner coherent snapshot | app wrapper, snapshot/store; target fixture and HOST availability tests |
| Clock/reentry/stale reset fail closed | HOST state matrix; real target fixtures label all peripheral/time IO modeled |

Two new CTest representatives: `boot_startup` and `boot_startup_target`.
The target representative builds four independently linked boot/app pairs:
A/B × startup `-Os`/`-Og` (crypto library fixed Release `-Os`), signs actual app
ELF loads using the existing external demo key, authenticates actual stored
payload, loads CRC/commit-last fixture authority, then runs five **fresh QEMU
processes per pair**: four positives (old MSP/PSP × representative inherited masks)
and one observed RCC2 mismatch at the pre-C sample,20 total.
Checks include poisoned real data/BSS, constructor effects, exact incoming MSP,
slot SVC dispatch, retained object/address/history/diagnostics, APP-only health,
original callback service after overwrite, stale epoch/second-transfer rejection,
running plus metadata snapshot after first feed and59999999/60000000 origin
boundaries. The negative fixture's separately named stop oracle checks still-poisoned
data/BSS, FAILED/unavailable running identity, unchanged diagnostics and no WDT
writes since dispatch. Production stop has no override hook. Boundary arithmetic
is not deadline enforcement.

Worker performed **builds only; no test execution**. Test-merit-gate owns all
execution/acceptance. Commands:

```sh
cmake -S tests/boot -B /tmp/application-startup-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/application-startup-build-debug -j4
cmake -S tests/boot -B /tmp/application-startup-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/application-startup-build-release -j4
# Build/link/sign only; no QEMU/static test assertions:
PYTHONDONTWRITEBYTECODE=1 python3 tests/boot/test_startup_target.py --build-only --output /tmp/application-startup-arm-final
# Execution, deferred to separately authorized test-merit-gate:
ctest --test-dir /tmp/application-startup-build-debug -R '^boot_(startup.*|runtime_watchdog.*|layout)$' --output-on-failure
```

Build-only map measurements (`/tmp/application-startup-resource-summary.json`):
`-Os` boot A/B flash30668/30672 bytes, app payload10208/10216;
startup `-Og` with crypto `-Os`: boot32260/32264, app11852/11856. Smallest
minimized-boot link margin is504 bytes, **not full-runtime headroom**. Retained
new boundary-unit text/rodata contributions (not a baseline delta) are boot1280,
app1090 bytes at `-Os`, boot1428/app1210 at `-Og`; unchanged startup and tiny
conditional default-handler changes are outside that unit-only count. Largest
fixture-main compiler frames: boot624, app536 bytes; these exclude transitive
callees and do not measure written-stack high-water.

Artifact maps/disassembly/size/stack-usage files are retained under the ARM build
output. Build-only fixture flash loads remain within boot32KiB and slot109KiB;
this is **not full bridge/updater fit**. Newlib enum-attribute warnings remain
because its archive uses variable-size enums; no enum values cross that libc
boundary. Actual startup consumers pin native enum32; vendor/newlib bytes were
not changed. Startup written-stack high-water and physical timing are not
measured here; compiler `.su` is not a whole-program stack bound. The standalone
FTP fixed8192 heap is unchanged; hypothetical9104→8080 spare after this reserve
is arithmetic, not measured updater margin. Full network/bridge/receiver/link,
heap/stack, hardware clocks/watchdog countdown/service gaps and reset-owner
acceptance remain open.
