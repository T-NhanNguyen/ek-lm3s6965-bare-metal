# HOST application service and PC workflow

`include/lm3s6965/boot_service.h` defines a separate HOST service API.
It does not register FTP commands. It does not change `ftp_storage_ops_t`.
The existing synchronous RAM backend stays unchanged.

## Status

Call `boot_snapshot_capture` once. Pass its successful owned output to
`boot_status_render`. The renderer does not sample sources again. It emits
exactly 21 ordered ASCII LF keys. It writes at most 1024 bytes. The returned
size excludes any NUL. The renderer does not add a NUL. It leaves output and
size unchanged on failure.

Metadata supplies roles, sequence, resident accounting, and the trial source.
The candidate entry supplies the trial when a candidate exists. Otherwise the
running entry supplies it. Running evidence supplies hash and release. The
snapshot context supplies health, stage, and reset availability. Historical
fault details need not match the current roles. Known zero stays zero. Valid
maximum sequence and trial ID stay numeric. Empty entries report `none`.
Permission requires running=current confirmed and no candidate.

The renderer accepts only a successful capture. It does not invent a partial
status when capture fails. A future transport must report failure in that case.
It must not send an old body as current status.

## Confirmation

Zero-initialize `boot_confirm_parser_t` for each STOR body. Feed arbitrary
fragments. Call `boot_confirm_finish` only at transport EOF. The parser accepts
exactly four keys once. It accepts any key order and decimal leading zeros
for format and trial ID. Format must have numeric value 1. Both decimal fields
must fit uint32. It rejects signs, whitespace, CRLF, NUL, non-ASCII, missing LF,
overflow, and bodies over 128 bytes.
It publishes a request only after full validation. Feed does not change storage.
Do not call the confirmation service before successful finish and clean EOF.

Zero-initialize one `boot_confirm_service_t` for the application session.
`boot_confirm_apply` captures fresh authority, actual running evidence, and
health. It calls the real lifecycle policy. A new confirmation uses the real
commit-last store and its committed authoritative readback. A matching confirmed
duplicate returns the captured durable result with no erase or program call,
including a retained trial ID of zero. It does not require renewed health.
The service does not reset the application or replay reset accounting.

The service rejects callback reentry. The caller must own the publication owner,
physical images, metadata pages, descriptors, and callback state across the full
call. The capture lock does not span the later store transaction. External
serialization must span both. No ISR, DMA, debugger, direct struct mutation, or
alternate writer may bypass that ownership. Running evidence must come from
prior independent stored-image authentication and actual execution. C structs
cannot prove that provenance. All input and output objects must be disjoint.

`BOOT_CONFIRM_OK` alone permits a future transport to report success. Other
results preserve output. `BOOT_CONFIRM_RECONCILE` blocks this service after an
uncertain commit. Stop. Reload authority explicitly and reconcile the outcome.
Do not automatically retry a cached proposal, reinitialize to bypass the block,
or replay reset accounting. A lost network reply after OK permits a fresh
matching confirmation request. It does not require a new metadata write.

## PC module

The [PC FTP client guide](boot-pc-ftp.md) describes the concrete bounded stdlib
adapter and CLI in `tools/boot/client.py`. The signer stays offline.

`tools/boot/workflow.py` takes an injected transport. The transport provides
`connect`, `binary`, `retr`, and `stor`. `binary` must select FTP TYPE I after
each connection. The module opens no sockets and starts no listener.

`Workflow.upload` reads status and chooses bytes by `permitted_slot`. It checks
the package target, exact framing, payload hash, and vectors. These local checks
do not authenticate ECDSA. The device must use its real verifier before mutation.
The workflow permits one firmware attempt. Even a successful STOR returns
`UNKNOWN` until status resolves the result. A lost reply never permits a blind
reupload through that workflow object. Preserve the object through reconnects.

