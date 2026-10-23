# Firmware update and PC workflow

Authenticate and admit before invalidating the inactive destination. Publish success only after stored verification and durable READY readback. A final FTP reply is not proof that the new image ran or became confirmed. Keep firmware acceptance, manual reset, running identity and durable confirmation distinct.

## Find the contract

- [Decision and dispatch images](#decision-and-dispatch-images)
- [Flash adapter](#flash-adapter)
- [Application status and confirmation](#application-status-and-confirmation)
- [Firmware receiver](#firmware-receiver)
- [PC demo and transport](#pc-demo-and-transport)


## Decision and dispatch images

For the bench workflow, `scripts/build_boot_dispatch` builds only
`lm3s6965_boot_dispatch` and prints the separate `scripts/flash.sh` command.
It does not build or sign the A/B pair. `scripts/build_ab_boot_demo` configures
the `ab-boot-demo` preset with explicit PUBLIC identity and
`LM3S6965_FTP_DIAGNOSTIC=OFF`, builds the two FTP application targets and invokes
the existing packager for their exact per-slot binaries. It publishes a fresh
non-overwriting `build/ab-demo-packages/release-<A>-<B>/` pair only after both
packages succeed. Direct preset builds do not sign packages or build dispatch.
The wrapper consumes bounded literal local configuration, not shell source.
Signing requires separately authorized private-key access and explicit releases.

```sh
scripts/build_boot_dispatch
scripts/build_ab_boot_demo
```

These are build/signing procedures, not instructions to execute during a
key-free review. Neither establishes blank-board provisioning or current proof.

Configure in an external build directory (as below), or in the ignored root
`build/` subtree, with an explicitly authorized raw 65-byte
SEC1 uncompressed P-256 **public** root:

```sh
cmake -S . -B /tmp/lm3s6965-boot-reset \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_BOOT_RESET=ON \
  -DLM3S6965_BOOT_PHYSICAL_PRODUCTION=ON \
  -DLM3S6965_BOOT_PUBLIC_KEY_FILE=/absolute/path/to/public.sec1
cmake --build /tmp/lm3s6965-boot-reset --target lm3s6965_boot_reset
```

`LM3S6965_BUILD_BOOT_RESET` defaults OFF. ON requires explicit-root production
selection. It never silently uses demo trust. A known public test fixture is
suitable only for authorized RnD, not deployment-grade provisioning.

### Opt-in checked dispatch image (not hardware accepted)

Use a separate build directory and the same explicitly selected PUBLIC root.
`LM3S6965_BUILD_BOOT_DISPATCH_IMAGE` defaults OFF and is mutually exclusive
with `LM3S6965_BUILD_BOOT_RESET`. The default and decision image still never
execute an application.

```sh
cmake -S . -B /tmp/lm3s6965-boot-dispatch \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_BOOT_RESET=OFF \
  -DLM3S6965_BUILD_BOOT_DISPATCH_IMAGE=ON \
  -DLM3S6965_BOOT_PHYSICAL_PRODUCTION=ON \
  -DLM3S6965_BOOT_PUBLIC_KEY_FILE=/absolute/path/to/public.sec1
cmake --build /tmp/lm3s6965-boot-dispatch --target lm3s6965_boot_dispatch
```

The distinct ELF/map/bin/hex basename is `lm3s6965_boot_dispatch`. It shares
boot linker, verifier/crypto, full-width enum ABI and image checks. The checker
uses `--dispatch` to require target handoff, prepare and terminal/startup closure
and reject application reset/main/constructor leakage. The unchanged 32 KiB
limit still applies. Dispatch-only Release requires supported IPO/LTO across
the executable and reachable source archives, an explicit final `-Os` link and
real definition roots required by the static checks. Decision-image optimization
and crypto configuration remain separate.

The signed selected application must use the existing continuity entry stub
and slot linker ABI, not an arbitrary structurally valid reset vector.

The dispatch entry initializes fresh boot-local target/handoff, observes the
actual masked privileged core, calls `boot_sequence_launch` exactly once and
passes its genuine runtime result and launch output to `boot_startup_prepare`,
then nonreturning `boot_startup_dispatch`. No second producer, selection,
watchdog configuration/feed or ordinary application RAM initialization occurs
in boot. Genuine reset, exclusive boot-only hardware ownership and stable
clock/storage remain mandatory. This is not debugger adoption or chainload.

Checkpoints 1..7 retain their old meanings. Dispatch adds 8 handoff-init failure,
9 observation failure, 10 launch failure, 11 checked launch ready, 12 prepare
failure, 13 prepared for dispatch. Sequence result is valid at 10..13.
`boot_entry_launch` only at 11..13. `boot_entry_decision` remains valid **only**
at decision state 7. State 13 is not proof of application entry or C_READY.
final dispatch validation may still fail (retained startup FAILED, no-feed stop).
Early failures also stop without feed, but do not prove WDT was configured.
State checkpoints describe debugger-visible software progress, not physical
acceptance. Validate the final linked dispatch consumer separately.

### Decision image contract

The target uses only `linker/bootloader.ld`, a target-local
`--entry=Boot_Reset_Handler`, real verifier/Mbed TLS and bounded boot-local
newlib allocation support. It does not inherit BSP/application startup or
application linker flags. No constructors or `main` run. Reset masks interrupts
as its first instruction, initializes ordinary data/BSS only, and preserves
retained continuity/diagnostics until the shared sequence intentionally begins.
All exceptions and IRQ0..42 stop without feed. Reserved core vectors are zero.
reserved IRQ positions remain terminal as in `src/startup.c`.

Clock setup assumes genuine reset, exclusive privileged masked ownership and
the EK-LM3S6965's **8 MHz crystal**. It uses the existing 200/4 = nominal **50 MHz**
recipe and bounded delays/PLL polling. Local datasheet (July 15, 2014),
pp.184–185, 200–202 documents RCC2 authority and BYPASS2: bypass the active PLL
before disabling the divider, prepare RCC's raw enabled MAIN source, then clear
USERCC2. RCC profile/RCC2/lock readback and USECRL=49 readback must succeed before
flash initialization. Failure is terminal. There is no claimed fallback Hz.
Register checks establish configuration, not measured frequency accuracy.

Boot-local storage is disjoint/stable through stop. The sole shared sequence
owns retained producer/WDT, fresh reset capture, accounting and real verification.
It is never retried and there is no application dispatch or reset actuator.
Debugger-visible `boot_entry_state` publishes last: 1 starting, 2 clock failure,
3 flash failure, 4 manifest-read failure, 5 bind failure, 6 sequence failure,
7 decision. `boot_entry_flash_result` is valid at 3/4.
`boot_entry_sequence_result` at 6/7 (nested validity rules still apply), and
`boot_entry_decision` only at 7. The original WDT may expire after terminal stop.

The map is `<build>/src/lm3s6965_boot_reset.map`. A successful link runs
`scripts/check-boot-reset-image.py` to enforce exactly 59 mapped vectors, Thumb
ELF/reset entry, first `cpsid i`, load/RAM bounds and real dependency closure.
The linker includes initialized-data LMA in the unchanged 32 KiB flash limit.
The compact crypto configuration uses `MBEDTLS_ECP_FIXED_POINT_OPTIM 0` and
`MBEDTLS_SHA256_SMALLER`. This trades speed for code size without changing
P-256 or SHA-256. A successful static link is not a watchdog timing result.


## Flash adapter

`include/lm3s6965/boot_flash.h` and `src/boot_flash.c` implement an opt-in,
synchronous controller adapter. **There is no physical MMIO binding, startup
integration, hardware authorization or physical timeout guarantee.**
`boot_store.c` and its callback ABI are unchanged.

### API and ownership

1. Supply fallible `read_register`, `write_register` and `read_flash` callbacks
   through `boot_flash_io_t`. Zero means truthful complete success. Failure may
   still have effects. Flash reads return bytes, not fabricated data on a bus
   fault. Failed reads may have modified their output buffer.
2. Initialize `boot_flash_t` once before publication with caller-established,
   stable **actual integral system MHz (1–50)** and a nonzero poll limit. The
   adapter does not infer the clock from USECRL/reset and does not configure it.
   Caller must have configured USECRL to actual MHz minus one and keep clocks
   stable throughout operations. Each modification checks its low eight bits.
3. Use `boot_flash_program` (one aligned LE numeric word), `boot_flash_erase`
   (one aligned 1024-byte page) or `boot_flash_read`. All requests must be
   nonempty, overflow-safe and wholly within `[0x08000,0x3f800)`. Boot and spare
   regions are excluded. These raw primitives do **not** grant lifecycle
   authority: caller must authorize the particular page/word.
4. For metadata, obtain `boot_flash_store_adapter(&flash)` and pass it to the
   unchanged `boot_store_load` / `boot_store_replace`. All three callbacks are
   restricted to `[0x08000,0x08800)`. Erases accept only the two page bases.
   Exclusive ownership must span the **whole store call**, not just callbacks.
5. After a blocked operation, use `boot_flash_recover` only to observe idle,
   clear status and verify status clear. Then reload/reconcile authoritative
   metadata and image state before authorizing another destructive operation.
   Never reinitialize the object, retry cached authority or treat recovery as
   proof that the prior command had no durable effect.

Caller owns controller, adapter, callbacks/context, both metadata pages and
images exclusively across transactions. No ISR calls, independent controller
writers, callback/argument mutation, or overlapping objects. Atomic entry flags
reject concurrent/reentrant operations on the same flash object or erase plan.
they do not establish transaction-level ownership or guard independent objects.
Init/erase-begin require fresh unpublished objects, not live concurrent calls.
The implementation allocates no heap and keeps no persistent RAM state.

### Controller sequence and failure boundary

Before a modification the adapter observes **all four** FMC command bits idle,
checks USECRL and, for programming, reads the old word to reject any 0→1 bit.
It clears both stale raw status sources by writing **3 to FCMISC**, then reads
FCRIS to verify clear. It writes FMD (program only), then FMA, then exactly
`0xa4420001` (WRITE) or `0xa4420002` (ERASE) to FMC. It never writes FCRIS,
FCIM, USECRL, protection/debug registers, COMT or MERASE. It never attempts
cancellation by writing zero.

Completion requires FMC idle within the poll count, PRIS=2 set, ARIS=1 clear,
and complete exact word/page readback. Stale PRIS cannot substitute for a fresh
completion. The last permitted poll can succeed. Both protection and missing
completion are errors, even if FMC cleared. A successful modification leaves
its status for inspection. The next modification clears it before launch.

The adapter marks itself blocked **before invoking the FMC write callback**.
Any failure from there onward leaves it blocked, including callback errors,
timeout, status errors or corrupted readback. An initially busy or unreadable
FMC also blocks ordinary operations. Blocked ordinary reads/writes/erases
issue no callbacks. Explicit recovery can poll a still-running command, but
cannot submit a new one. Pre-command callback failures submit no command.
This conservative quarantine is stronger than merely testing an issued busy
bit on the next request.

A poll budget bounds callback invocations, not elapsed time. The datasheet
states that flash instruction/literal fetches stall during flash operations.
this code does not relocate execution to SRAM. Mock completion tests and Arm
archive compilation do not establish physical timing, watchdog/network service
margin, final boot fit or hardware readiness.

### Durable authority and inactive-slot erase

`boot_store_replace` still preserves the authoritative old page, writes the
alternate page, verifies staging, programs commit **last**, and verifies/selects
the committed result. From commit callback invocation onward, every failure
remains `BOOT_STORE_UNCERTAIN`, even if the adapter failed before launching the
commit command. Recovering the controller does not resolve that ambiguity.
`boot_store_load` must establish old/new/recovery authority.

`boot_flash_erase_begin` creates a fresh plan from an explicit running slot,
destination and invalidated store snapshot. Before calling it, the caller must
have authenticated/admitted the incoming manifest and actually-running image,
and durably invalidated the inactive destination with `BOOT_STORE_OK` (or
reloaded and reconciled equivalent authority). A lifecycle RAM proposal is
**not** permission. Structural checks enforce codec validity, running=current
confirmed, no candidate, and destination canonical empty with no
role/confirmation/intent. C structs cannot prove signature provenance,
durability or current authority. Those remain explicit caller preconditions.

`boot_flash_erase_step` erases/verifies **one page per call**: manifest reserve
first, then the 109 payload pages in ascending order. `OK` advances one page.
`DONE` issues no command. Every admitted step error permanently stops that plan
without advancing. Pre-entry argument rejection or `BUSY` reentry leaves the
active plan unchanged. A fresh authorized restart must again start at the manifest. Caller
must keep authority/images stable across the whole upload and service its own
foreground work between steps. Neither function reads or writes durable
metadata, authenticates a manifest, programs payloads, accepts READY, or
composes FTP/watchdog/handoff policy.

### Local source evidence

Verified directly from the repository's `datasheets/LM3S6965 Microcontroller
Data Sheet.pdf`, printed/PDF pages **261–275** (July 15, 2014):

| Pages | Contract used |
| --- | --- |
| 261, 263–264 | 1 KiB page, 32-bit 1→0 programming. USECRL responsibility. Fetch stalls. Data/address/key ordering |
| 265–268 | Flash control base `0x400fd000`, FMA/FMD offsets 0/4. 18-bit FMA and software alignment |
| 269–270 | FMC +8, key `0xa442`, exactly one command bit, WRITE=1/ERASE=2, completion clears busy, zero cannot cancel |
| 271–273 | FCRIS +12 RO, PRIS=2/ARIS=1. FCMISC +20 W1C both raw sources, independent of interrupt masking |
| 275 | USECRL `0x400fe140`, low byte actual MHz−1, reset 49 is not clock evidence |

The prose protection-bit polarity on p262 is internally inconsistent with the
p261 table. This adapter makes no inference from those protection registers.
It checks the unambiguous ARIS bit definition on p271 and requires readback.


## Application status and confirmation

`include/lm3s6965/boot_service.h` defines a separate HOST service API.
It does not register FTP commands. It does not change `ftp_storage_ops_t`.
The existing synchronous RAM backend stays unchanged.

### Status

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

### Confirmation

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

### PC module

The [PC FTP client reference](#pc-demo-and-transport) describes the concrete bounded stdlib
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
no wrap. A newly accepted upload cannot have ID 0. Zero therefore stays
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
`poll(attempts)` retries status resolution. Explicit `confirm()` also returns
`CONFIRMED` for matching durable status without renewed health. It sends no
STOR and cannot authorize an unconfirmed trial or upload firmware. A lost read
reply is `UNKNOWN`. Retry the same object/identity. Exact running physical slot,
payload hash, numeric release and retained ID must match. Missing identity or
sequence is `UNKNOWN`. A known identity conflict is `MISMATCH`. Observed sequence
must cover the retained ID. Success requires the running slot be confirmed,
with no candidate or active trial. Health may be unavailable or zero. This is
idempotent no-write resolution, not proof that a new upload succeeded. The
fresh-upload acceptance predicate and upload admission above remain unchanged.


## Firmware receiver

Use `include/lm3s6965/boot_receive.h` for a separate firmware transaction.
This API does not implement FTP, start a listener, or replace the synchronous
RAM `ftp_storage_ops_t` backend. It composes the real verifier, lifecycle,
commit-last store, and injected flash controller. Reset is an output intent only.
Validate receiver policy with native models and the final consumer separately.
A build result is not behavioral or hardware acceptance.

### Ownership and admission

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

### Bounded input and work

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
between steps. This does not prove network, watchdog, SRAM, or physical timing
margins. The receiver buffers 160 manifest bytes and one payload word, not the
whole image. Crypto and store still have their existing scratch/heap needs.

### Failure and continuation rules

All admitted failures are terminal. `failed_phase`, `failure`, and subordinate
store/flash/verifier/lifecycle results retain the first stop. No later phase or
cleanup IO runs. Abort/disconnect calls `boot_receive_abort`. It stops without
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
readback can leave durable READY. It is an uncertain acceptance, not a rollback.
Current image bytes, identity, confirmation, and retained history are preserved.
There are no per-handoff writes or reset-accounting calls.

### Watchdog continuity

The receiver deliberately takes no watchdog pointer. It cannot copy, initialize,
transfer, report to, or feed one. Continue the same APPLICATION watchdog object,
epoch, and lifetime verification history outside these calls. Report only real
foreground Ethernet, timer, and FTP progress. Verifier observer events permit
cancellation. They cannot stand for application progress. Do not map them to
blind feeds. The regression fixture retains a real transferred APPLICATION
watchdog, existing history, and partial genuine evidence through reception.
Opaque crypto/store/flash gaps remain unmeasured.


## PC demo and transport

### Start with the two-command demo client

From the repository root, after separate approval for device and network access:

```sh
scripts/ab-boot-firmwareflash-ftp.sh status
scripts/ab-boot-firmwareflash-ftp.sh download /path/to/signed-ftp-packages
```

This is an **FTP-only** client. It defaults to
`192.168.7.2`, control port 21. `--host` overrides the address. Use `--help` for
optional port and diagnostic controls. The script neither invokes OpenOCD nor
resets the MCU. The exact old physical programmer is retained separately as
`scripts/ab-boot-provision-recovery.sh`. It writes ONE SLOT only, not complete
blank-board provisioning. Bootloader, public root and metadata remain unchanged.
It is not a recovery fallback here. A new signing key requires a matching device
bootloader root. FTP cannot rotate the existing root.

The first production firmware must already serve `boot-status`, be durably
confirmed, and report a permitted update slot. Provisioning, authenticated boot
dispatch and initial confirmation are prerequisites, not actions of this client.
A decision-only boot image or an LED-only application cannot provide them.

#### Prepare the directory

Supply a directory, not a package path or archive. Only top-level `*.pkg` files
are scanned. There must be exactly one signed A package and one signed B package.
The manifest identifies the slot. Filenames do not. Missing, extra, duplicate
or malformed packages stop the workflow. Nothing is recursively collected,
packed into an archive, signed or generated by the client.

Both packages must contain FTP-enabled production applications. LED-only
packages do **not** serve FTP and are unsuitable. Keep
A breathing and B double-blinking so the slots remain visibly distinct. See
[FTP variant build requirements](../examples/ab-boot-demo/README.md#choose-ftp-enabled-packages-not-the-led-only-fixtures).
Build and sign those variants only under separate authority with the existing
approved signing identity. Do not create replacement keys for this workflow.

#### Follow the download prompts

1. The host checks both packages locally and reads status. The MCU's permitted
   slot selects the package. The host does not choose an arbitrary slot.
2. The host makes one `STOR firmware` attempt, then shows status after transfer.
   It prompts you to reset the board manually. There is no remote reset command.
3. After that reset, the host polls until the expected application is healthy.
   It retrieves `boot-report` and checks slot, payload hash, release, trial
   identity and health against status and the selected package.
4. Only a matching healthy trial proceeds to the existing `STOR confirm`.
   The host polls again for matching durable confirmed status. A transfer's
   final 226 reply is not an update-success result.

The application prepares its startup report before its foreground loop. It
serves it when the host polls with `RETR boot-report`, while networking remains
serviced. This is not an unsolicited startup packet. The report has a separate
version prefix. `boot-status` retains its unchanged `status1` format. A report
alone does not prove durable confirmation.

#### Stop safely on errors

Invalid directories, package admission failures, unavailable permitted slots,
identity conflicts and unhealthy reports do not authorize confirmation. A lost
firmware completion reply is ambiguous, not permission to retry. **Do not rerun
`download` as a firmware retry after an ambiguous transfer.** Inspect read-only
status and retain the exact package and known trial identity for diagnosis.
There is no automatic firmware retry or saved resumable upload session.

Ctrl-C stops further host actions. It sends no additional firmware,
confirmation or reset after cancellation. It cannot roll back a mutation the
MCU already accepted. Losing a confirmation reply also requires durable-status
resolution rather than assuming success.

### Lower-level client and transport reference

`python3 tools/boot/client.py --help` describes the lower-level
`status`/`update`/`resolve` API below. Its explicit package arguments and optional
`--confirm` are not the two-command wrapper interface. The signer
`tools/boot/package.py` stays offline. Neither client signs images.

### Boundary

This is plain FTP for an isolated, trusted bench. Status and confirmation are
unsigned. Neither passwords nor local package checks authenticate the server.
Local `Artifact` checks verify framing, bounds, hash, and vectors, **not ECDSA**.
The device must authenticate firmware before mutation. Production startup,
reset scheduling, watchdog timing, physical flash durability, and final boot
link/32 KiB fit remain separate acceptance work. The standalone RAM-backed
Ethernet example does not become a production updater merely by running this
client. The opt-in A/B FTP variants require their production boot binding.
This documentation claims no new hardware run or full static proof.

Use only after separate approval for device and network access. These examples
are usage instructions, not evidence of a live run:

```sh
python3 tools/boot/client.py --host 192.0.2.2 status
python3 tools/boot/client.py --host 192.0.2.2 update \
  --slot-a release-a.pkg --slot-b release-b.pkg --polls 60
python3 tools/boot/client.py --host 192.0.2.2 update \
  --slot-a release-a.pkg --slot-b release-b.pkg --polls 60 --confirm
python3 tools/boot/client.py --host 192.0.2.2 resolve \
  --package release-b.pkg --trial-id 21 --polls 60
```

`update` validates both bounded package files before it opens FTP. It reads
permission and selects the physical slot artifact. It makes **one** firmware
attempt. It polls with one second between reads, up to `--polls` (1..120).
`--confirm` is allowed only on `update`. It acts on a healthy matching trial
within that same live workflow object. It rereads status before confirmation.
It then polls again, with the same bound, to resolve durable confirmation.
A lost firmware or confirmation completion reply is not success. No automatic
firmware retry occurs. Polling preserves the pre-upload sequence and pinned
trial ID across reconnects. Fresh acceptance still requires
`pre_upload_sequence < trial_id <= observed_sequence`.

Do not rerun `update` as a retry for a lost reply. There is no saved upload
session or resumable fresh-upload state. `resolve` is read-only: supply a
previously known exact package and retained trial ID. It cannot authorize a
trial or send either STOR. Factory retained ID zero is valid. The existing
resolver's defensive sequence-zero/retained-zero limit remains unchanged:
a matching confirmed status with sequence zero can still resolve. The factory
contract starts sequence at one. Do not infer a retained ID from an arbitrary
first response and call it upload provenance.

### Endpoint and credentials

Only numeric IPv4 is accepted. DNS and IPv6 are not supported. Both control
and passive data connections construct AF_INET/SOCK_STREAM sockets directly.
neither uses create_connection or getaddrinfo. The remaining budget is set
immediately before connect. Port defaults to 21.
`--user` or `BOOT_FTP_USER` sets the username. The existing server accepts
`USER anonymous` and a PASS body, so defaults are `anonymous` and `bench@`.
These defaults provide no access control. Set `BOOT_FTP_PASSWORD` from a secure
shell environment, or use `--prompt-password` for a non-echoed prompt. Do not
put a password in command-line arguments. There is no password argument.
Credentials must be nonempty printable ASCII, at most 256 bytes. They are
sent literally, including password `-` for `anonymous` (no ftplib rewrite).
Control characters are rejected before the FTP factory runs. Server error text,
passwords, and filesystem paths are not printed on failure.

### Deadlines and transport

`ftp_transport.FTPTransport` uses stdlib `ftplib`. It accepts an injected FTP
factory and monotonic clock. The factory supplies `arm(remaining)` in addition
to the used ftplib methods. The default `DeadlineFTP` enforces the budget
inside control reads and partial writes. It does not use buffered control
`readline`, which could restart a timeout on each read indefinitely.

Defaults: each blocking call is limited to **5 seconds**, and each connect,
binary selection, RETR, or STOR operation has a **75 second total deadline**.
The CLI accepts integer overrides of 1..120 seconds. A monotonic remaining
budget is recomputed before/after data chunks and control steps, for each
control byte/write, and after PASV before the numeric data connection. No
worker threads or timeout retries are used. Bounds apply to network work,
not local filesystem operations, OS scheduling pauses, or password prompts.
Each reconnect discards and closes the old connection, then requires greeting
220, USER reply 331, PASS reply 230, selects passive mode, and requires TYPE I
reply 200. Preliminary greeting 120, early USER success, and ACCT are not
supported. They fail closed rather than queue replies for the next command. Each transfer selects
TYPE I again. Passive data addresses use ftplib's default control-peer policy.
the client does not opt into trusting a foreign PASV address.

Status reads preserve exact binary bytes, read at most 1025 bytes, and reject
byte 1025 without draining an unbounded body. Firmware is 396..111776 bytes.
confirmation is 1..128 bytes. The workflow produces the exact package and
strict confirmation body. The transport does not translate or append bytes.
Writes handle partial sends. Data EOF/close is followed by the final control
reply, which must be exactly 226 (not 200, 221, or ABOR's 225). PASV requires
227 with six decimal octets in 0..255 and a nonzero port. Transfer preliminary
reply must be 150, not arbitrary 1xx or an initial 2xx. Active mode, REST, and
125 are not supported by this restricted adapter. Server numeric conversion
failures are normalized at the parser boundary. The data socket remains
explicitly owned through preliminary parsing and closes on parsing failure.
Protocol errors, socket errors, lost EOF/replies, or deadline expiry raise a
safe workflow `OSError` and discard the control stream. Cleanup does
not hide the original error. There is no ABOR or risky reuse of a failed stream.

Control replies (including errors) are limited to **65536 aggregate wire
bytes** and **256 lines** per reply, inclusive of opening/terminating codes,
separators, content, and every CR/LF. Each line remains limited to **8192 wire
bytes**, including LF or CRLF. The first byte beyond either byte limit closes
the control socket immediately, before retaining that byte. A required 257th
line closes it before reading that line. These conservative limits accommodate
this restricted service without retaining an unbounded multiline transcript.
Bounded lines are collected and joined once, not repeatedly concatenated.
All reads still share the operation's total deadline. Progress does not reset it.

RFC 959 multiline replies are supported: an ASCII three-digit opening code
followed by `-` ends only at the **same code followed by SPACE**. Matching-code
lines with `-` continue. Bare matching codes and other matching-code separators
are protocol errors, not successful terminators. Foreign-code and arbitrary
interior lines remain content, not standalone replies or terminators. A valid
single-line reply requires a three-digit code and SPACE. The expected command
state is checked after complete framing. Malformed/oversized replies close
control and any owned data socket and produce only the redacted `OSError`.

### Results

| Output | Exit | Meaning |
| --- | --- | --- |
| Status body | 0 | A parsed status was read, not update success |
| `CONFIRMED` | 0 | Matching durable confirmed status was observed |
| Error on stderr | 2 | Invalid arguments, package, admission, or status |
| `TRIAL` | 3 | Matching healthy trial, not confirmed success |
| `UNKNOWN` | 4 | No outcome established. Includes transport/filesystem errors |
| `MISMATCH` | 5 | Known identity conflict |
| `UNHEALTHY` | 6 | Matching trial without all required health bits |

No success message is printed for UNKNOWN, MISMATCH, or UNHEALTHY. A final FTP
226 alone is not reported as update success. Poll bounds count operations,
not seconds: default worst-case polling includes up to 60 connect/read pairs
plus 59 one-second sleeps. Confirmation can add a second bounded poll phase.
