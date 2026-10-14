# Verified candidate-launch decision

`boot_launch_candidate` in `include/lm3s6965/boot_launch.h` combines the real
stored-image verifier, lifecycle proposal and commit-last metadata store. It
publishes a decision. It does not execute an image or perform a handoff.

## Caller boundary

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

## Transaction

| Step | READY | TRIAL retry |
| --- | --- | --- |
| Initial authority | Load both pages and compare exact encoded record and selected page with caller snapshot | Same |
| Image | Independently authenticate manifest, stream/hash exact payload and check vectors for the physical candidate slot | Same; no cached verification |
| Identity | `boot_lifecycle_begin_trial` checks exact resident slot/hash/release/length | Same |
| Proposal | Exactly next sequence, READY to TRIAL; preserve trial ID and history | Exact NO_CHANGE, including maximum sequence |
| Durability | `boot_store_replace` rechecks authority, erases alternate page only, programs commit last and checks committed readback/selection | Load both pages again and reject any authority change; zero erase/program |
| Publication | Exact verified image plus authoritative TRIAL snapshot after `BOOT_STORE_OK` only | Exact verified image plus freshly checked authoritative snapshot |

No launch count, new trial ID, per-handoff write, confirmed selection, fallback
or recovery promotion is added. READY at exhausted sequence cannot launch. TRIAL
retry at maximum sequence can launch without a write. No candidate means
`BOOT_LAUNCH_INELIGIBLE`, not a request to choose a different image.

## Failure and reconciliation

`boot_launch_result_t` returns a status and the exact failing component result.
Consult `verify_result` only for IMAGE_INVALID/VERIFY_FAILED, `lifecycle_result`
only for LIFECYCLE_FAILED and `store_result` only for STORE_FAILED/RECONCILE.
Unused fields have neutral defaults; they do not report completed steps.

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
boundary; do not use this wrapper to bypass it. The explicit reconciliation
route applies to a transaction after the successful reset pass, not to claiming
that a failed reset pass succeeded. Store readback cannot protect against a
false adapter, later corruption or violated exclusive ownership.

## Build and host gate

The established `tests/boot` entry point includes the launch tests as a supported
subdirectory. It discovers 24 tests (the existing 22 plus `boot_launch` and
`boot_launch_fault`); target-handoff registration and fixtures are unchanged.
Build all targets before requesting the unfiltered host gate:

```sh
cmake -S tests/boot -B /tmp/boot-runtime/review-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/boot-runtime/review-debug -j4
ctest --test-dir /tmp/boot-runtime/review-debug -N
# Run only after test-merit-gate approval:
ctest --test-dir /tmp/boot-runtime/review-debug --output-on-failure
```

Repeat with `Release` in `/tmp/boot-runtime/review-release`. Discovery is not test
execution or a full-suite pass claim. ASan/UBSan are enabled by
usual host defaults. Tests use the existing public OpenSSL signed fixtures and
exact payload construction, real crypto, lifecycle and store, and flash_model.
The allocator-fault target uses the existing isolated real-crypto interception,
not a verifier mock. No private signing key or new dependency is needed.

Coverage includes both slots/pages, durable READY, independently reverified
zero-write TRIAL, retained history/intent at maximum sequence, stale snapshots,
identity/verification failures, all READY store callback cuts before/partial/
after effects, TRIAL read failures, precommit verification faults, postcommit
UNCERTAIN, explicit old/new reload and later media-loss/conflict recovery.
Regression coverage also checks adapter/output aliases and partial input/output
overlaps, invalid snapshots/pages and missing required callbacks before any IO,
exact TRIAL read-error component results, and owned decision values after the
caller's source snapshot/manifest copies change after return. These checks do
not establish reset provenance or permit reentry or physical image mutation.
Every injected callback checks the success-output sentinel before and after its
work. An adversarial authority-change fixture deliberately violates exclusive
ownership to check rereads; it does not prove support for concurrent writers.
Reentry is prohibited, not claimed to be runtime-detected or tested as supported.

Root `lm3s6965_boot_launch` is an opt-in wrapper archive. Its consumer must link
verifier/lifecycle/store/format/crypto dependencies. The standalone project
provides those targets for host and Cortex-M3 archive builds. Neither archive is
an integrated boot image, dependency-closure proof or 32 KiB fit acceptance.
No MMIO, hardware, timing, reset composition, watchdog bridge, deadline or FTP
acceptance follows from this host gate. Test execution is a separate gate.
