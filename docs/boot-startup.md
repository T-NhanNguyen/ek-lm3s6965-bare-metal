# Startup continuity and watchdog ownership

Preserve the same configured watchdog, publication owner and retained context across authenticated application entry. Do not copy, reinitialize or blindly feed them. Boot/A/B startup reserves 1024 bytes of NOLOAD continuity. Cold standalone initialization remains a separate path.

## Find the contract

- [Retained SRAM and producer](#retained-sram-and-producer)
- [Shared boot sequence](#shared-boot-sequence)
- [Entry and running provenance](#entry-and-running-provenance)
- [Startup timer to application tick](#startup-timer-to-application-tick)
- [Watchdog register and progress contract](#watchdog-register-and-progress-contract)
- [Protected runtime and launch](#protected-runtime-and-launch)


## Retained SRAM and producer

### Owned native SRAM ABI

Boot/A/B linkers reserve exactly `[0x2000DBC0,0x2000DFC0)` as 1024-byte NOLOAD
`.boot_continuity`. Ordinary SRAM/remaining-capacity heap shrinks **57280 → 56256
bytes**, a 1024-byte cost (1.5625% of 64KiB). Diagnostic64 at DFC0..E000,
stack8192 at E000..10000 and MSP `0x20010000` are unchanged. `_heap_end` is
`_continuity_start`. Production generic `_sbrk` enforces it. Unknown retained
allocated inputs still reject. No arbitrary `.noinit*` is accepted. The linker
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
vectors8 = image120, not116. The allocated128-byte slot/overall1024 ABI is
unchanged. HOST objects use native host sizes and do not emulate pointer32.
Version/ABI mismatch rejects. There is no migration/adoption of live objects.

### Setup-only physical producer (pre-verifier)

`boot_startup_producer_begin(c, diagnostic, reset_flags, system_hz)` uses the
existing direct volatile read/write/delay helpers and explicitly reserves full
Timer2 (`BOOT_STARTUP_PRODUCER_TIMER == 2`). Its `_begin_io` seam accepts the
existing IO descriptor plus gate delay, not a new backend framework. Both are
registered in the opt-in `lm3s6965_boot_startup` archive. Neither is called by
startup, main or a launch wrapper.

Call **once per genuine reset, before verification**, in interrupt-excluded
privileged Thread mode, with exclusive reset/default WDT and Timer2 ownership.
No live-controller adoption or retries, including after failure. Reset/default
WDT debug STALL policy is a caller precondition. This setup does not change it.
The caller supplies actual unchanged Hz and trusted captured reset flags, not a
nominal clock macro or frequency inferred from RCC. On ARM the capsule and
64-byte diagnostics must be at their exact reserved linker addresses.

The bounded order is: reject zero/overflow Hz and structural arguments before
any effect. `boot_startup_reset` clears retained context and publishes fresh
diagnostics. Establish WDT gate readiness. Claim Timer2. Publish the claim's
observed RCC/RCC2 with full-width masks. Configure **the same `c->watchdog` once**
with checked `5 * Hz`. Success leaves state EMPTY and boot owner/epoch1. Pass
`&c->watchdog` unchanged to the real verifier/runtime wrapper. Subsequent real
verification/service advances its original history and epoch. There is no
sampling loop, feed, reconfiguration, verifier, prepare or dispatch here.
Existing GPTM matches reject later RCC/RCC2 drift. Frequency truth remains a
caller obligation.

Structural/Hz rejection intentionally leaves all bytes, including potentially
stale diagnostics, untouched: false is never permission to use retained state.
Any admitted failure quarantines FAILED/blocked with evidence cleared, without
peripheral cleanup, rollback, retry or reset actuation. Partial hardware effects
may remain. Caller must stop on every false. One-shot use is a genuine-reset
call-site contract, not a retained magic/reset classifier.

The IO context is exactly `c->context`. GPTM occupies its prefix (ARM40/HOST64),
while direct WDT IO is stateless and shares the same base without extra storage.
Descriptors/delay arguments are copied, never retained as stack pointers. Any
injected mutable state must fit outside the GPTM prefix (no room on HOST64).
All transitive callbacks/helpers/literals, including compiler/libc helpers,
require immutable boot-FLASH placement and retained-only mutable dependencies.
no old-stack or ordinary boot/app data/BSS dependency may survive entry. Final
consumers must audit link/map/disassembly closure and fit. This bounded API is
**not a full production boot entry or resource/physical timing guarantee**.


## Shared boot sequence

### Shared boot sequence. Terminal RUN only

The opt-in `lm3s6965_boot_sequence` archive exposes `boot_sequence_init` and
`boot_sequence_run` in `lm3s6965/boot_sequence.h`. A trusted boot-only caller
initializes one fresh sequence per genuine/model boot, not the continuity or
watchdog, and supplies the existing producer, reset-register, store, image and
reader contracts. Simulation and physical IO use the same runtime/selector/
verifier decisions. No simulated authentication or alternate policy is added.
Generic BSP reset and application entry do not call this sequence. The explicit
boot-only decision image is its caller. The separate `boot_sequence_launch`
entry composes launch intent for the opt-in dispatch image.

The sequence validates known spans/alignment, callbacks, context starting
addresses and checked Hz before IO, snapshots descriptors, and consumes itself
before the first callback. It performs a read-only full reset-word observation,
then producer `_begin_io`, then `boot_runtime_watchdog_run` with **the same
`&c->watchdog`** and a private checked reset adapter. The first runtime capture
performs a **new underlying read** and compares the entire word with the early
observation. Read error or any mismatch follows existing capture failure before
clear/store, without publishing captured flags. Subsequent reads are fresh and
uncompared, preserving existing fresh-read clear semantics and failure reports.

`ARGUMENT`/stage `NONE` permits structural correction. Every admitted result is
terminal, including inner preflight rejection. Reentry is `BUSY`, duplicate use
is `CONSUMED`, both without IO. All failures leave output unchanged. Do not use
possibly stale continuity after early failure. The nested runtime result is
valid **only at stage `RUNTIME`**, and its own valid bits still apply: zero-valued
OK fields do not prove execution. No cleanup, retry, watchdog copy/reinit,
reconfiguration, extra feed or budget restart is introduced.

Caller establishes privileged masked Thread mode, genuine-reset/exclusive
ownership, truthful stable clock and stable metadata/images. Sequence/bridge and
checked-reset context are boot-local outside continuity and live through the
synchronous call. Opaque context sizes/nonaliasing remain caller obligations.
retained callbacks still require boot-FLASH closure and retained-only mutable
dependencies. The shared64 context is not wrapper scratch. Success leaves
continuity EMPTY and publishes only a terminal owned RUN decision, **not**
launch/handoff/prepare/dispatch permission. Do not chain `_launch` afterward.


## Entry and running provenance

### Reset versus entry, and consumable provenance

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
although software guards have been released. Release is a caller postcondition,
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
`Application_Reset_Handler`. Signed payload includes this stub. It captures all
incoming core observations, exact local `Application_Entry_Witness` PC, local
Thumb stub identity and live VTOR before excluding interrupts/using the new stack.
Only DISPATCHING, exact stub/witness/core state and inherited clock may enter.
Early faults/NMI use a retained-only stackless failure handler, never app BSS.

The ordinary real data-copy, BSS-clear and `__libc_init_array` constructor path
then runs. Caller performs bounded app initialization and calls `boot_startup_c_ready`
only on actual success in masked privileged Thread mode. Foreground main orders
`ram_file_init` → `ftp_core_init` → `ftp_core_reset` → `c_ready` →
`net_clock_initialize`. Readiness validates the inherited clock/owner and
`sample.us >= entry_us`. This is not the final clock-switch sample.
APP=1/C_READY commits only after successful original-owner publication. No
Ethernet/FTP bits or WDT progress are synthesized. Initialization gets no blind feed.

### Lifetime, clock and running identity

Watchdog callbacks and read-only clock snapshot code, helpers/literals/tables and
all mutable dependencies must close over **boot flash `[0,0x8000)` and retained
context only**, not old stack/boot or app data/BSS/heap/newlib/TLS. Their contexts
must equal the capsule's64-byte context base. Address checks cannot prove
transitive lifetime: final consumers must audit closure/maps/disassembly, keep
boot flash immutable while callable, and bound callback storage use. The fixture
callbacks are closed leaf functions with model state entirely in that context.
No IO table/context replacement or watchdog copy/reinitialization occurs at entry.
Publication's words pointer is exactly DFC0 on ARM. Unused word/barrier/user
pointers must be NULL, and the same associated owner/availability survives.

The retained clock callback must return **observed RCC/RCC2**, monotonic us,
source identity and established Hz, not an acceptance boolean or expectations
masquerading as reads. Masks/expected registers/source are trusted boot config.
Entry validates observed registers, nonzero source/masks, exact Hz and checked
`reload == 5*Hz` before C initialization, PLL/SysTick/peripheral/WDT mutations or
health publication. Oscillator frequency truth and startup timer validity remain physical boot
obligations. The [experimental boot images](boot-reset-image.md#decision-and-dispatch-images)
provide explicit callers, not deployment or timing acceptance. App inherits the unchanged
system frequency/RCC/RCC2 and must not call the PLL routine. Application SysTick
configuration and named startup GPTM release are permitted clock-consumer changes,
not system-frequency changes.

`dispatch_us` precedes the terminal. `entry_us` is separate pre-C evidence.
These immutable origins are not restarted by the switch. The separate startup
offset below is entry-relative. Future deadline policy must explicitly select
its applicable origin and include any required dispatch prefix. Backwards/failed
clock samples stop. No timer, intent commit or actuator is advertised as enforcing
60 seconds. Accepted update reset is a distinct sticky event: the approved
provisional policy is fixed 1000 ms after durable READY plus readback, without
traffic/reconnect extension or early sent-reply reset. Actuation remains deferred.

`boot_startup_running` is an **app-linked executable wrapper** (snapshot callback:
zero success), checking its own function address in the entered physical payload
and live VTOR, backed by consumed exact entry binding and retained real verifier
result. A boot helper PC, metadata role, selected struct or VTOR alone cannot
establish execution. `_at` is a policy seam. Trusted app wrapper supplies its own
address, never caller-supplied external claims. Failed startup is unavailable.
read-only output stays unchanged on rejection. Immutable `entry_epoch` is checked
through entry only. Later successful service uses evolving controller epoch.
Identity survives service and metadata role changes. `boot_snapshot_capture`
loads current real authority and independently matches the actual running image.
This is trusted firmware consistency, not attestation against malicious signed
firmware, DMA or debugger modification.


## Startup timer to application tick

### Startup-only GPTM → application SysTick

`include/lm3s6965/boot_startup_gptm.h` exposes the optional backend:
`boot_startup_gptm_claim`, `boot_startup_gptm_read`, identity-only
`boot_startup_gptm_supported`, and one-shot `boot_startup_gptm_release`.
Call claim **after** `boot_startup_reset` clears the shared context and **before**
verification. Select physical timer **0..3 explicitly** and supply actual system
Hz, exclusive ownership and trusted boot-FLASH/retained IO and delay closure.
There is **no Timer0 default**. The boot reader identity is retained across
separately linked app/boot copies. Comparing it with the app-local address is not
provenance. Ordinary boot BSS/stack is not callback lifetime. Direct volatile IO
helpers are used by the setup-only Timer2 producer above. Boot images must
explicitly compose that producer under their own reset and ownership contract.

The backend uses a native 32-bit periodic down counter, with no retained 64-bit
wrap extension or ongoing/feed-time sampling. Claim clears stale status once.
uncleared sticky timeout RIS brackets TAR reads. Expiry before/racing the sample,
configuration/frequency drift or callback failure rejects without publishing time.
Synchronous callback reentry poisons the held operation. Failures are terminal,
not retryable. Release consumes exact named ownership before validation and never
stops a guessed timer. It validates status/configuration without another TAR read.

The foreground transition in `examples/ethernet-ftp/net_clock.c` is:

1. Require masked privileged Thread C_READY and supported GPTM provenance. Disable
   both external NVIC banks, clear pending external/PendSV/stale SysTick state,
   and reject unsafe faults/masks/pending sources.
2. Start a clean **1 kHz SysTick while masked**, barrier, then take the **last
   validated GPTM sample** with `boot_startup_time`. Commit the separate offset
   `floor((final_us - entry_us) / 1000) + 1` milliseconds, including conservative
   truncation allowance. Start-to-final-sample time deliberately overlaps the
   application ticks: user-approved early bias, not an omitted handoff gap.
3. Recheck masked state and safe pending-source exposure, then unmask. External
   NVIC sources remain disabled. Only the initialized tick consumer is exposed.
   This is not proof of complete physical producer shutdown or NMI race freedom.
4. Initialize `lwip_init` and the netif, explicitly release the named GPTM, then
   enter the serialized foreground owner loop. No consumer reads GPTM after release.

`SysTick_Handler` only increments an aligned modular millisecond word. `sys_now`
is application-origin time, not inherited GPTM uptime. `net_clock_startup_elapsed`
returns the separate immutable startup offset for future deadline composition.
Modular elapsed comparisons require interval/observation gap below 2^31 ms.
arbitrarily long masking coalesces ticks and is not recovered. The initial tick
phase is below 1 ms. No strict physical reset deadline is promised.

The **same watchdog object, physical Hz/load (`5 * Hz`), history and live epoch**
survive the transition. Immutable entry provenance stays distinct from evolving
service epochs. No WDT configure/reinit/retransfer/feed occurs in the switch or
ISR. `boot_startup_running_at` remains clock-free. Only genuine foreground
progress permits service. NO_SYS/polled Ethernet is unchanged.

Claim/read/release remain a small extension point for future GPTM reuse under
new ownership, not revival of the old startup origin. `net_clock_port.{c,h}`
provides ordinary compile/link masked-thread, barrier and unmask operations.
actual MMIO/admission/time policy remain in `net_clock.c`. Future tick/task ports
must preserve serialization and ownership. There are no RTOS dependencies or
scheduler framework. Standalone reinitialization remains supported.


## Watchdog register and progress contract

### Register contract and initial protection

The local *LM3S6965 Microcontroller Data Sheet*, July 15, 2014, pp. 371–382,
is the authority (not TM4C/Tiva registers):

- System clock drives WDT. RCGC0.WDT is bit 3 (pp. 214–215). Caller enables it, establishes
  peripheral readiness and the actual stable clock before configuration.
- Base `0x40000000`: LOAD `0`, VALUE `4`, CTL `8`, ICR `0xc`, RIS `0x10`,
  MIS `0x14`, TEST `0x418`, LOCK `0xc00`. Only LOAD/CTL/ICR/LOCK are written.
- Unlock writes `0x1ACCE551`. Lock writes `0`. **LOCK reads 0/1**, not the key.
  CTL is read-modify-written, preserving reserved bits, with INTEN and RESEN
  enabled **together**. After INTEN all CTL writes are ignored until hardware
  reset. An already enabled controller is rejected, not “repaired” or adopted.
- First zero sets the interrupt and reloads. Second zero before clear resets.
  LOAD reloads but does **not** clear an interrupt. Configuration writes LOAD,
  clears stale interrupt with ICR and checks RIS before enabling CTL. It locks
  and checks CTL/LOAD/LOCK before reporting protection established.
- Service writes ICR (any value clears interrupt **and** reloads), checks RIS,
  then relocks. ICR is write-only with indeterminate read value. No ICR readback
  or comparison of the changing VALUE to LOAD is fabricated. RIS clear alone
  cannot prove reload if it was already clear: success callbacks must truthfully
  mean the write reached the register. A new first-stage event during readback
  can conservatively cause a fault. No cycle-exact observation is claimed.

`boot_watchdog_init` initializes fresh unpublished software storage exactly
once. It does **not** configure hardware. Call `boot_watchdog_configure` before
verification, and proceed only on `BOOT_WATCHDOG_OK`. Failure can leave the
controller disabled, partially configured, enabled, or unlocked. The software
`configured` flag is not a claim that a failed configure left hardware disabled.
An enabled INTEN-only controller cannot acquire reset protection without reset.

The caller supplies `actual_system_hz` and a nonzero finite `reload`. The policy
checks `reload == 5 * actual_system_hz` in 64-bit arithmetic with uint32 fit:
**nominal 5 seconds per stage, intended 10 seconds TOTAL**, never ten per stage.
This chooses an explicit nominal register budget, not a measured constant or a
LOAD-versus-LOAD+1 cycle-accurate guarantee. No clock is discovered from
`SYSTEM_CLOCK_HZ`/50 MHz macros. Synthetic test frequencies are not board
configuration recommendations. Physical clock accuracy, reload edge semantics,
service gaps, peripheral startup, clock inheritance and elapsed reset timing
remain unmeasured. The caller must validate them before hardware use.

### Ownership and evidence

All calls are foreground-only. The caller exclusively owns the controller and
object. Do not mutate fields, change clocks, run independent register writers,
alias inputs, or reinitialize to escape a fault. An atomic entry guard rejects
callback reentry/concurrent calls with BUSY. It does not serialize external
controller users or make ISR use valid. Callbacks return zero for truthful
success. Nonzero may already have affected hardware.

1. Boot begins at **epoch 1** after init/configure. Every report/service names
   the current epoch. Rejected reports/insufficient evidence cause no register
   access. A successful service consumes evidence and advances the epoch.
2. `boot_watchdog_boot_report` accepts actual existing verifier events with a
   caller-assigned nonzero monotonically increasing verification ID. BEGIN
   requires the verifier's checked total, resets stream/evidence and grants no
   feed. AUTHENTICATED supplies one milestone only after successful signature
   completion. PAYLOAD_PROGRESS must advance cumulative successfully read+hashed
   bytes by exactly the next verifier chunk (256 or all remaining bytes for the
   final chunk) within the unchanged total. Duplicate, incomplete, backwards, oversize,
   wrong-stream and stale-epoch reports fail. Several milestones before service
   coalesce into one permission, never a bank of future feeds. A new BEGIN can
   abandon a failed stream, discarding evidence. IDs must not recycle.
3. `boot_watchdog_transfer` makes a **single boot→application** software ownership
   transition, advances epoch and discards boot evidence. It performs no register
   operations/feed, including on epoch exhaustion. The enabled counter continues.
   old boot reports/services and repeat transfer cannot feed it. Physical transfer
   and application preservation of this software object are future glue, not
   supplied here. Application must not call init/configure again.
4. Application reports exactly one completed predicate at a time: Ethernet poll/
   drain, all due network timers, FTP state-machine work. All three are required
   for service. Each report is accepted once per epoch and `work_complete` must
   equal 1. Idle polls, no timers due and no FTP client count as completed service
   passes. **Client traffic is not required**. Work that is merely scheduled,
   entered, stalled or failed does not count. These are caller attestations,
   not automatic observations, startup health or confirmation bits.
5. Confirmation does not change this policy: the confirmed application remains
   protected with the same foreground obligations. There is no candidate
   60-second timer or metadata operation in the watchdog policy.

The optional [runtime verifier bridge](boot-startup.md) now composes
these actual callbacks through reset/selection in HOST scope, adopting only an
already configured controller and retaining all unmeasured intervals below.

A verifier callback bridge must use the **current epoch on each callback** and
one stable verification ID per invocation. Callbacks are scheduling points, not
blind feed instructions. BEGIN cannot feed during nonrestartable ECDSA. No new
callbacks or verifier rewrite are introduced. That indivisible operation and
all boot/flash/application startup work between milestones must fit the actual
budget. This API does not prove their timing or compose them automatically.

### Faults and limits

Any admitted register I/O/readback fault permanently quarantines the object,
clears evidence and reports failure. After an attempted unlock, best-effort
relock is cleanup only. Even successful cleanup never converts failure to
success. No CTL disable, LOAD/ICR retry or progress reuse occurs. Before unlock,
failures do not attempt unclocked watchdog cleanup. A failed feed may already
have reloaded, and failed lock/cleanup may leave writes unlocked. No software
“recover” API adopts this ambiguous state: reset/reestablish hardware under
separate integration authority. Caller must not continue verification/handoff
on failed configuration or report a failed feed as success.

Owner/epoch rejection and incomplete evidence are non-faulting, callback-free
rejections. Epoch exhaustion instead quarantines without wrap. C structs and
IDs are not security/provenance tokens: a dishonest caller can fabricate work
or repeat a signature with a new ID. Actual code must report genuine work.
No unconditional interrupt feed, indefinite progress bank, clock rewrite or
silent protection disable is provided.


## Protected runtime and launch

`boot_runtime_watchdog_run` wraps the unchanged one-shot [runtime](boot-architecture.md)
and real selector/launch/verifier callbacks. It returns an owned decision, **not
execution or handoff permission**. Use `boot_runtime_watchdog_launch` instead
for the complete selection-to-final-handoff intent transaction below. Neither
entry configures a controller, binds MMIO, installs an ISR or proves timing.

### Establish protection, then adopt it

1. Initialize fresh `boot_watchdog_t` and `boot_runtime_watchdog_t` objects once
   for this physical/model boot. The bridge owns its embedded raw runtime.
2. Establish actual stable clock, RCGC0 readiness and explicit finite reload.
   call `boot_watchdog_configure` and require OK **before calling the bridge**.
3. Exclusively own controller, bridge, adapters/contexts, metadata, images and
   output throughout the transaction and preserve decision stability until use.
   Call `boot_runtime_watchdog_run` once. Only wrapper OK publishes output.

The bridge preflight requires a configured, nonblocked BOOT owner with nonzero
reload/epoch and callbacks. Invalid protection causes no reset/flash/watchdog IO
and consumes no run. This is software admission, not a fresh hardware/clock
measurement or adoption of unknown inherited hardware. Caller-established clock,
readiness, correct configuration and truthful callbacks remain prerequisites.
Do not use raw APIs to create a parallel reset path, report fabricated work or
service independently. Do not copy/reinitialize/recreate either object to escape
consumption/quarantine. The existing watchdog stream counter is adopted, never
reset: any legitimate earlier stream history is retained.

Bridge entry is guarded before mutable input inspection. Known bridge/watchdog/
output spans must be mutually disjoint and disjoint from all input tables.
Opaque contexts must also be disjoint from mutable objects/output. Their sizes
are caller obligations. Inputs may alias other inputs. Register/store tables and
both manifest prefixes are copied before operations. Watchdog IO was copied by
watchdog init. Callbacks must not mutate these objects, inputs or output.

The bridge holds the **real watchdog's entry guard for the entire transaction**,
including reset/flash callbacks and final publication, not just individual
reports/services. External watchdog report/service/configure/transfer calls from
callbacks return BUSY. A private source-only held-operation seam reuses the same
watchdog report/service logic without releasing that guard. It is not a consumer
API. Caller exclusivity is still necessary: the guard cannot stop direct MMIO,
field mutation, copying or reinitialization.

### Complete launch intent

1. Configure the watchdog before reset capture as above.
2. Initialize a fresh `boot_handoff_t` with truthful logical handoff callbacks.
   Supply a current `boot_handoff_context_t` observation of privileged Thread mode.
3. Call `boot_runtime_watchdog_launch` once instead of the decision-only entry.
   Pass disjoint `boot_runtime_watchdog_launch_t` output storage.

The launch entry uses the same bridge object and the same watchdog object.
It holds the bridge, handoff and watchdog guards through output publication.
Nested runs and public watchdog operations return BUSY. Guards do not prevent direct
hardware access. Keep exclusive ownership of objects, images and controllers.
Keep opaque contexts disjoint from all mutable objects and output. The entry
checks known object, input and output spans for overlap. It copies the core
observation, reset/store tables and manifest prefixes before callbacks.

Only runtime OK enters the existing final-handoff transaction. The request uses
the selected verified image and the watchdog's **current** epoch after all real
services. It does not restore the entry epoch or reset stream history. Handoff
orders exclusion, SysTick stop, both interrupt-bank disables, source quiescence,
core and bank pending clears, the pre-VTOR barrier, selected VTOR and the
post-VTOR barrier. It transfers watchdog ownership once before terminal intent.
Transfer increments the current epoch without register IO or a feed. Terminal
intent sets exact MSP and reset vector with CONTROL and all masks zero.
The terminal callback records intent only. It must not execute the application.

Success publishes `decision` and `intent` together as owned values. The whole
output stays frozen through all callbacks and on every error. No handoff step
writes metadata or reports progress. READY-to-TRIAL, reset accounting and previous
promotion retain only their existing required writes before handoff.

Structural and preflight rejection permit correction without IO or consumption.
Every admitted runtime or handoff failure consumes the bridge and quarantines
both handoff and watchdog. Quarantine adds no IO. Stop on failure. Do not retry,
resume boot work or replay reset accounting. A terminal failure can follow a
recorded intent. It retains APPLICATION ownership and the advanced epoch.
Read-only authority reconciliation cannot authorize another launch in this boot.
The decision-only and launch entries share consumption. Do not chain them.

`HANDOFF` in the report makes the last handoff preflight or run result valid.
It does not mean handoff ran. Runtime failure can retain handoff preflight OK.
`HANDOFF_FAILED` means runtime succeeded but final handoff failed. Only wrapper
OK means the complete requested transaction succeeded. No default diagnostic
field or partial validity bit grants launch permission.

### Post-return startup continuity (opt-in)

Only after this function has actually returned OK may its trusted boot caller
pass the owned output/report to `boot_startup_prepare`, under continued physical
PRIMASK exclusion and stable authenticated image storage. Guards being released
is a call-site postcondition. No atomic_flag layout inspection is introduced.
A matching APPLICATION owner alone is insufficient (terminal may have failed).
[Startup continuity](boot-startup.md) consumes EMPTY → PREPARED → DISPATCHING →
ENTERED → C_READY once, with any admitted error terminal FAILED. Dispatch never
repeats runtime, quiescence, accounting, transfer or metadata writes.

The retained `entry_epoch` is immutable history, compared to the original
controller only through early entry. Later service uses the live watchdog epoch.
first successful foreground feed does not invalidate running identity. The new
code does not configure/feed during startup, choose a physical clock source,
enable the candidate deadline actuator or implement accepted-reset grace.

### Genuine milestones only

| Actual verifier event | Bridge action |
| --- | --- |
| BEGIN, after structural/target checks | Allocate `watchdog.verification + 1`, report actual checked total and zero completed. **never service** |
| AUTHENTICATED, after signature success | Report with current epoch and same ID/total, then service exactly once |
| PAYLOAD_PROGRESS, after successful exact read+hash chunk | Report actual cumulative completed bytes and unchanged total, then service exactly once |

Each successful service consumes evidence and increments the epoch. The next
callback reads that new epoch. IDs increase across candidate/current/previous
attempts and are unrelated to slot, metadata sequence or trial ID. BEGIN can
abandon a failed stream. A new ID cannot wrap. ID exhaustion returns watchdog
STATE before attempting BEGIN report. Epoch exhaustion follows existing watchdog
service semantics: STATE/quarantine with no register service or wrap. Exhaustion
inside verification aborts that operation. The final legal ID can finish one
stream. It cannot admit another stream. No pending evidence is banked: adoption
discards preexisting evidence, every service consumes it, and terminal failures
clear it.

No cancellation API is added. A watchdog report/service failure returns zero
through the existing progress callback chain: verifier ABORTED is operational,
so selector stops, never treats it as image-invalid fallback. Valid signature
mismatch/hash/vector invalidity retains the existing independent fallback policy.
A last successful chunk is **progress, not validation**: final hash/vector checks
or later reader/crypto/store errors can still fail after a good milestone.

### One-shot failure and diagnostic authority

Admission consumes the wrapper before any callback, and raw runtime consumes
itself before reset capture. Callback reentry is BUSY. Every admitted result is
terminal. Duplicates return CONSUMED with no IO/feed/reset replay. Structural or
watchdog preflight rejection alone permits corrected arguments.

Output stays frozen during every callback and all failures. Success publishes an
owned local decision only after runtime OK. The runtime result is preserved
unchanged: its stage `valid` bits, reset nested validity, previous statuses and
selector `evidence_mask` retain their existing meanings (not an attempt trace).

Wrapper `valid` bits are separate:

- `DIAGNOSTIC`: exact last attempted watchdog operation/result, including
  preflight rejection. Default watchdog OK without this bit proves nothing.
- `RUNTIME`: raw runtime was attempted. Inspect its own status and validity.
- `CALLBACK`: phase/epoch/verification/completed/total describe the last actual
  callback. Epoch is the epoch **used for the attempt**, before successful service
  increment. No callback is inferred from default phase/zero values.

A later runtime failure may retain last watchdog OK: this does not mean image
validity or watchdog failure. Report/service errors retain their exact cause and
operation, not merely the resulting verifier ABORTED. All admitted failures
quarantine the watchdog and discard evidence without additional register IO.
Service faults retain existing before-unlock stop or best-effort relock cleanup.
cleanup never retries a feed or converts failure to success. No automatic
reconciliation, retry, rollback or replay occurs. UNCERTAIN remains raw runtime
RECONCILE. Explicit external read-only authority reconciliation cannot authorize
rerunning this boot or servicing the quarantined object.

### Unmeasured intervals and later gates

Reset capture/accounting/flash before BEGIN, nonrestartable ECDSA, interattempt
work, final hash/vector checking, metadata commits and final publication receive
**no synthetic progress reports**. Their service gaps remain unmeasured. This
bridge cannot interrupt a stalled callback or signature calculation. Caller must
validate all these intervals against real hardware budgets before deployment.
Nominal 5 seconds/stage means intended **10 seconds TOTAL**, not a measured bound.
No physical register/clock/reset behavior, ISR/MMIO binding, startup/handoff,
application/FTP continuity, final dependency closure/link or **32 KiB fit** is
proved by HOST tests or archive compilation.
