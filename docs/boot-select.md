# Verified boot selection (HOST composition)

`boot_select` in `include/lm3s6965/boot_select.h` returns an owned verified image
and authoritative metadata snapshot only on `BOOT_SELECT_OK`. It does not jump,
provision, capture/account resets, feed/transfer a watchdog, or implement recovery
tooling. `boot_launch_candidate` remains the candidate-only transaction API.

## Caller boundary

1. Complete reset accounting with `BOOT_RESET_HANDLER_OK` **in this boot**, and
   supply that final authoritative snapshot (or explicitly reconciled authority
   after a later selection transaction). A failed reset report is never permission
   to proceed. Struct fields are forgeable, not provenance.
2. Establish watchdog protection before verification. Supply physical A/B manifest
   prefixes in `boot_select_images_t`, an honest physical payload reader and an
   optional verifier-progress callback. `size[slot]` is available prefix length;
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
overlap any input or context storage. Known object spans are checked before IO;
opaque context sizes remain caller obligations. Callbacks must not modify output.
There is no mutable policy global, runtime lock, automatic retry or reentry guard.

## Selection and durable ordering

| Authority / evidence | Action |
| --- | --- |
| READY/TRIAL candidate | Attempt first through the existing real verifier/lifecycle/store candidate transaction. READY requires durable TRIAL/readback. TRIAL independently verifies and rechecks authority with zero writes. |
| Candidate IMAGE_INVALID | Independently verify identity-matching confirmed CURRENT. Do **not** count, reject, clear intent or modify any candidate metadata. Successful fallback publishes the exact original authority with zero writes. |
| No candidate, confirmed CURRENT valid | Independently authenticate/hash/check vectors and exact resident identity; recheck canonical authority and selected page immediately before zero-write publication. |
| CURRENT image-invalid, eligible confirmed PREVIOUS | Independently authenticate/hash/check vectors and resident identity of PREVIOUS; call `boot_lifecycle_promote_previous`, then require `boot_store_replace` OK and authoritative committed readback before publication. |
| Missing roles, neither valid, metadata absent/conflicting | Explicit RECOVERY: no executable decision, guessed factory image, automatic provisioning or candidate-as-previous promotion. |

Only FORMAT, TARGET, SIGNATURE, HASH, MSP, THUMB and VECTOR_RANGE prove image
invalidity. ARGUMENT, CRYPTO, READ and ABORTED are operational stops. Identity
mismatch, stale authority, sequence exhaustion and store errors are **not**
invalid-image evidence and cannot authorize trying another role. A confirmed but
roleless entry is not PREVIOUS. With two slots, current+candidate has no eligible
previous; current+previous has no candidate. PREVIOUS cannot run until promotion
is durable. Both resident entries' identities, trial IDs, counts, reasons and
rollback history are retained by promotion; only required roles/confirmation and
sequence change. No verification failure opportunistically adds a count/reject.

Maximum sequence permits normal CURRENT and TRIAL zero-write selection, including
invalid-candidate fallback. It prevents READY first-TRIAL and PREVIOUS promotion.
Freshness compares canonical encoded metadata **and** the codec-selected page,
not padding or sequence alone. This detects sampled stale authority, not races;
violating stable ownership remains unsupported.

## Status and reconciliation

`boot_select_result_t` carries status, per-slot verifier evidence and exact store
and lifecycle component results. `evidence_mask` marks meaningful verifier entries;
it is not a complete attempt trace (candidate store failure may precede or follow
verification). Unset/default OK entries are not authentication evidence.

- `VERIFY_FAILED` is operational verifier failure; `IDENTITY_FAILED` is confirmed
  role resident mismatch. Candidate identity failure is `LIFECYCLE_FAILED` with
  `BOOT_LIFECYCLE_ERROR_IDENTITY`, preserving the existing transaction boundary.
- `LIFECYCLE_FAILED` includes sequence exhaustion. `STORE_FAILED` includes invalid
  caller snapshots, stale authority, IO and precommit VERIFY errors. None falls
  back. `RECOVERY` also reports exact NO_RECORD/CONFLICT when store supplied it.
- `RECONCILE` means exactly `BOOT_STORE_UNCERTAIN`. Stop. The API does not reload,
  retry, publish or assume rollback. Explicitly load and reconcile old/new/recovery
  authority, then make a fresh call if appropriate. Never replay cached READY or
  a previous-promotion proposal. Reset-accounting success is still a prerequisite.

## Builds and pending execution gate

The standard `tests/boot` project discovers `boot_select` and `boot_select_fault`
in addition to its 24 existing tests (26 total). Build all, discover, then request
an unfiltered execution gate; discovery is not execution:

```sh
cmake -S tests/boot -B /tmp/boot-select/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/boot-select/debug -j4
ctest --test-dir /tmp/boot-select/debug -N
# Execution only after test-merit-gate approval:
ctest --test-dir /tmp/boot-select/debug --output-on-failure
```

Repeat for Release. ASan/UBSan follow existing defaults. Tests reuse real crypto,
verifier, lifecycle and store, the public independently signed OpenSSL fixtures
and exact payload definition in `tests/boot/verify-fixtures.md`, plus the existing
local flash model and allocator-intercepted crypto variant. No new private key,
signing dependency or verifier mock is introduced.

Regressions cover both slots/pages, normal/READY/TRIAL and maximum sequence,
invalid-candidate zero-change fallback, all seven invalid-image results by role,
operational read/cancellation/crypto faults and identity mismatches by role,
missing roles/metadata/conflicts, exact durable promotion/history, every store
callback cut before/after effect, stale canonical/page authority, silent staging
and readback faults, explicit old/new/recovery reload and unchanged output through
callbacks/errors. Alias tests cover every intersecting byte displacement of each
known input. Adversarial callback input/authority mutation probes copying/freshness,
not permission for mutation or concurrency. No runtime reentry detection is claimed.

These are implemented tests, not a test-pass claim: execution is deferred to
`test-merit-gate`. Root `lm3s6965_boot_launch` and standalone `boot_launch` archives
include selection and retain their existing dependency contracts. Archives are
not a final linked image, dependency closure, 32 KiB fit, measured timing, physical
MMIO/reset/flash, watchdog bridge/transfer, deadline, FTP/network or full-MVP gate.
