# Injected boot watchdog ownership and progress

`include/lm3s6965/boot_watchdog.h` / `src/boot_watchdog.c` implement a bounded
host-tested policy, **not a physical driver or complete boot runtime**. The
FLASH adapter from local commit `0b81f97` is unchanged. No ISR, MMIO binding,
clock transition, debug/sleep policy, candidate deadline, FTP composition or
MSP/VTOR handoff is installed.

## Register contract and initial protection

The local *LM3S6965 Microcontroller Data Sheet*, July 15, 2014, pp. 371–382,
is the authority (not TM4C/Tiva registers):

- System clock drives WDT. RCGC0.WDT is bit 3 (pp. 214–215). Caller enables it, establishes
  peripheral readiness and the actual stable clock before configuration.
- Base `0x40000000`: LOAD `0`, VALUE `4`, CTL `8`, ICR `0xc`, RIS `0x10`,
  MIS `0x14`, TEST `0x418`, LOCK `0xc00`. Only LOAD/CTL/ICR/LOCK are written.
- Unlock writes `0x1ACCE551`; lock writes `0`. **LOCK reads 0/1**, not the key.
  CTL is read-modify-written, preserving reserved bits, with INTEN and RESEN
  enabled **together**. After INTEN all CTL writes are ignored until hardware
  reset; an already enabled controller is rejected, not “repaired” or adopted.
- First zero sets the interrupt and reloads; second zero before clear resets.
  LOAD reloads but does **not** clear an interrupt. Configuration writes LOAD,
  clears stale interrupt with ICR and checks RIS before enabling CTL. It locks
  and checks CTL/LOAD/LOCK before reporting protection established.
- Service writes ICR (any value clears interrupt **and** reloads), checks RIS,
  then relocks. ICR is write-only with indeterminate read value; no ICR readback
  or comparison of the changing VALUE to LOAD is fabricated. RIS clear alone
  cannot prove reload if it was already clear: success callbacks must truthfully
  mean the write reached the register. A new first-stage event during readback
  can conservatively cause a fault; no cycle-exact observation is claimed.

`boot_watchdog_init` initializes fresh unpublished software storage exactly
once; it does **not** configure hardware. Call `boot_watchdog_configure` before
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
remain unmeasured; the caller must validate them before hardware use.

## Ownership and evidence

All calls are foreground-only. The caller exclusively owns the controller and
object; do not mutate fields, change clocks, run independent register writers,
alias inputs, or reinitialize to escape a fault. An atomic entry guard rejects
callback reentry/concurrent calls with BUSY; it does not serialize external
controller users or make ISR use valid. Callbacks return zero for truthful
success; nonzero may already have affected hardware.

1. Boot begins at **epoch 1** after init/configure. Every report/service names
   the current epoch. Rejected reports/insufficient evidence cause no register
   access. A successful service consumes evidence and advances the epoch.
2. `boot_watchdog_boot_report` accepts actual existing verifier events with a
   caller-assigned nonzero monotonically increasing verification ID. BEGIN
   requires the verifier's checked total, resets stream/evidence and grants no
   feed. AUTHENTICATED supplies one milestone only after successful signature
   completion. PAYLOAD_PROGRESS must advance cumulative successfully read+hashed
   bytes by exactly the next verifier chunk (256 or all remaining bytes for the
   final chunk) within the unchanged total; duplicate, incomplete, backwards, oversize,
   wrong-stream and stale-epoch reports fail. Several milestones before service
   coalesce into one permission, never a bank of future feeds. A new BEGIN can
   abandon a failed stream, discarding evidence; IDs must not recycle.
3. `boot_watchdog_transfer` makes a **single boot→application** software ownership
   transition, advances epoch and discards boot evidence. It performs no register
   operations/feed, including on epoch exhaustion. The enabled counter continues;
   old boot reports/services and repeat transfer cannot feed it. Physical transfer
   and application preservation of this software object are future glue, not
   supplied here; application must not call init/configure again.
4. Application reports exactly one completed predicate at a time: Ethernet poll/
   drain, all due network timers, FTP state-machine work. All three are required
   for service; each report is accepted once per epoch and `work_complete` must
   equal 1. Idle polls, no timers due and no FTP client count as completed service
   passes. **Client traffic is not required**. Work that is merely scheduled,
   entered, stalled or failed does not count. These are caller attestations,
   not automatic observations, startup health or confirmation bits.
5. Confirmation does not change this policy: the confirmed application remains
   protected with the same foreground obligations. There is no candidate
   60-second timer or metadata operation in this increment.

A verifier callback bridge must use the **current epoch on each callback** and
one stable verification ID per invocation; callbacks are scheduling points, not
blind feed instructions. BEGIN cannot feed during nonrestartable ECDSA; no new
callbacks or verifier rewrite are introduced. That indivisible operation and
all boot/flash/application startup work between milestones must fit the actual
budget. This API does not prove their timing or compose them automatically.

## Faults and limits

Any admitted register I/O/readback fault permanently quarantines the object,
clears evidence and reports failure. After an attempted unlock, best-effort
relock is cleanup only; even successful cleanup never converts failure to
success. No CTL disable, LOAD/ICR retry or progress reuse occurs. Before unlock,
failures do not attempt unclocked watchdog cleanup. A failed feed may already
have reloaded, and failed lock/cleanup may leave writes unlocked. No software
“recover” API adopts this ambiguous state: reset/reestablish hardware under
separate integration authority. Caller must not continue verification/handoff
on failed configuration or report a failed feed as success.

Owner/epoch rejection and incomplete evidence are non-faulting, callback-free
rejections; epoch exhaustion instead quarantines without wrap. C structs and
IDs are not security/provenance tokens: a dishonest caller can fabricate work
or repeat a signature with a new ID. Actual code must report genuine work.
No unconditional interrupt feed, indefinite progress bank, clock rewrite or
silent protection disable is provided.

## Host and Arm gates

Standalone host project (existing local dependencies/toolchain/emulator only):

```sh
cmake -S tests/boot -B /tmp/boot-watchdog/debug -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build /tmp/boot-watchdog/debug --parallel 8
ctest --test-dir /tmp/boot-watchdog/debug --output-on-failure
# Repeat with /tmp/boot-watchdog/release and -DCMAKE_BUILD_TYPE=Release.
```

`test_watchdog.c` models real CTL write-once, LOCK status, ICR and LOAD semantics;
it tests callback failures at every configure/feed boundary before/after effects,
ignored writes/useful readback faults, persistent cleanup failure, reentry,
configuration/reconfiguration, stream replay, ownership, all application
predicate subsets, idle service and finite budget/epoch limits. Release assertions
remain enabled (`-UNDEBUG`), with ASan/UBSan in the host suite. Standalone Arm
archive `boot_watchdog` and root opt-in `lm3s6965_boot_watchdog` compile without
crypto linkage, startup, BSP, lwIP or linker scripts. Including verifier types
is not verifier composition. Archive/header compilation is **not** final boot
link/32 KiB fit, hardware timing, full handoff or integrated FTP acceptance.
