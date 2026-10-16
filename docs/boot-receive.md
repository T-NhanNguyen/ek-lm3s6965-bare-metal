# HOST firmware receive transaction

Use `include/lm3s6965/boot_receive.h` for a separate firmware transaction.
This API does not implement FTP, start a listener, or replace the synchronous
RAM `ftp_storage_ops_t` backend. It composes the real verifier, lifecycle,
commit-last store, and injected flash controller. Reset is an output intent only.
Behavioral test execution is deferred to test-merit-gate.

## Ownership and admission

Initialize fresh unpublished storage with `BOOT_RECEIVE_INIT`. Call
`boot_receive_begin` once. Keep exclusive foreground ownership of the receiver,
controller, metadata pages, both images, and callback state until termination.
No ISR, DMA, debugger, or alternate writer may bypass this ownership. Do not
copy or reinitialize an admitted receiver. Callbacks must not mutate receiver
state, images, or output. Every receiver callback runs under the receiver guard.
Callback reentry returns BUSY. Invalid pre-admission arguments allow correction.
Every admitted begin, including a failing begin, consumes the transaction.

Begin loads fresh authoritative metadata. It reserves two nonwrapping sequence
advances before mutation. It asks the caller for independently authenticated,
stable evidence of the image actually executing. The callback must return
successful `boot_verify_stored` evidence, not a selected role or an invented
identity. The HOST tests authenticate the modeled running bytes in that callback.
C structs cannot prove provenance or that code really executes at that address.

Supply the explicitly permitted inactive physical slot. Signed target/base and
lifecycle admission must agree with it. Running must equal current confirmed.
READY or TRIAL candidates prohibit reception. Invalid metadata, evidence,
manifest, target, admission, or exhausted sequence causes no destination write.

## Bounded input and work

1. Call `boot_receive_feed` with available bytes and a separate accepted-count
   output. Feed performs no IO. It accepts at most the remaining 160-byte
   manifest, or enough payload bytes to complete one four-byte word. A call
   crossing the manifest boundary accepts only the manifest prefix. Retain the
   unaccepted suffix. WAIT means backpressure with zero accepted bytes. Zero
   bytes are harmless and do not mean EOF.
2. Call `boot_receive_step` with a separate acceptance output. Each call performs
   one phase action. Authentication and admission precede invalidation. A real
   metadata replacement and authoritative committed readback clear only the
   destination. This replacement is required even when it was already empty.
3. Further steps erase one page each, with the old destination manifest page
   first. All 110 destination pages are erased before payload programming.
   Each program step writes at most one word. Feed may accumulate that word
   across arbitrary fragments. No padding is invented for incomplete words.
4. Call `boot_receive_eof` only for clean transport EOF. Short manifest, short
   payload, or trailing input stops the transaction. Exact EOF may leave one
   complete buffered word for the next program step. The declared payload is
   always a multiple of four. Step stores the 160-byte manifest one word at a
   time only after exact EOF and completed payload programming.
5. Step reads back the stored manifest exactly and calls the real stored
   verifier. Signature, hash, physical bounds, and vector rules must pass.
   The next step commits READY through the real store and committed readback.
   Only ACCEPTED publishes an owned metadata/image result and reset-request
   intent. It neither resets nor executes the image.

Manifest authentication, metadata replacement, and complete stored verification
are synchronous opaque work units. Hash reads are bounded at 256 bytes, but
verification is not resumable between calls. Flash polls bound callback work,
not elapsed time or flash-fetch stalls. One page/word per step allows scheduling
between steps; this does not prove network, watchdog, SRAM, or physical timing
margins. The receiver buffers 160 manifest bytes and one payload word, not the
whole image. Crypto and store still have their existing scratch/heap needs.

## Failure and continuation rules

All admitted failures are terminal. `failed_phase`, `failure`, and subordinate
store/flash/verifier/lifecycle results retain the first stop. No later phase or
cleanup IO runs. Abort/disconnect calls `boot_receive_abort`; it stops without
cleanup IO. It cannot revoke an accepted READY record.

Store uncertainty or a blocked controller returns RECONCILE. A commit can have
succeeded even when a later read failed. Stop, recover the controller if needed,
and explicitly reload authoritative metadata. Resolve the outcome externally.
Never retry a cached proposal, replay reset accounting, or restart this object.
Read-only reconciliation does not revive it. Valid terminal duplicate calls
return CONSUMED without IO or another acceptance. Output stays unchanged
through callbacks and every error, and remains independent of receiver storage.

Before invalidation the prior authoritative record remains. After committed
invalidation the destination is absent until durable READY. A failed READY
readback can leave durable READY; it is an uncertain acceptance, not a rollback.
Current image bytes, identity, confirmation, and retained history are preserved.
There are no per-handoff writes or reset-accounting calls.

## Watchdog continuity

The receiver deliberately takes no watchdog pointer. It cannot copy, initialize,
transfer, report to, or feed one. Continue the same APPLICATION watchdog object,
epoch, and lifetime verification history outside these calls. Report only real
foreground Ethernet, timer, and FTP progress. Verifier observer events permit
cancellation; they cannot stand for application progress. Do not map them to
blind feeds. The regression fixture retains a real transferred APPLICATION
watchdog, existing history, and partial genuine evidence through reception.
Opaque crypto/store/flash gaps remain unmeasured.

## Build and gate

The standalone project builds `boot_receive` and registers `boot_receive` and
`boot_receive_fault`, for 34 total CTest entries. The second entry uses a separate
allocator-fault crypto/verifier/receiver copy; production has no fault hooks.
Root target `lm3s6965_boot_receive` is opt-in and requires final consumers to
supply verifier/crypto linkage. Neither archive is a linked boot image.

```sh
cmake -S tests/boot -B /tmp/firmware-receive-build-debug -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build /tmp/firmware-receive-build-debug -j 4
cmake -S tests/boot -B /tmp/firmware-receive-build-release -DCMAKE_BUILD_TYPE=Release -DBOOT_SANITIZERS=ON
cmake --build /tmp/firmware-receive-build-release -j 4
```

Tests use signed public fixtures and modeled register/flash IO with the real
components. They cover both inactive slots, empty/resident destinations,
fragmentation, exact framing, pre-mutation refusal, sampled before/after-effect
faults with exact callback prefixes, uncertain commits, stored verification
failures, frozen output, callback reentry, terminal duplicates, and explicit
read-only reconciliation. These are meaningful classes, not an exhaustive
callback/image/slot matrix. Maximum-size receive, arbitrary torn bit patterns,
transport scheduling, real MMIO, hardware reset, clock continuity, final link,
and 32 KiB fit remain outside this evidence. Existing verifier tests cover
signed maximum-size validation separately.

Test-merit-gate owns behavioral execution. Use the frozen manifest/verifier,
exact commands, counts, and budgets in `/tmp/firmware-receive-worker-handoff.md`
and `/tmp/firmware-receive-test-intent.md`. No build result is a behavioral pass.
