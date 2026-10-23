# Boot architecture and diagnostic ABI

Boot captures and accounts a reset once, authenticates eligible images, commits any required authority change, then publishes an owned decision. A decision is not execution. Protected launch adds one-way watchdog transfer and terminal intent. Startup consumes that intent separately. Stop on ambiguous commits rather than replaying a cached transaction.

## Find the contract

- [Selection policy](#selection-policy)
- [One-shot runtime](#one-shot-runtime)
- [Candidate transaction](#candidate-transaction)
- [Handoff admission and ordering](#handoff-admission-and-ordering)
- [Target terminal contract](#target-terminal-contract)
- [Diagnostic wire and publication](#diagnostic-wire-and-publication)


## Selection policy

`boot_select` in `include/lm3s6965/boot_select.h` returns an owned verified image
and authoritative metadata snapshot only on `BOOT_SELECT_OK`. It does not jump,
provision, capture/account resets, feed/transfer a watchdog, or implement recovery
tooling. `boot_launch_candidate` remains the candidate-only transaction API.

### Caller boundary

1. Complete reset accounting with `BOOT_RESET_HANDLER_OK` **in this boot**, and
   supply that final authoritative snapshot (or explicitly reconciled authority
   after a later selection transaction). A failed reset report is never permission
   to proceed. Struct fields are forgeable, not provenance.
2. Establish watchdog protection before verification. Supply physical A/B manifest
   prefixes in `boot_select_images_t`, an honest physical payload reader and an
   optional verifier-progress callback. `size[slot]` is available prefix length.
   only the first 160 bytes are used. Short prefixes fail image format checks.
3. Exclusively own both pages, physical images, arguments, adapters and contexts
   through synchronous selection and decision use. No ISR, concurrency, callback
   reentry or mutation. Only requested metadata operations may change storage.
   Stable ownership is a caller precondition, not established by this API.
4. Use output only on OK while the image and authority remain stable. All other
   results, including RECOVERY and RECONCILE, leave output **unchanged** and give
   no executable decision. Physical handoff and application startup are separate.

The adapter, expected snapshot and both manifest-prefix inputs are copied before
any callback. Inputs may alias each other if still valid objects. Output must not
overlap any input or context storage. Known object spans are checked before IO.
opaque context sizes remain caller obligations. Callbacks must not modify output.
There is no mutable policy global, runtime lock, automatic retry or reentry guard.

### Selection and durable ordering

| Authority / evidence | Action |
| --- | --- |
| READY/TRIAL candidate | Attempt first through the existing real verifier/lifecycle/store candidate transaction. READY requires durable TRIAL/readback. TRIAL independently verifies and rechecks authority with zero writes. |
| Candidate IMAGE_INVALID | Independently verify identity-matching confirmed CURRENT. Do **not** count, reject, clear intent or modify any candidate metadata. Successful fallback publishes the exact original authority with zero writes. |
| No candidate, confirmed CURRENT valid | Independently authenticate/hash/check vectors and exact resident identity. Recheck canonical authority and selected page immediately before zero-write publication. |
| CURRENT image-invalid, eligible confirmed PREVIOUS | Independently authenticate/hash/check vectors and resident identity of PREVIOUS. Call `boot_lifecycle_promote_previous`, then require `boot_store_replace` OK and authoritative committed readback before publication. |
| Missing roles, neither valid, metadata absent/conflicting | Explicit RECOVERY: no executable decision, guessed factory image, automatic provisioning or candidate-as-previous promotion. |

Only FORMAT, TARGET, SIGNATURE, HASH, MSP, THUMB and VECTOR_RANGE prove image
invalidity. ARGUMENT, CRYPTO, READ and ABORTED are operational stops. Identity
mismatch, stale authority, sequence exhaustion and store errors are **not**
invalid-image evidence and cannot authorize trying another role. A confirmed but
roleless entry is not PREVIOUS. With two slots, current+candidate has no eligible
previous. Current+previous has no candidate. PREVIOUS cannot run until promotion
is durable. Both resident entries' identities, trial IDs, counts, reasons and
rollback history are retained by promotion. Only required roles/confirmation and
sequence change. No verification failure opportunistically adds a count/reject.

Maximum sequence permits normal CURRENT and TRIAL zero-write selection, including
invalid-candidate fallback. It prevents READY first-TRIAL and PREVIOUS promotion.
Freshness compares canonical encoded metadata **and** the codec-selected page,
not padding or sequence alone. This detects sampled stale authority, not races.
violating stable ownership remains unsupported.

### Status and reconciliation

`boot_select_result_t` carries status, per-slot verifier evidence and exact store
and lifecycle component results. `evidence_mask` marks meaningful verifier entries.
it is not a complete attempt trace (candidate store failure may precede or follow
verification). Unset/default OK entries are not authentication evidence.

- `VERIFY_FAILED` is operational verifier failure. `IDENTITY_FAILED` is confirmed
  role resident mismatch. Candidate identity failure is `LIFECYCLE_FAILED` with
  `BOOT_LIFECYCLE_ERROR_IDENTITY`, preserving the existing transaction boundary.
- `LIFECYCLE_FAILED` includes sequence exhaustion. `STORE_FAILED` includes invalid
  caller snapshots, stale authority, IO and precommit VERIFY errors. None falls
  back. `RECOVERY` also reports exact NO_RECORD/CONFLICT when store supplied it.
- `RECONCILE` means exactly `BOOT_STORE_UNCERTAIN`. Stop. The API does not reload,
  retry, publish or assume rollback. Explicitly load and reconcile old/new/recovery
  authority, then make a fresh call if appropriate. Never replay cached READY or
  a previous-promotion proposal. Reset-accounting success is still a prerequisite.


## One-shot runtime

`boot_runtime_run` composes the existing `boot_reset_handler_run` and
`boot_select`, using the real verifier, crypto, lifecycle and commit-last store.
It does not execute an image, bind MMIO, establish/service a watchdog, impose a
candidate deadline, or implement production startup/handoff/FTP integration.
Use the optional [runtime watchdog bridge](boot-startup.md) to adopt an
already configured watchdog and service actual verifier milestones across this
unchanged raw API. Its `boot_runtime_watchdog_launch` entry also composes the
successful selection with final-handoff intent on the same watchdog object.
Use that entry instead of chaining a decision run and a handoff call. All
unmeasured service gaps and physical limits remain.

### Caller lifecycle

1. Allocate fresh `boot_runtime_t` storage for a new physical/model boot and call
   `boot_runtime_init` exactly once before publication.
2. Establish watchdog protection and exclusive ownership of reset registers,
   metadata, images, adapters, contexts and output. Supply manifest prefixes for
   both physical slots and truthful injected reset-register/flash/read adapters.
3. Call `boot_runtime_run` once. Only `BOOT_RUNTIME_OK` publishes an owned image
   and authoritative snapshot. Preserve ownership/stability until use.

Structural argument rejection performs no IO and allows corrected arguments on
that object. An atomic entry guard rejects callback reentry as BUSY. Once
structural admission succeeds, the object is consumed **before the first
callback**. Every admitted failure and success is terminal. Duplicates return
CONSUMED with no callbacks, capture, clearing, accounting or selection. There is
no same-object selection-only retry.

Do not reinitialize, copy or recreate the object during the same boot, or bypass
it with raw reset/selection APIs. This is a caller contract, not a global mutable
latch or persistent exactly-once mechanism. A new boot needs a fresh object and
fresh capture. Never replay cached flags. The guard does not establish physical
exclusivity or protect against arbitrary external calls/input mutation.

### Ordering and effects

The actual handler captures the **original raw flags**, reads fresh reset flags
and writes `fresh & ~(captured & 0x1f)` (R/W, **not W1C**), then loads authoritative
metadata, classifies the original capture and makes at most one accounting
commit. Unsupported or newly set uncaptured bits are retained. Zero supported
capture skips fresh register read/write. Fresh flags never change classification.

Uncounted resets preserve valid pending intent with **zero accounting writes**.
Applicable counted TRIAL resets consume matching intent, count and reason in one
commit. Count three rejects and clears the candidate before selection. Only
handler OK's final snapshot enters the selector. The selector can independently
commit READY→TRIAL or eligible previous promotion, so zero accounting writes does
**not** necessarily mean zero total writes. Candidate image-invalid fallback and
operational failure distinctions remain [selector policy](boot-architecture.md).

Adapters and both manifest prefixes are snapshotted before any callback. Valid
inputs may alias each other, but not the runtime object. Output must be disjoint
from inputs, runtime and opaque context storage (opaque sizes are caller
obligations). Callbacks must not mutate them. Output stays unchanged throughout
all callbacks and on any failure. Full success publishes an owned copy.

### Failure/report authority

The returned composite `valid` bits identify which stages were attempted.
`RESET` makes `reset_status` and the partial reset report available. The nested
reset `valid` bits control each diagnostic field. A reset snapshot is final
**only on handler OK**, never on error. `SELECT` is set only after handler OK and
makes the selector result available. Selector `evidence_mask` describes known
verifier evidence, **not a complete attempt trace**. Neutral/default enum OK
values in invalid report fields do not establish success or authentication.

Reset STORE_FAILED/UNCERTAIN and selection RECONCILE map to composite RECONCILE,
with no decision, automatic reload, retry or reset replay. Explicit **read-only**
authority reload/reconciliation outside the composition can discover old or new
committed authority after an ambiguous callback. It does not turn a failed reset
into successful this-boot provenance or authorize rerunning reset or selection
in that boot. The old object stays consumed even after reload.

Clear-before-commit interruption can lose an event. Same-bit relatch can be lost.
a stale matching deadline intent can classify a later SW reset. No event journal,
persistent replay guarantee, physical register behavior/timing, final integrated
link/dependency closure or 32 KiB fit claim is added.


## Candidate transaction

`boot_launch_candidate` in `include/lm3s6965/boot_launch.h` combines the real
stored-image verifier, lifecycle proposal and commit-last metadata store. It
publishes a decision. It does not execute an image or perform a handoff.
[Boot selection](boot-architecture.md) now composes this unchanged candidate transaction
with confirmed CURRENT, durable PREVIOUS promotion and explicit recovery.

### Caller boundary

1. Complete `boot_reset_handler_run` with `BOOT_RESET_HANDLER_OK` in this boot.
   Use its final authoritative snapshot. A snapshot flag in a failed report is
   not permission to proceed. This API does not accept a reset report or prove
   that reset accounting occurred. C structs do not establish provenance.
2. Establish watchdog protection before verification. Own both metadata pages,
   the physical candidate manifest and payload, adapters and callback contexts
   exclusively. Maintain ownership throughout verification, commit and decision
   use. No ISR, concurrent writer, callback reentry or input/output mutation is
   permitted. Only the requested store operations may change metadata.
3. Supply the candidate's physical manifest bytes and a truthful physical
   payload reader. Supply an optional genuine-progress callback. The wrapper
   passes both callbacks and their contexts unchanged to `boot_verify_stored`.
   It does not feed a watchdog, schedule work or measure crypto/flash gaps.
4. Use the decision only on `BOOT_LAUNCH_OK` and while image and authority remain
   stable. If ownership is released, a saved decision is not current authority.
   Physical handoff, watchdog transfer and application startup are separate.

The API is synchronous and non-reentrant by caller contract. There is no global
lock, object initialization or reentry detector. All spans must cover complete
stable objects. Output overlap with input objects and the wire span is rejected
before callbacks. Output overlap with opaque context storage is prohibited but
cannot be checked because the API has no context sizes. Inputs can alias if
both remain valid. Callbacks must not modify the output. Errors leave it unchanged.

### Transaction

| Step | READY | TRIAL retry |
| --- | --- | --- |
| Initial authority | Load both pages and compare exact encoded record and selected page with caller snapshot | Same |
| Image | Independently authenticate manifest, stream/hash exact payload and check vectors for the physical candidate slot | Same. No cached verification |
| Identity | `boot_lifecycle_begin_trial` checks exact resident slot/hash/release/length | Same |
| Proposal | Exactly next sequence, READY to TRIAL. Preserve trial ID and history | Exact NO_CHANGE, including maximum sequence |
| Durability | `boot_store_replace` rechecks authority, erases alternate page only, programs commit last and checks committed readback/selection | Load both pages again and reject any authority change. Zero erase/program |
| Publication | Exact verified image plus authoritative TRIAL snapshot after `BOOT_STORE_OK` only | Exact verified image plus freshly checked authoritative snapshot |

No launch count, new trial ID, per-handoff write, confirmed selection, fallback
or recovery promotion is added. READY at exhausted sequence cannot launch. TRIAL
retry at maximum sequence can launch without a write. No candidate means
`BOOT_LAUNCH_INELIGIBLE`, not a request to choose a different image.

### Failure and reconciliation

`boot_launch_result_t` returns a status and the exact failing component result.
Consult `verify_result` only for IMAGE_INVALID/VERIFY_FAILED, `lifecycle_result`
only for LIFECYCLE_FAILED and `store_result` only for STORE_FAILED/RECONCILE.
Unused fields have neutral defaults. They do not report completed steps.

- IMAGE_INVALID contains FORMAT, TARGET, SIGNATURE, HASH, MSP, THUMB or
  VECTOR_RANGE. It does not select a fallback or modify metadata.
- VERIFY_FAILED contains operational ARGUMENT, READ, ABORTED or CRYPTO. These
  failures do not prove that the image is invalid.
- LIFECYCLE_FAILED includes resident identity mismatch and sequence exhaustion.
  STORE_FAILED includes stale authority, read errors, invalid/conflicting or
  absent metadata and precommit IO/VERIFY. None publishes a launch decision.
- RECONCILE contains exactly `BOOT_STORE_UNCERTAIN`. The commit may be absent,
  partial or complete. The wrapper does not reload, retry or publish after this
  result. Stop. Explicitly load and reconcile old/new/recovery authority before
  another operation. Do not replay the cached READY transaction. A reconciled
  TRIAL can be independently reverified without writes. A reconciled READY
  requires a fresh transaction. Recovery has no launch permission here.

This is not the whole reset composition. A failed reset handler remains a stop
boundary. Do not use this wrapper to bypass it. The explicit reconciliation
route applies to a transaction after the successful reset pass, not to claiming
that a failed reset pass succeeded. Store readback cannot protect against a
false adapter, later corruption or violated exclusive ownership.


## Handoff admission and ordering

`include/lm3s6965/boot_handoff.h` and `src/boot_handoff.c` implement a host-model
admission and ordered trace gate. Success records **terminal entry intent**, not
physical execution. This module is opt-in and is not part of BSP startup.
The [protected launch entry](boot-startup.md#complete-launch-intent)
composes real reset accounting and selection with this same handoff transaction.
It holds both object guards from admission through owned output publication.
A private held-operation seam preserves the public transfer semantics without
copying or releasing the watchdog. The standalone API below remains unchanged.

### Separate physical startup boundary (opt-in)

[boot-startup.md](boot-startup.md) provides the reusable post-return continuation:
returning logical terminal callback → owned launch output/guards released →
one-shot prepare → physical stackless dispatch → authenticated slot prefix →
real C initialization. Never branch from the logical callback or replay handoff.
Independently linked A/B entry must preserve the original in-place controller,
publication owner and boot-resident callback context across real data/BSS init.
The isolated terminal fixture checks the core tail, not production peripheral
shutdown or foreground service.

### Admission and authority

1. Obtain a successful `boot_verify_stored` result for the selected eligible
   image. Keep the authenticated storage unchanged through the transaction.
2. Supply that image by value in `boot_handoff_request_t`, the selected physical
   slot and the current boot-owned watchdog epoch. Inject a truthful current
   context observation, under the caller's whole-transaction exclusive ownership.
3. Initialize a fresh `boot_handoff_t` once. Call `boot_handoff_run` with the same
   already configured watchdog object that the application will preserve.

`boot_handoff_admit` is a pure structural check. It does not authenticate a C
struct, establish eligibility, sample hardware or acquire storage authority.
There is no forgeable boolean presented as proof of verification. Provenance,
selection authority, observation freshness and exclusivity are caller contracts.
The run function snapshots the entire request before invoking callbacks.

The selected slot must be A=0/B=1 and match manifest slot and base exactly:
`0x8800`/`0x24000`, aligned to 256 bytes. Length must be 236..111616 inclusive
and a multiple of four. MSP must be exactly `0x20010000`. The original reset
vector must be Thumb. Its cleared target must lie in the authenticated half-open
`[base, base + length)` interval. Targets inside the vector table remain allowed:
**there is no new target >= vector-table-end rule**. This matches the verifier's
existing boundary, not an ELF entry-point rule.

Context admission requires IPSR=0, privileged Thread mode and CONTROL=0 or 2
(MSP or PSP allowed). PRIMASK and FAULTMASK may each be 0 or 1. This part has
three priority bits: BASEPRI may be `0,32,64,...,224`. Other bits reject. These
are representability checks, not an inherited reset-state requirement. Final
intent contains selected VTOR/MSP/original Thumb vector and CONTROL, PRIMASK,
BASEPRI, FAULTMASK **all zero**, plus the advanced application watchdog epoch.

Run rejects a nonconfigured, blocked, nonboot-owned, stale-epoch or busy watchdog
before any callback. Epoch zero or UINT32_MAX rejects. UINT32_MAX-1 permits the
last one-way transfer into UINT32_MAX. Exhausted later service remains subject
to the watchdog API. No watchdog IO, configuration, clock change or feed is
performed by handoff. No metadata write or new eligibility decision occurs.

### Ordered trace and terminal boundary

The operation callback receives these logical events in exactly this order:

1. Establish host exclusion. Stop SysTick. Disable external interrupt delivery
   for bank 0 (IRQ 0..31) and bank 1 (IRQ 32..42).
2. Quiesce peripheral interrupt sources **and work**, preserving the enabled
   watchdog, its clock and object. Stopping delivery alone cannot stop repending.
3. Final-clear pending SysTick/PendSV, then both logical NVIC banks. All final
   pending clears occur **after** source quiescence.
4. Emit a barrier-before-VTOR event, set selected VTOR, emit barrier-after-VTOR.
5. Call existing `boot_watchdog_transfer` once without callbacks or register IO.
   Invoke `terminal_intent` with the exact tuple and advanced epoch.

The operation value is selected base only for SET_VTOR and zero otherwise. Bank
numbers and zero values are **not hardware write masks**. Barrier events express
ordering intent, not exact DSB/ISB instruction, access-width or register semantics.
No callback is authorized to switch MSP, change CONTROL/masks physically, branch
to the application or release exclusion. Reset-like entry masks are recorded in
intent only. The opt-in startup boundary supplies assembly. Enabling a physical caller still needs review
with no old C frame after MSP switch and no return, plus target register/barrier,
source coverage and clock/watchdog continuity evidence.

### Failure and ownership contract

Callbacks return zero only after completing their operation. A nonzero result
may occur before or **after** effects. Invalid admission produces no callbacks
or output change. An admitted callback error immediately quarantines handoff:
no later callback, cleanup, implicit exclusion release, retry or rollback. The
software reentry guard is released. **host exclusion is not**. These are distinct
mechanisms. A failed EXCLUDE may itself already have acquired exclusion.

The caller owns the whole transaction, including all interrupt sources, selected
storage, context, watchdog/controller and both objects. Atomic guards only reject
nested calls. They are not cross-module hardware locks. Callbacks must not mutate
objects/IO/output or call watchdog APIs. The preflight briefly checks the watchdog
guard then releases it so the existing transfer API can acquire it. Concurrent
watchdog use across this interval is forbidden by the caller contract.

The `boot_handoff_run` output storage must be disjoint from the request, callback
context storage and both handoff/watchdog objects. Request snapshotting does not
relax this requirement: output remains unchanged throughout every callback and
is published only after full success. This is a caller storage contract, not a
runtime alias-detection guarantee.

Before terminal intent, a failure leaves boot ownership intact. Caller must not
resume boot work or feed via another API. Stop and retain watchdog protection.
At terminal intent the watchdog has already transferred to application ownership,
advanced epoch and consumed evidence. **Terminal failure does not roll this back.**
The callback may have recorded the intent before reporting failure. Neither
absence of entry intent nor physical application execution can be inferred from
its error. The safe host response is quarantine, not continuation or another
entry attempt. Output is published only on complete success. Successful handoff
also cannot be repeated. It leaves host exclusion logically held.

Never reinitialize either object to bypass terminal state or quarantine. No blind
feed, ICR/LOAD/CTL access, clock removal or disabling protection is permitted.
Application continuation and actual service gaps are later composition gates.


## Target terminal contract

`tests/boot/handoff_terminal.S` is deliberately **fixture-only**. No production
archive, BSP startup or host contract is changed. Its register ABI is r0=selected
VTOR, r1=validated initial MSP, r2=original authenticated Thumb reset vector.
It does not validate inputs, transfer ownership or shut down sources. Preconditions:

1. Privileged Thread mode, no active exception, exclusive transaction ownership.
   inherited CONTROL=0/2 and representable masks as described above.
2. PRIMASK already 1. SysTick stopped, external delivery disabled, peripheral
   work and interrupt producers stopped, final pending clears completed.
3. Selected image/table stable and authenticated, exact MSP `0x20010000`,
   application startup compatible with continued watchdog protection.
4. Same configured watchdog object already transferred once. Its clock,
   controller and object lifetime remain intact through application startup.

All input values are in registers before switching MSP/CONTROL. The assembly
completes a DSB, writes VTOR, executes DSB/ISB, sets LR to a Thumb return trap,
sets MSP with immediate ISB, sets CONTROL=0 with immediate ISB, clears BASEPRI
and FAULTMASK, and only then clears PRIMASK and branches through original r2.
There is no C frame, stack operation, memory access after the MSP switch, call,
restore or return in this primitive. LR traps a reset routine that returns.
NMI/fault hazards are not removed by PRIMASK. Callers must establish a safe
system, not infer safety from exclusion alone. This is not a reset of all core
state (for example PSP, priorities and fault status are not reset).

The local MCU PDF was re-extracted with PDFKit because dedicated document tools
were unavailable. `datasheets/LM3S6965 Microcontroller Data Sheet.pdf`, July 15,
2014: masks/CONTROL pp. 68–71. Stack-switch ISB p. 71. DSB/ISB and vector ordering
pp. 75–76. SysTick stop/count/current semantics pp. 106–109. NVIC clear-enable
pp. 112–113 and clear-pending pp. 116–117. Pending SysTick/PendSV clear commands
pp. 124–125. VTOR fields/alignment p. 127. Clear-enable and clear-pending are
write-one commands, not read-modify-write. Bank masks are `0xffffffff` and
`0x00000fff`: the register description includes unused IRQ 43 (bit 11), while
the established 43-IRQ host model ends at IRQ 42. No host geometry changes.
ICSR uses only clear command bits 27/25 (`0x0a000000`), never a mixed set/clear
RMW. Barriers are actual instructions, not conclusions from host mock events.

`tests/boot/test_handoff_target.py` checks the isolated tail at `-Os` and
`-Og`, including both VTOR bases, inherited MSP/PSP and representable masks.
Cases share fixture state and are not independent fresh-reset trials. The
fixture uses synthetic watchdog IO. It does not prove production data/BSS
preservation, physical watchdog countdown or peripheral shutdown.

Its disassembly gate checks exact terminal/capture instructions, barriers,
register operands, vector literal and return trap. Limited old-stack witness
words cannot prove that all old-stack addresses were untouched. Retain raw QEMU
stdout/stderr and the command/exit/timeout log before interpreting a result.
Source fixtures and final linked closure remain separate evidence.


## Diagnostic wire and publication

### Wire contract

Exactly 64 bytes, sixteen explicitly little-endian uint32 words, intended address
range `[0x2000DFC0,0x2000E000)`. Existing `boot_format.h` supplies SRAM geometry
and A/B/unavailable slot constants. It is unchanged. No C struct is copied to
or from the wire. The fixed 64-byte prefix is inspected/written. Larger caller
spans are permitted, and their tails are ignored/unchanged.

| Offset | Word |
| ---: | --- |
| 0 | Magic `ABD1`, bytes `41 42 44 31`, LE `0x31444241` |
| 4 | Schema 1 |
| 8 | Length 64 bytes |
| 12 | Validity: 0 staging/unpublished, 1 published |
| 16 | Stage: reset 0, metadata 1, validation 2, handoff/startup 3, trial 4, recovery 5 |
| 20 | Latched fault: none 0, metadata 1, validation 2, startup 3, trial 4, terminal recovery 5 |
| 24 | Global detail 0..20 (below) |
| 28 | Raw captured reset flags, all 32 bits preserved |
| 32 | Running slot: A 0, B 1, unavailable `0xFFFFFFFF` |
| 36 | Selected slot: same encoding |
| 40 | Metadata sequence: full uint32 domain |
| 44 | Applicable trial ID: full uint32 domain |
| 48 | Counted failures: 0..3 |
| 52 | Health: app init bit 0, Ethernet init bit 1, FTP listening bit 2. Mask 7 |
| 56 | Failed slot: same slot encoding |
| 60 | Reserved: zero |

Global detail is: none 0. No valid metadata 1. Conflicting metadata 2. CRC 3.
sequence exhaustion 4. Manifest format/device 5. Target/link 6. Signature 7.
hash 8. Payload bounds 9. Vectors/MSP 10. Startup unknown 11. App init 12.
Ethernet init 13. FTP init 14. WDT 15. EXT 16. BOR 17. Deadline 18. Combined 19.
no recovery 20. It is not fault-scoped numbering. The codec deliberately imposes
no extra stage/fault/detail pair mapping or slot relationship restrictions.
Fault 0 does not prove health and has no LED error pattern. Fault precedence,
retaining underlying terminal detail and successful-fallback latching remain
future caller obligations, not transformations performed by this codec.

### APIs and evidence availability

- `boot_diag_validate` checks the in-memory enum/slot/count/health spaces.
- `boot_diag_encode(record, validity, wire, capacity)` validates before any
  writes and emits canonical header/reserved fields. Both staging and published
  fixtures can be encoded. This is **not** a live publication operation.
- `boot_diag_decode(wire, size, required_validity, out)` validates the header,
  exact requested validity, reserved word and fields before publishing output.
  Use `BOOT_DIAG_PUBLISHED` for readers. `BOOT_DIAG_STAGING` is an explicit
  offline inspection mode and rejects published records. Unknown validity
  values are never accepted. Unsupported schema is rejected before interpreting
  version-dependent fields.
- `boot_diag_init(captured_flags, out, context)` initializes an offline boot
  record: stage reset, no detected fault/detail, no app health bits, all slots
  unavailable, raw flags unchanged. It guesses no metadata roles or history.
  Sequence/trial/count zeros are placeholders, with context explicitly marking
  all three unavailable. Reset/stage/health are known because this caller has
  supplied a captured snapshot and is initializing a fresh boot fixture.
- `boot_diag_slot`, `boot_diag_sequence`, `boot_diag_trial_id`, and
  `boot_diag_failures` return typed `{available, value}` results (slot uses
  `{available, slot}`). An unavailable result's zero value is not evidence.
  Null numeric-helper inputs yield unavailable. Invalid slot/count inputs also
  yield unavailable. Callers should validate records first.

All writable outputs are unchanged on errors. Inputs/outputs must not overlap,
as with `boot_format.h`. Aliasing is a caller precondition, not runtime detection.
All spans must actually cover their declared size. Unaligned wire pointers are
supported. No native word accesses occur.

**There is no approved numeric unknown sentinel for sequence/trial/count.**
`UINT32_MAX` is a legitimate sequence and trial ID, and known zero is distinct
from absent evidence. The caller supplies `boot_diag_context_t`, which is NOT a
wire field, flag or reserved-bit extension. It tracks availability of
sequence/trial/count and of stage/reset/health. No availability is inferred from
wire zero/max, fault code, stage, selected/running/failed slot or metadata roles.
Availability cannot be recovered by decoding the 64 bytes alone. Future status
or halted-SWD glue needs trustworthy, same-snapshot external evidence/context.
without it, report unavailable rather than fabricate a value. This increment
neither defines that glue nor changes the format to solve it.

### Publication and integration limits

#### Ordered foreground publication API

One zero-initialized `boot_diag_publication_t` owns an aligned 16-word SRAM
port, external availability context and nonreentry guard. On target, set `words`
to `(volatile uint32_t *)BOOT_DIAGNOSTIC_BASE` and leave `word`/`barrier`/`user`
NULL. This is caller-owned ordinary SRAM, **not fallible MMIO**. The whole owner
and port must remain exclusively owned. All declared spans must actually exist.
owner/input/output overlap with the port is rejected before access. No C wire
struct or untrusted byte-buffer alias is used for target stores.

| API | Contract |
| --- | --- |
| `boot_diag_publication_boot(owner, raw_flags)` | Explicitly overwrites any old valid or garbage block via reviewed `boot_diag_init`. Preserves every raw capture bit. Numeric availability initially false. Stage/reset/health known. |
| `boot_diag_publication_adopt(owner, context, trusted)` | Validates published 64 bytes, writes **no SRAM words**, associates exact caller context. Requires trustworthy same-record associated metadata. No initialize/reset/reconstruction or inference from zero/max/header. `trusted=false` rejects before reads. The bool is a provenance assertion, not authentication. |
| `boot_diag_publication_progress(owner, stage, health)` | Reads/validates existing published record under the same owner, changes **only stage and health**. Preserves fault, detail, raw reset, failed/running/selected slot, sequence, trial ID, count and **all availability**, even unknown stage/health with a correct header. Metadata/slot refresh is not implemented. |
| `boot_diag_publication_snapshot(owner, record, context)` | Serialized foreground-only decoded copy plus associated sidecar, outputs unchanged on failure. Not a final status snapshot with metadata/identity/accounting. |
| `boot_diag_publication_capture(owner, callback, context)` | Calls the read-only extension once with temporary owned diagnostic/context values while the same owner is busy and interrupts remain masked. Callback false returns CAPTURE. Pointers must not escape. |

Complete candidate is byte-encoded and validated locally, converted explicitly
from LE to a local aligned word array, then written: validity **0 first** →
DMB/compiler memory clobber → other **15 aligned uint32 stores** → sidecar update
in the same owner scope → DMB/compiler clobber → validity **1 last**. The native
store is volatile uint32, not byte stores into reserved SRAM. On Cortex-M3 all
operations reject nonzero **IPSR** before owner/port mutations, save PRIMASK then
`cpsid i`, and restore the exact caller PRIMASK (0 or 1) at every exit. Privileged
Thread mode is required. No BASEPRI/FreeRTOS primitive is imported. **PRIMASK does
not mask NMI or HardFault**: they must never write diagnostics or this owner.
No ISR writers, no alternate owners, no foreground task interleaving across
calls. `busy` rejects callback reentry, not arbitrary multithread races.

Host-only bounded `word(user,index,write,value)` callbacks model exactly one
word (index 0..15). Barrier hook traces ordering. Target rejects non-NULL hooks.
Callbacks must be truthful, bounded, serialized and cannot mutate other words.
False on read returns READ before publication. False on any store returns
**UNCERTAIN**, including false after an effective final validity-1 store. The
owner then disassociates availability and snapshot refuses to claim publication.
After successful validity-0, every interrupted pre-final native store leaves
validity 0. First-store failure can leave old/garbage RAM, and final-store failure
can leave a fully published new block. Do not assume the old state, retry
implicitly, or reuse a cached sidecar. Reconcile and explicitly adopt only with
trustworthy same-record context. An authorized boot initialization is a different
operation. Infallible native SRAM has no callback failure result. Power/reset
interruption is not a returned C error or a retained-owner guarantee.

#### Coherence boundary

Validity double-check **1→1 is not whole-block atomicity** (ABA). Coherence here
comes only from serialized foreground readers and writers using this owner.
External SWD requires a separately authorized halt, then header/field/published
validation. It must not infer availability from raw SRAM, even all-zero numeric
fields or maximum sequence/trial. Do not replay old boot context on app adoption.
No live debugger/FTP atomic-read promise or full FTP status is provided.

#### Read-only coherent snapshot foundation

`boot_snapshot_capture(owner, sources, out)` copies decoded diagnostics and their
external availability context, authoritative metadata (including accounting), and
actually-running authenticated stored-image evidence under **the same publication
owner session**, before unlock. Success publishes a fixed-size, caller-owned
`boot_snapshot_t`. No borrowed pointers escape, later source changes do not change
the copy, and all errors leave output unchanged. Capture writes no diagnostic
words and never invokes metadata erase/program callbacks.

The metadata `read` follows `boot_store`'s truthful read/error contract. The
`running` callback returns zero only for the actually executing, previously
authenticated stored image, including vectors, **not** a selected slot or role
inferred from metadata. Capture checks running identity against its resident
metadata entry, image bounds, exact initial MSP and Thumb reset-vector bounds.
it does not perform cryptographic verification. Verification-to-use stability
and provenance remain caller obligations, not authentication of a C struct.

Both callbacks run with the owner busy and target interrupts masked. They must
be bounded, read-only, nonblocking and allocation-free: no crypto, formatting,
erase/program, owner/port/context/source mutation or reentry except attempted
publisher calls, which return BUSY. Callbacks may write only their requested
output spans (host trace counters can be separate local state). Descriptor,
output, owner and port must be disjoint. Callback-managed storage must not alias
output/port and may reference but not mutate the owner. All participants must
serialize through this owner and prevent source/image changes throughout capture
and between verification and use. PRIMASK cannot protect against DMA, debugger,
another owner, or dishonest/stale callbacks.

Diagnostics and availability are copied **verbatim**, including raw reset flags
and latched historical fault, failed slot, sequence, trial and count. Coherent
sampling does not mean historical diagnostic numbers/slots equal live metadata,
roles or running identity: a failed image can legitimately precede a running
fallback. The prior global diagnostic-source cross-match requirement was an
orchestrator overrestriction and was removed. No new source-binding layer,
wire schema or event history was implemented. External availability still must
be trustworthy for the diagnostic record itself, not fabricated from live roles.
The [application service](boot-reset-image.md#application-status-and-confirmation)
uses this coherent snapshot for status rendering. The codec alone neither
registers FTP commands nor establishes production transport ownership.

Running status must capture metadata/identity/accounting/diagnostics together,
not combine fields sampled across transitions. Halted
SWD readers must check header and published validity under separately authorized
operational procedures. Boot/app linker and startup exclusions, retained-SRAM
behavior, stack/heap/resource fit and timing require separate validation. No
linker/startup path is changed here. Existing reviewed layouts remain untouched.
No image hash is duplicated, no reset is classified here, and power loss cannot
restore transient history from this codec.