`poll(attempts)` reconnects with a one-second interval and a caller-set bound.
It checks exact running slot, hash, release, retained trial ID, candidate state,
and health. Before a trial ID is pinned, retained old running identity with the
old confirmed role remains `UNKNOWN` during destination invalidation and READY.
Metadata sequence and candidate may change before reset. These observations do
not prove execution or success. Polling continues until the caller's bound.
An exhausted pending poll returns `UNKNOWN`, without another firmware upload.
Unavailable running slot, hash, or release also yields `UNKNOWN`. It cannot
permit confirmation. Known conflicting identity yields `MISMATCH`, except for
the pending old-image transition above. That exception ends once a trial ID is
pinned. Before binding a trial ID, require numeric pre-upload sequence, trial ID,
and observed sequence. Require `pre-upload sequence < trial ID <= observed sequence`.
The trial ID is the acceptance sequence. Metadata sequences do not wrap.
Apply this check to TRIAL and confirmed status, including later observations.
Missing, stale, or inconsistent authority yields `UNKNOWN`. It cannot permit
confirmation or prove success. Keep the retained ID fixed across reconnects.
A different retained ID still yields `MISMATCH`. Numeric zero is known, not
unavailable. Factory metadata starts at the approved sequence 1 and the factory
image retains trial ID 0. New acceptance uses its next metadata sequence, with
no wrap; a newly accepted upload cannot have ID 0. Zero therefore stays
`UNKNOWN` in this fresh-upload workflow, even if an unrelated existing image
is already confirmed. This does not make zero an unavailable wire field.
Silence or malformed status yields `UNKNOWN`. READY is not TRIAL.
`confirm()` is explicit. It requires a matching healthy TRIAL. A lost reply can
be followed by the same explicit confirmation request or by status polling.
A confirmed duplicate does not need a renewed health gate. Status is unsigned
bench evidence. It is not server identity or production security.

For an existing confirmed image (including factory ID 0), use the separate
read-only `ConfirmedIdentity(transport, artifact, trial_id)` entry. Supply an
`Artifact.from_bytes(package)` and the exact retained integer uint32 ID from
the prior operation/provisioning, not an ID inferred from the first response.
`poll(attempts)` retries status resolution; explicit `confirm()` also returns
`CONFIRMED` for matching durable status without renewed health. It sends no
STOR and cannot authorize an unconfirmed trial or upload firmware. A lost read
reply is `UNKNOWN`; retry the same object/identity. Exact running physical slot,
payload hash, numeric release and retained ID must match. Missing identity or
sequence is `UNKNOWN`; a known identity conflict is `MISMATCH`. Observed sequence
must cover the retained ID. Success requires the running slot be confirmed,
with no candidate or active trial. Health may be unavailable or zero. This is
idempotent no-write resolution, not proof that a new upload succeeded. The
fresh-upload acceptance predicate and upload admission above remain unchanged.

## Build and validation

The standalone `tests/boot` project registers `boot_service` and
`boot_pc_workflow`. With the separate receiver entries it discovers 34 CTest entries. The new C test uses real
snapshot, publication, lifecycle, and store components with the existing flash
model. It injects 416 callback cuts across both slots, both source pages, and
before/after effects. It checks old/new authority without automatic replay.
The Python suite uses scripted transport calls. It performs no network IO.
Test execution belongs to test-merit-gate. The reconciled frozen target,
regression intent, commands and complete tracked/untracked file manifest are
recorded in `/tmp/ftp-pc-frozen-handoff.md`. Historical execution counts and
handoffs do not establish behavioral acceptance of this target. The added
regressions distinguish unresolved fresh-upload zero from existing confirmed
zero, exercise read retry, wrong/missing identity and no-STOR resolution, and
check service duplicate zero requests against unchanged encoded metadata and
flash with no erase/program, including retries without health.

## Separate receiver and remaining production integration

The [HOST firmware receiver](boot-receive.md) now implements the separate
manifest-first, bounded flash transaction through real stored verification and
durable READY readback. It is not registered with this service or FTP transport.
Its behavioral execution remains deferred to test-merit-gate.

Next bind bounded storage work, transport EOF/abort, backpressure, and foreground
scheduling. Measure buffers, service gaps, watchdog progress, and target resources.
Do not map asynchronous flash work onto the synchronous RAM storage callbacks.
Production flash/FTP wiring, reset scheduling, application startup deadline,
physical timing, final link/32 KiB fit, and hardware acceptance remain open.
