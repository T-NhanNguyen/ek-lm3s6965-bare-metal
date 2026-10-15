# Runtime → verifier → watchdog bridge (HOST only)

`boot_runtime_watchdog_run` wraps the unchanged one-shot [runtime](boot-runtime.md)
and real selector/launch/verifier callbacks. It returns an owned decision, **not
execution or handoff permission**. It does not configure a controller, bind MMIO,
install an ISR, transfer ownership or establish physical timing.

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

Standalone ARM archive target: `boot_runtime_watchdog`; root opt-in archive:
`lm3s6965_boot_runtime_watchdog`. Both depend on runtime and watchdog archives;
consumers still supply verifier/crypto and final-link policy. Neither is a boot
image or an integration/fit/timing acceptance claim.
