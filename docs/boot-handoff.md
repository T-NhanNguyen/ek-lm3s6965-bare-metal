# Logical boot handoff gate and isolated target fixture

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
Neither archive proves final boot fit, actual MMIO/interrupt binding, terminal
assembly integration, boot composition, FTP/PC behavior, hardware safety or
full-MVP acceptance.

## Isolated Cortex-M3 terminal increment (QEMU only)

`tests/boot/handoff_terminal.S` is deliberately **fixture-only**. No production
archive, BSP startup or host contract is changed. Its register ABI is r0=selected
VTOR, r1=validated initial MSP, r2=original authenticated Thumb reset vector.
It does not validate inputs, transfer ownership or shut down sources. Preconditions:

1. Privileged Thread mode, no active exception, exclusive transaction ownership;
   inherited CONTROL=0/2 and representable masks as described above.
2. PRIMASK already 1; SysTick stopped, external delivery disabled, peripheral
   work and interrupt producers stopped, final pending clears completed.
3. Selected image/table stable and authenticated, exact MSP `0x20010000`,
   application startup compatible with continued watchdog protection.
4. Same configured watchdog object already transferred once; its clock,
   controller and object lifetime remain intact through application startup.

All input values are in registers before switching MSP/CONTROL. The assembly
completes a DSB, writes VTOR, executes DSB/ISB, sets LR to a Thumb return trap,
sets MSP with immediate ISB, sets CONTROL=0 with immediate ISB, clears BASEPRI
and FAULTMASK, and only then clears PRIMASK and branches through original r2.
There is no C frame, stack operation, memory access after the MSP switch, call,
restore or return in this primitive. LR traps a reset routine that returns.
NMI/fault hazards are not removed by PRIMASK; callers must establish a safe
system, not infer safety from exclusion alone. This is not a reset of all core
state (for example PSP, priorities and fault status are not reset).

### Evidence and limits

The local MCU PDF was re-extracted with PDFKit because dedicated document tools
were unavailable. `datasheets/LM3S6965 Microcontroller Data Sheet.pdf`, July 15,
2014: masks/CONTROL pp. 68–71; stack-switch ISB p. 71; DSB/ISB and vector ordering
pp. 75–76; SysTick stop/count/current semantics pp. 106–109; NVIC clear-enable
pp. 112–113 and clear-pending pp. 116–117; pending SysTick/PendSV clear commands
pp. 124–125; VTOR fields/alignment p. 127. Clear-enable and clear-pending are
write-one commands, not read-modify-write. Bank masks are `0xffffffff` and
`0x00000fff`: the register description includes unused IRQ 43 (bit 11), while
the established 43-IRQ host model ends at IRQ 42. No host geometry changes.
ICSR uses only clear command bits 27/25 (`0x0a000000`), never a mixed set/clear
RMW. Barriers are actual instructions, not conclusions from host mock events.

`test_handoff_target.py` builds at `-Os` and `-Og` and requires compiler/QEMU
(no silent skips). Each ELF runs **128 cases in one shared QEMU machine**:
both selected VTOR bases × MSP/PSP × both inherited PRIMASK values × both
FAULTMASK values × all eight BASEPRI values. Cases share fixture BSS and the
once-transferred watchdog; they are not 128 fresh boot/reset trials. Each
optimization level launches a separate QEMU process. Assembly samples entry MSP/CONTROL/IPSR/masks/selected entry
before C prologue. Real QEMU SysTick/NVIC register effects, selected VTOR readback
and distinct A/B SVC handlers establish target-emulated evidence. The fixture
seeds enabled interrupt state under PRIMASK exclusion, injects a **software
producer**, stops it, then disables NVIC delivery and finally clears pending state. The producer is not a
modeled Ethernet/DMA device or proof of physical source shutdown coverage.
QEMU retains SysTick CLKSOURCE=1 after a zero control write; assertions check
ENABLE/INTEN/COUNTFLAG clear and CURRENT=0 rather than claiming CLKSOURCE reset.

Two distinct old-stack witness words (one per inherited stack) are written
after the final fixture C return and checked in the application. Preserving
these two words does not prove the whole old stacks are untouched: other
addresses could be accessed, and reads would not alter either word. The
stackless claim for the reviewed terminal/capture prefix rests on the exact
disassembly checks, not these limited runtime witnesses.
The original Thumb reset vectors point to distinct entry stubs inside their
respective slots; shared capture/check code remains fixture infrastructure.
Entry checks preserve the witnesses and capture the Thumb return-trap LR before
any C call; disassembly checks enforce the current exact terminal instruction
sequence and operands, immediate ISBs after MSP/CONTROL, the VTOR literal and
return-trap target. The allowlist excludes all extra memory/branch/stack/call
operations and premature mask clearing. Capture is parsed by instruction rows
through its first call, with the exact register samples/stores and the literal
address of the `captured` data symbol checked against the ELF symbol table.
Both slot-entry stubs must contain exactly the slot-ID move and a non-linking
branch to capture, preserving LR and avoiding stack access. The fixture uses real `boot_watchdog_init/configure/transfer` with
**synthetic watchdog IO**, transfers once before the case sweep and observes
same-object application ownership/epoch and unchanged bytes across entry, with
no transfer IO/feed and no second successful transfer. Intentionally preserved
fixture BSS is **not evidence that production `.data`/`.bss` startup preserves
the object**. Physical watchdog countdown, clock continuity and timing remain
unknown; this test does not depend on QEMU watchdog support.

```sh
ctest --test-dir /tmp/boot-handoff-target/debug \
  -R 'boot_(handoff|watchdog)' --output-on-failure
```

Each wrapper invocation creates and prints a retained unique
`/tmp/boot-handoff-target/run-*/` directory for its command log, tool versions,
ELFs, disassembly and QEMU logs; parallel Debug/Release invocations do not share
those files. QEMU stdout/stderr is saved before raising on nonzero exit or
timeout, as well as before checking the success marker; the command log records
command, configured timeout and exit/timeout status. Launch errors retain an
empty output log and their error status. Explicit Python error gates remain active under `python -O` and
`PYTHONOPTIMIZE`. Older shared-root artifacts, local PDF extraction and prior
execution logs are historical evidence, not execution of the current wrapper.
The review follow-up validates compilation and static disassembly only; current
optimized-mode rejection probes, concurrent wrapper execution, registered target
CTest and full host suites are deferred to `test-merit-gate`. Commands are in
`/tmp/boot-handoff-target/review-followup-report.md`.
Production binding, integrated
boot composition/link/fit, startup storage survival, actual peripheral shutdown,
physical timing and board acceptance remain separate gates.
