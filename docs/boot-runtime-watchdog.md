# Protected runtime selection and launch intent (HOST only)

`boot_runtime_watchdog_run` wraps the unchanged one-shot [runtime](boot-runtime.md)
and real selector/launch/verifier callbacks. It returns an owned decision, **not
execution or handoff permission**. Use `boot_runtime_watchdog_launch` instead
for the complete selection-to-final-handoff intent transaction below. Neither
entry configures a controller, binds MMIO, installs an ISR or proves timing.

## Establish protection, then adopt it

1. Initialize fresh `boot_watchdog_t` and `boot_runtime_watchdog_t` objects once
   for this physical/model boot. The bridge owns its embedded raw runtime.
2. Establish actual stable clock, RCGC0 readiness and explicit finite reload;
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
Opaque contexts must also be disjoint from mutable objects/output; their sizes
are caller obligations. Inputs may alias other inputs. Register/store tables and
both manifest prefixes are copied before operations; watchdog IO was copied by
watchdog init. Callbacks must not mutate these objects, inputs or output.

The bridge holds the **real watchdog's entry guard for the entire transaction**,
including reset/flash callbacks and final publication, not just individual
reports/services. External watchdog report/service/configure/transfer calls from
callbacks return BUSY. A private source-only held-operation seam reuses the same
watchdog report/service logic without releasing that guard. It is not a consumer
API. Caller exclusivity is still necessary: the guard cannot stop direct MMIO,
field mutation, copying or reinitialization.

## Complete launch intent

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

## Post-return startup continuity (opt-in)

Only after this function has actually returned OK may its trusted boot caller
pass the owned output/report to `boot_startup_prepare`, under continued physical
PRIMASK exclusion and stable authenticated image storage. Guards being released
is a call-site postcondition; no atomic_flag layout inspection is introduced.
A matching APPLICATION owner alone is insufficient (terminal may have failed).
[Startup continuity](boot-startup.md) consumes EMPTY → PREPARED → DISPATCHING →
ENTERED → C_READY once, with any admitted error terminal FAILED. Dispatch never
repeats runtime, quiescence, accounting, transfer or metadata writes.

The retained `entry_epoch` is immutable history, compared to the original
controller only through early entry. Later service uses the live watchdog epoch;
first successful foreground feed does not invalidate running identity. The new
code does not configure/feed during startup, choose a physical clock source,
enable the candidate deadline actuator or implement accepted-reset grace.

## Genuine milestones only

| Actual verifier event | Bridge action |
| --- | --- |
| BEGIN, after structural/target checks | Allocate `watchdog.verification + 1`, report actual checked total and zero completed; **never service** |
| AUTHENTICATED, after signature success | Report with current epoch and same ID/total, then service exactly once |
| PAYLOAD_PROGRESS, after successful exact read+hash chunk | Report actual cumulative completed bytes and unchanged total, then service exactly once |

Each successful service consumes evidence and increments the epoch. The next
callback reads that new epoch; IDs increase across candidate/current/previous
attempts and are unrelated to slot, metadata sequence or trial ID. BEGIN can
abandon a failed stream; a new ID cannot wrap. ID exhaustion returns watchdog
STATE before attempting BEGIN report. Epoch exhaustion follows existing watchdog
service semantics: STATE/quarantine with no register service or wrap. Exhaustion
inside verification aborts that operation. The final legal ID can finish one
stream; it cannot admit another stream. No pending evidence is banked: adoption
discards preexisting evidence, every service consumes it, and terminal failures
clear it.

No cancellation API is added. A watchdog report/service failure returns zero
through the existing progress callback chain: verifier ABORTED is operational,
so selector stops, never treats it as image-invalid fallback. Valid signature
mismatch/hash/vector invalidity retains the existing independent fallback policy.
A last successful chunk is **progress, not validation**: final hash/vector checks
or later reader/crypto/store errors can still fail after a good milestone.

## One-shot failure and diagnostic authority

Admission consumes the wrapper before any callback, and raw runtime consumes
itself before reset capture. Callback reentry is BUSY; every admitted result is
terminal. Duplicates return CONSUMED with no IO/feed/reset replay. Structural or
watchdog preflight rejection alone permits corrected arguments.

Output stays frozen during every callback and all failures; success publishes an
owned local decision only after runtime OK. The runtime result is preserved
unchanged: its stage `valid` bits, reset nested validity, previous statuses and
selector `evidence_mask` retain their existing meanings (not an attempt trace).

Wrapper `valid` bits are separate:

- `DIAGNOSTIC`: exact last attempted watchdog operation/result, including
  preflight rejection. Default watchdog OK without this bit proves nothing.
- `RUNTIME`: raw runtime was attempted; inspect its own status and validity.
- `CALLBACK`: phase/epoch/verification/completed/total describe the last actual
  callback. Epoch is the epoch **used for the attempt**, before successful service
  increment. No callback is inferred from default phase/zero values.

