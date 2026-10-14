# Logical boot handoff gate

`include/lm3s6965/boot_handoff.h` and `src/boot_handoff.c` implement a host-model
admission and ordered trace gate. Success records **terminal entry intent**, not
physical execution. This module is opt-in and is not part of BSP startup.

## Admission and authority

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
vector must be Thumb; its cleared target must lie in the authenticated half-open
`[base, base + length)` interval. Targets inside the vector table remain allowed:
**there is no new target >= vector-table-end rule**. This matches the verifier's
existing boundary, not an ELF entry-point rule.

Context admission requires IPSR=0, privileged Thread mode and CONTROL=0 or 2
(MSP or PSP allowed). PRIMASK and FAULTMASK may each be 0 or 1. This part has
three priority bits: BASEPRI may be `0,32,64,...,224`; other bits reject. These
are representability checks, not an inherited reset-state requirement. Final
intent contains selected VTOR/MSP/original Thumb vector and CONTROL, PRIMASK,
BASEPRI, FAULTMASK **all zero**, plus the advanced application watchdog epoch.

Run rejects a nonconfigured, blocked, nonboot-owned, stale-epoch or busy watchdog
before any callback. Epoch zero or UINT32_MAX rejects; UINT32_MAX-1 permits the
last one-way transfer into UINT32_MAX. Exhausted later service remains subject
to the watchdog API. No watchdog IO, configuration, clock change or feed is
performed by handoff. No metadata write or new eligibility decision occurs.

## Ordered trace and terminal boundary

The operation callback receives these logical events in exactly this order:

1. Establish host exclusion; stop SysTick; disable external interrupt delivery
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
intent only. A future physical implementation needs separately reviewed assembly
with no old C frame after MSP switch and no return, plus target register/barrier,
source coverage and clock/watchdog continuity evidence.

## Failure and ownership contract

Callbacks return zero only after completing their operation. A nonzero result
may occur before or **after** effects. Invalid admission produces no callbacks
or output change. An admitted callback error immediately quarantines handoff:
no later callback, cleanup, implicit exclusion release, retry or rollback. The
software reentry guard is released; **host exclusion is not**. These are distinct
mechanisms. A failed EXCLUDE may itself already have acquired exclusion.

The caller owns the whole transaction, including all interrupt sources, selected
storage, context, watchdog/controller and both objects. Atomic guards only reject
nested calls; they are not cross-module hardware locks. Callbacks must not mutate
objects/IO/output or call watchdog APIs. The preflight briefly checks the watchdog
guard then releases it so the existing transfer API can acquire it. Concurrent
watchdog use across this interval is forbidden by the caller contract.

The `boot_handoff_run` output storage must be disjoint from the request, callback
context storage and both handoff/watchdog objects. Request snapshotting does not
relax this requirement: output remains unchanged throughout every callback and
is published only after full success. This is a caller storage contract, not a
runtime alias-detection guarantee.

Before terminal intent, a failure leaves boot ownership intact; caller must not
resume boot work or feed via another API. Stop and retain watchdog protection.
At terminal intent the watchdog has already transferred to application ownership,
advanced epoch and consumed evidence. **Terminal failure does not roll this back.**
The callback may have recorded the intent before reporting failure. Neither
absence of entry intent nor physical application execution can be inferred from
its error. The safe host response is quarantine, not continuation or another
entry attempt. Output is published only on complete success. Successful handoff
also cannot be repeated; it leaves host exclusion logically held.

Never reinitialize either object to bypass terminal state or quarantine. No blind
feed, ICR/LOAD/CTL access, clock removal or disabling protection is permitted.
Application continuation and actual service gaps are later composition gates.

## Local evidence and scope

The local `datasheets/LM3S6965 Microcontroller Data Sheet.pdf` (July 15, 2014)
provides the processor model on pp. 57–58; IPSR on pp. 64, 67; PRIMASK/FAULTMASK on
pp. 68–69; BASEPRI bits 7:5 on p. 70; CONTROL ASP/TMPL (SPSEL/nPRIV) on p. 71.
`include/lm3s6965/interrupts.h` defines IRQ count 43, three priority bits and
59 vector entries; `src/startup.c` uses that geometry. These support admission
and the **logical** bank model. They do not establish exact MMIO clear masks,
source shutdown coverage or physical barrier semantics for this module.
PDFKit extraction was used locally because document tools were unavailable;
extracted evidence and command provenance are saved in `/tmp/boot-handoff/`.

Tests cover both slots, authenticated boundaries (including targets within the
vector table), context representations, invalid admission with unchanged output,
watchdog state/epoch/busy rejection followed by corrected successful retries on
the same objects (including releasing the busy watchdog guard), individual NULL
run/admit/init arguments and missing init callbacks without reinitialization,
reentry at every callback, input mutation, disjoint sentinel output unchanged
through every callback and published only on full success, repending sources
and final-clear order, exact intent and all 12 callback failures
before/after effects in both slots (48 cases). They verify no watchdog IO or feeds,
no subsequent operations and one-way terminal transfer even after terminal error.
No host test proves elapsed watchdog timing or actual CPU entry.

```sh
cmake -S tests/boot -B /tmp/boot-handoff/debug -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build /tmp/boot-handoff/debug
ctest --test-dir /tmp/boot-handoff/debug --output-on-failure
```

Standalone archive: `boot_handoff` (links `boot_watchdog`). Root opt-in archive:
`lm3s6965_boot_handoff` (links `lm3s6965_boot_watchdog`). Arm code generation
also references the existing C runtime `memcpy`/`memset` for snapshots and tuple
initialization; these archives are not freestanding dependency-closure links.
Neither archive proves
final boot fit, actual MMIO/interrupt binding, no-return assembly, boot composition,
FTP/PC behavior, hardware safety or full-MVP acceptance.
