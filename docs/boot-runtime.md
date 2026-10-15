# One-shot reset → selection (HOST decision only)

`boot_runtime_run` composes the existing `boot_reset_handler_run` and
`boot_select`, using the real verifier, crypto, lifecycle and commit-last store.
It does not execute an image, bind MMIO, establish/service a watchdog, impose a
candidate deadline, or implement production startup/handoff/FTP integration.

## Caller lifecycle

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
callback**. Every admitted failure and success is terminal; duplicates return
CONSUMED with no callbacks, capture, clearing, accounting or selection. There is
no same-object selection-only retry.

Do not reinitialize, copy or recreate the object during the same boot, or bypass
it with raw reset/selection APIs. This is a caller contract, not a global mutable
latch or persistent exactly-once mechanism. A new boot needs a fresh object and
fresh capture; never replay cached flags. The guard does not establish physical
exclusivity or protect against arbitrary external calls/input mutation.

## Ordering and effects

The actual handler captures the **original raw flags**, reads fresh reset flags
and writes `fresh & ~(captured & 0x1f)` (R/W, **not W1C**), then loads authoritative
metadata, classifies the original capture and makes at most one accounting
commit. Unsupported or newly set uncaptured bits are retained. Zero supported
capture skips fresh register read/write. Fresh flags never change classification.

Uncounted resets preserve valid pending intent with **zero accounting writes**.
Applicable counted TRIAL resets consume matching intent, count and reason in one
commit; count three rejects and clears the candidate before selection. Only
handler OK's final snapshot enters the selector. The selector can independently
commit READY→TRIAL or eligible previous promotion, so zero accounting writes does
**not** necessarily mean zero total writes. Candidate image-invalid fallback and
operational failure distinctions remain [selector policy](boot-select.md).

Adapters and both manifest prefixes are snapshotted before any callback. Valid
inputs may alias each other, but not the runtime object. Output must be disjoint
from inputs, runtime and opaque context storage (opaque sizes are caller
obligations). Callbacks must not mutate them. Output stays unchanged throughout
all callbacks and on any failure; full success publishes an owned copy.

## Failure/report authority

The returned composite `valid` bits identify which stages were attempted.
`RESET` makes `reset_status` and the partial reset report available; the nested
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

Clear-before-commit interruption can lose an event; same-bit relatch can be lost;
a stale matching deadline intent can classify a later SW reset. No event journal,
persistent replay guarantee, physical register behavior/timing, final integrated
link/dependency closure or 32 KiB fit claim is added.

## Build and deferred test gate

The normal `tests/boot` project registers `boot_runtime` and `boot_runtime_fault`
through `tests/boot_launch`, with real signed A/B fixtures, flash model and the
existing allocator-intercepted crypto variant. Build-all/discovery/unfiltered
CTest includes both. Standalone ARM archive target is `boot_runtime`; root
opt-in archive target is `lm3s6965_boot_runtime`. Archives are not a final image.

Regressions cover a unified callback trace, original/fresh classification, both
slots/pages, all supported flags and counts 0..3, pending intent, third rejection,
READY/promotion writes, every flash callback cut before/after effect, explicit
old/new read-only reconciliation without replay, callback reentry, duplicate
no-IO behavior, frozen/owned outputs, snapshot mutation probes and structural
arguments. Test execution is delegated to test-merit-gate; build success alone
does not claim passing tests or acceptance.