A later runtime failure may retain last watchdog OK: this does not mean image
validity or watchdog failure. Report/service errors retain their exact cause and
operation, not merely the resulting verifier ABORTED. All admitted failures
quarantine the watchdog and discard evidence without additional register IO.
Service faults retain existing before-unlock stop or best-effort relock cleanup;
cleanup never retries a feed or converts failure to success. No automatic
reconciliation, retry, rollback or replay occurs. UNCERTAIN remains raw runtime
RECONCILE; explicit external read-only authority reconciliation cannot authorize
rerunning this boot or servicing the quarantined object.

## Unmeasured intervals and later gates

Reset capture/accounting/flash before BEGIN, nonrestartable ECDSA, interattempt
work, final hash/vector checking, metadata commits and final publication receive
**no synthetic progress reports**. Their service gaps remain unmeasured. This
bridge cannot interrupt a stalled callback or signature calculation. Caller must
validate all these intervals against real hardware budgets before deployment.
Nominal 5 seconds/stage means intended **10 seconds TOTAL**, not a measured bound.
No physical register/clock/reset behavior, ISR/MMIO binding, startup/handoff,
application/FTP continuity, final dependency closure/link or **32 KiB fit** is
proved by HOST tests or archive compilation.

## Build and test handoff

Standard `tests/boot` build-all/discovery registers `boot_runtime_watchdog` and
`boot_runtime_watchdog_fault` through `tests/boot_launch`. They use real signed
A/B min/max fixtures, verifier/crypto/runtime/reset/store/selector and injected
watchdog registers. The fault variant uses existing crypto allocator interception,
not a production fault API. Strict warnings and ASan/UBSan apply to both.

```sh
cmake -S tests/boot -B /tmp/boot-runtime-watchdog/debug \
  -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build /tmp/boot-runtime-watchdog/debug --parallel 8
ctest --test-dir /tmp/boot-runtime-watchdog/debug -N
# Execution belongs to test-merit-gate, not the implementing worker:
ctest --test-dir /tmp/boot-runtime-watchdog/debug \
  -R '^boot_runtime_watchdog(_fault)?$' --output-on-failure
# Repeat Release, then full unfiltered dependency gates.
```

Coverage includes both slots/pages; exact AUTH/chunk service epochs/IO/IDs;
nonzero lifetime stream start; candidate-invalid/current fallback and
current-invalid/previous promotion; signature/hash/vector/read/crypto/ABORTED
distinctions; ID/epoch exhaustion; every service IO cut before/after effects,
readback mismatch and failed cleanup; reset failures and commit-relative store
UNCERTAIN with no additional feeds/replay; callback reentry/terminal duplicates;
output validity/frozen publication; input snapshot probes; zero-epoch/missing
watchdog callbacks and structural null/missing transport callback rejection with
no IO/consumption and corrected successful retry; aligned union-backed
bridge/watchdog/output versus input-table overlap rejection; public watchdog
report/service BLOCKED with no IO after admitted failures; and READY-candidate
AUTH service cuts before/after effects with no CURRENT fallback, store commit or
additional feed (apart from an ICR effect already applied by the failing call).
Execution is deferred to test-merit-gate: compilation alone claims no test pass.

Launch regressions use the real final-handoff component with these same fixtures.
They cover both slots and metadata pages, min/max images, READY/TRIAL/current,
counted reset accounting, candidate fallback and previous promotion. They check
current epoch and lifetime IDs, exact ordered callbacks, 96 before/after-effect
handoff cuts, terminal ambiguity, no extra writes or feeds, frozen owned output,
callback reentry, cross-entry duplicates, preflight correction and known-span
aliases. They also cover runtime stops and commit-relative uncertainty without
handoff or accounting replay. These are modeled cases, not physical trials.

Standalone ARM archive target: `boot_runtime_watchdog`. Root opt-in archive:
`lm3s6965_boot_runtime_watchdog`. Both depend on runtime and handoff archives.
Consumers still supply verifier/crypto and final-link policy. Neither is a boot
image or an integration/fit/timing acceptance claim.

## Next application FTP and PC boundary

MVP 4 starts with a new three-operation service and a bounded streaming state
machine. Start with strict text, parser, admission and durable-success HOST
tests. Then integrate storage, PC reconnect and idempotent confirmation. Preserve
the existing synchronous RAM-only FTP storage contract. Do not silently replace
its backend or inherit its acceptance verdict.

The new operations are `RETR boot-status`, `STOR firmware` and `STOR confirm`.
Use TYPE I for all three. Status has the specified 21 ordered ASCII LF keys and
a 1024-byte limit. Confirmation has exactly four once-only keys and a 128-byte
limit. Firmware has a signed 160-byte manifest and exactly its declared payload.
Authenticate and admit before destination invalidation or erase. Durable READY
readback must precede upload success and the reset request. Durable confirmation
readback must precede confirmation success. A matching confirmed duplicate writes
nothing. The PC reconnects and checks exact running image and trial status after
upload, even if the completion reply was lost. Missing status means UNKNOWN.

Production trampoline and startup object placement, clock/watchdog continuity,
the candidate deadline from application startup, final link/32 KiB fit and
physical service gaps remain separate open gates. This HOST intent transaction
does not execute an application or implement FTP, a network service or PC tools.
