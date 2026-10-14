# Injected flash adapter (mocked-register foundation)

`include/lm3s6965/boot_flash.h` and `src/boot_flash.c` implement an opt-in,
synchronous controller adapter. **There is no physical MMIO binding, startup
integration, hardware authorization or physical timeout guarantee.**
`boot_store.c` and its callback ABI are unchanged.

## API and ownership

1. Supply fallible `read_register`, `write_register` and `read_flash` callbacks
   through `boot_flash_io_t`. Zero means truthful complete success; failure may
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
   restricted to `[0x08000,0x08800)`; erases accept only the two page bases.
   Exclusive ownership must span the **whole store call**, not just callbacks.
5. After a blocked operation, use `boot_flash_recover` only to observe idle,
   clear status and verify status clear. Then reload/reconcile authoritative
   metadata and image state before authorizing another destructive operation.
   Never reinitialize the object, retry cached authority or treat recovery as
   proof that the prior command had no durable effect.

Caller owns controller, adapter, callbacks/context, both metadata pages and
images exclusively across transactions. No ISR calls, independent controller
writers, callback/argument mutation, or overlapping objects. Atomic entry flags
reject concurrent/reentrant operations on the same flash object or erase plan;
they do not establish transaction-level ownership or guard independent objects.
Init/erase-begin require fresh unpublished objects, not live concurrent calls.
The implementation allocates no heap and keeps no persistent RAM state.

## STE: controller sequence and failure boundary

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
its status for inspection; the next modification clears it before launch.

The adapter marks itself blocked **before invoking the FMC write callback**.
Any failure from there onward leaves it blocked, including callback errors,
timeout, status errors or corrupted readback. An initially busy or unreadable
FMC also blocks ordinary operations. Blocked ordinary reads/writes/erases
issue no callbacks. Explicit recovery can poll a still-running command, but
cannot submit a new one. Pre-command callback failures submit no command.
This conservative quarantine is stronger than merely testing an issued busy
bit on the next request.

A poll budget bounds callback invocations, not elapsed time. The datasheet
states that flash instruction/literal fetches stall during flash operations;
this code does not relocate execution to SRAM. Mock completion tests and Arm
archive compilation do not establish physical timing, watchdog/network service
margin, final boot fit or hardware readiness.

## STE: durable authority and inactive-slot erase

`boot_store_replace` still preserves the authoritative old page, writes the
alternate page, verifies staging, programs commit **last**, and verifies/selects
the committed result. From commit callback invocation onward, every failure
remains `BOOT_STORE_UNCERTAIN`, even if the adapter failed before launching the
commit command. Recovering the controller does not resolve that ambiguity;
`boot_store_load` must establish old/new/recovery authority.

`boot_flash_erase_begin` creates a fresh plan from an explicit running slot,
destination and invalidated store snapshot. Before calling it, the caller must
have authenticated/admitted the incoming manifest and actually-running image,
and durably invalidated the inactive destination with `BOOT_STORE_OK` (or
reloaded and reconciled equivalent authority). A lifecycle RAM proposal is
**not** permission. Structural checks enforce codec validity, running=current
confirmed, no candidate, and destination canonical empty with no
role/confirmation/intent. C structs cannot prove signature provenance,
durability or current authority; those remain explicit caller preconditions.

`boot_flash_erase_step` erases/verifies **one page per call**: manifest reserve
first, then the 109 payload pages in ascending order. `OK` advances one page;
`DONE` issues no command. Every admitted step error permanently stops that plan
without advancing; pre-entry argument rejection or `BUSY` reentry leaves the
active plan unchanged. A fresh authorized restart must again start at the manifest. Caller
must keep authority/images stable across the whole upload and service its own
foreground work between steps. Neither function reads or writes durable
metadata, authenticates a manifest, programs payloads, accepts READY, or
composes FTP/watchdog/handoff policy.

## Local source evidence

Verified directly from the repository's `datasheets/LM3S6965 Microcontroller
Data Sheet.pdf`, printed/PDF pages **261–275** (July 15, 2014):

| Pages | Contract used |
| --- | --- |
| 261, 263–264 | 1 KiB page, 32-bit 1→0 programming; USECRL responsibility; fetch stalls; data/address/key ordering |
| 265–268 | Flash control base `0x400fd000`, FMA/FMD offsets 0/4; 18-bit FMA and software alignment |
| 269–270 | FMC +8, key `0xa442`, exactly one command bit, WRITE=1/ERASE=2, completion clears busy, zero cannot cancel |
| 271–273 | FCRIS +12 RO, PRIS=2/ARIS=1; FCMISC +20 W1C both raw sources, independent of interrupt masking |
| 275 | USECRL `0x400fe140`, low byte actual MHz−1, reset 49 is not clock evidence |

The prose protection-bit polarity on p262 is internally inconsistent with the
p261 table; this adapter makes no inference from those protection registers.
It checks the unambiguous ARIS bit definition on p271 and requires readback.

## Build and regression

```sh
cmake -S tests/boot -B /tmp/boot-flash-check -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/boot-flash-check --target test_flash test_store
ctest --test-dir /tmp/boot-flash-check -R '^(boot_flash|boot_store)$' --output-on-failure
```

Host target `boot_flash` / test `boot_flash` use the existing sanitizers and
strict warning policy. The root opt-in archive is `lm3s6965_boot_flash`;
the standalone Arm archive target is `boot_flash`. Neither binds MMIO or links
an integrated boot image. Tests inject failures before/after effects at every
callback boundary for program/erase, both store authority pages and the first
two inactive erase steps. They also check last-poll completion, indefinite
busy/recovery, stale status, ordering/keys, alignment/range/overflow, clock
mismatch, 0→1 rejection, readback corruption, commit-last/UNCERTAIN/reload,
reentry rejection and both complete 110-page erase orders. This is a bounded
mock fault model, not arbitrary silicon/power-loss behavior or an independent
metadata codec oracle.
