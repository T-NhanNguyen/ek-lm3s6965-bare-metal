# Boot diagnostics, ordered publication and coherent snapshot foundation

The pure codec implements ordinary-memory serialization and evidence helpers
in `include/lm3s6965/boot_diag.h` and `src/boot_diag.c`. The separately gated
ordered publication API is in `include/lm3s6965/boot_diag_publication.h` and
`src/boot_diag_publication.c`. The authoritative design
is `.dev-vault/boot-mvp-spec.md`, Diagnostic schema 1 and coherent publication,
and the latest closeout supplement in `.dev-vault/design/boot-diagnostics.md`.
The reviewed increments provide an opt-in reserved-SRAM word writer and a
read-only coherent snapshot foundation in `include/lm3s6965/boot_snapshot.h`
and `src/boot_snapshot.c`. They do not provide production startup integration,
health/LED control, diagnostic source attribution, FTP status rendering or durable
event history. The next bounded increment is a target FLASH adapter tested through
mocked registers before actual hardware, then watchdog, handoff and boot composition.
Full MVP, integrated network resources/latency and final boot fit remain unaccepted.

## Wire contract

Exactly 64 bytes, sixteen explicitly little-endian uint32 words, intended address
range `[0x2000DFC0,0x2000E000)`. Existing `boot_format.h` supplies SRAM geometry
and A/B/unavailable slot constants; it is unchanged. No C struct is copied to
or from the wire. The fixed 64-byte prefix is inspected/written; larger caller
spans are permitted, and their tails are ignored/unchanged.

| Offset | Word |
| ---: | --- |
| 0 | Magic `ABD1`, bytes `41 42 44 31`, LE `0x31444241` |
| 4 | Schema 1 |
| 8 | Length 64 bytes |
| 12 | Validity: 0 staging/unpublished, 1 published |
| 16 | Stage: reset 0, metadata 1, validation 2, handoff/startup 3, trial 4, recovery 5 |
| 20 | Latched fault: none 0, metadata 1, validation 2, startup 3, trial 4, terminal recovery 5 |
| 24 | Global detail 0..20 (below) |
| 28 | Raw captured reset flags, all 32 bits preserved |
| 32 | Running slot: A 0, B 1, unavailable `0xFFFFFFFF` |
| 36 | Selected slot: same encoding |
| 40 | Metadata sequence: full uint32 domain |
| 44 | Applicable trial ID: full uint32 domain |
| 48 | Counted failures: 0..3 |
| 52 | Health: app init bit 0, Ethernet init bit 1, FTP listening bit 2; mask 7 |
| 56 | Failed slot: same slot encoding |
| 60 | Reserved: zero |

Global detail is: none 0; no valid metadata 1; conflicting metadata 2; CRC 3;
sequence exhaustion 4; manifest format/device 5; target/link 6; signature 7;
hash 8; payload bounds 9; vectors/MSP 10; startup unknown 11; app init 12;
Ethernet init 13; FTP init 14; WDT 15; EXT 16; BOR 17; deadline 18; combined 19;
no recovery 20. It is not fault-scoped numbering. The codec deliberately imposes
no extra stage/fault/detail pair mapping or slot relationship restrictions.
Fault 0 does not prove health and has no LED error pattern. Fault precedence,
retaining underlying terminal detail and successful-fallback latching remain
future caller obligations, not transformations performed by this codec.

## APIs and evidence availability

- `boot_diag_validate` checks the in-memory enum/slot/count/health spaces.
- `boot_diag_encode(record, validity, wire, capacity)` validates before any
  writes and emits canonical header/reserved fields. Both staging and published
  fixtures can be encoded; this is **not** a live publication operation.
- `boot_diag_decode(wire, size, required_validity, out)` validates the header,
  exact requested validity, reserved word and fields before publishing output.
  Use `BOOT_DIAG_PUBLISHED` for readers; `BOOT_DIAG_STAGING` is an explicit
  offline inspection mode and rejects published records. Unknown validity
  values are never accepted. Unsupported schema is rejected before interpreting
  version-dependent fields.
- `boot_diag_init(captured_flags, out, context)` initializes an offline boot
  record: stage reset, no detected fault/detail, no app health bits, all slots
  unavailable, raw flags unchanged. It guesses no metadata roles or history.
  Sequence/trial/count zeros are placeholders, with context explicitly marking
  all three unavailable. Reset/stage/health are known because this caller has
  supplied a captured snapshot and is initializing a fresh boot fixture.
- `boot_diag_slot`, `boot_diag_sequence`, `boot_diag_trial_id`, and
  `boot_diag_failures` return typed `{available, value}` results (slot uses
  `{available, slot}`). An unavailable result's zero value is not evidence.
  Null numeric-helper inputs yield unavailable; invalid slot/count inputs also
  yield unavailable. Callers should validate records first.

All writable outputs are unchanged on errors. Inputs/outputs must not overlap,
as with `boot_format.h`; aliasing is a caller precondition, not runtime detection.
All spans must actually cover their declared size. Unaligned wire pointers are
supported; no native word accesses occur.

**There is no approved numeric unknown sentinel for sequence/trial/count.**
`UINT32_MAX` is a legitimate sequence and trial ID, and known zero is distinct
from absent evidence. The caller supplies `boot_diag_context_t`, which is NOT a
wire field, flag or reserved-bit extension. It tracks availability of
sequence/trial/count and of stage/reset/health. No availability is inferred from
wire zero/max, fault code, stage, selected/running/failed slot or metadata roles.
Availability cannot be recovered by decoding the 64 bytes alone. Future status
or halted-SWD glue needs trustworthy, same-snapshot external evidence/context;
without it, report unavailable rather than fabricate a value. This increment
neither defines that glue nor changes the format to solve it.

## Publication and integration limits

### Ordered foreground publication API

One zero-initialized `boot_diag_publication_t` owns an aligned 16-word SRAM
port, external availability context and nonreentry guard. On target, set `words`
to `(volatile uint32_t *)BOOT_DIAGNOSTIC_BASE` and leave `word`/`barrier`/`user`
NULL. This is caller-owned ordinary SRAM, **not fallible MMIO**. The whole owner
and port must remain exclusively owned. All declared spans must actually exist;
owner/input/output overlap with the port is rejected before access. No C wire
struct or untrusted byte-buffer alias is used for target stores.

| API | Contract |
| --- | --- |
| `boot_diag_publication_boot(owner, raw_flags)` | Explicitly overwrites any old valid or garbage block via reviewed `boot_diag_init`; preserves every raw capture bit. Numeric availability initially false; stage/reset/health known. |
| `boot_diag_publication_adopt(owner, context, trusted)` | Validates published 64 bytes, writes **no SRAM words**, associates exact caller context. Requires trustworthy same-record associated metadata. No initialize/reset/reconstruction or inference from zero/max/header. `trusted=false` rejects before reads. The bool is a provenance assertion, not authentication. |
| `boot_diag_publication_progress(owner, stage, health)` | Reads/validates existing published record under the same owner, changes **only stage and health**. Preserves fault, detail, raw reset, failed/running/selected slot, sequence, trial ID, count and **all availability**, even unknown stage/health with a correct header. Metadata/slot refresh is not implemented. |
| `boot_diag_publication_snapshot(owner, record, context)` | Serialized foreground-only decoded copy plus associated sidecar, outputs unchanged on failure. Not a final status snapshot with metadata/identity/accounting. |
| `boot_diag_publication_capture(owner, callback, context)` | Calls the read-only extension once with temporary owned diagnostic/context values while the same owner is busy and interrupts remain masked. Callback false returns CAPTURE. Pointers must not escape. |

Complete candidate is byte-encoded and validated locally, converted explicitly
from LE to a local aligned word array, then written: validity **0 first** →
DMB/compiler memory clobber → other **15 aligned uint32 stores** → sidecar update
in the same owner scope → DMB/compiler clobber → validity **1 last**. The native
store is volatile uint32, not byte stores into reserved SRAM. On Cortex-M3 all
operations reject nonzero **IPSR** before owner/port mutations, save PRIMASK then
`cpsid i`, and restore the exact caller PRIMASK (0 or 1) at every exit. Privileged
Thread mode is required; no BASEPRI/FreeRTOS primitive is imported. **PRIMASK does
not mask NMI or HardFault**: they must never write diagnostics or this owner.
No ISR writers, no alternate owners, no foreground task interleaving across
calls; `busy` rejects callback reentry, not arbitrary multithread races.

Host-only bounded `word(user,index,write,value)` callbacks model exactly one
word (index 0..15); barrier hook traces ordering. Target rejects non-NULL hooks.
Callbacks must be truthful, bounded, serialized and cannot mutate other words.
False on read returns READ before publication; false on any store returns
**UNCERTAIN**, including false after an effective final validity-1 store. The
owner then disassociates availability and snapshot refuses to claim publication.
After successful validity-0, every interrupted pre-final native store leaves
validity 0; first-store failure can leave old/garbage RAM, and final-store failure
can leave a fully published new block. Do not assume the old state, retry
implicitly, or reuse a cached sidecar. Reconcile and explicitly adopt only with
trustworthy same-record context; an authorized boot initialization is a different
operation. Infallible native SRAM has no callback failure result. Power/reset
interruption is not a returned C error or a retained-owner guarantee.

### Coherence boundary

Validity double-check **1→1 is not whole-block atomicity** (ABA). Coherence here
comes only from serialized foreground readers and writers using this owner.
External SWD requires a separately authorized halt, then header/field/published
validation; it must not infer availability from raw SRAM, even all-zero numeric
fields or maximum sequence/trial. Do not replay old boot context on app adoption.
No live debugger/FTP atomic-read promise or full FTP status is provided.

### Read-only coherent snapshot foundation

`boot_snapshot_capture(owner, sources, out)` copies decoded diagnostics and their
external availability context, authoritative metadata (including accounting), and
actually-running authenticated stored-image evidence under **the same publication
owner session**, before unlock. Success publishes a fixed-size, caller-owned
`boot_snapshot_t`; no borrowed pointers escape, later source changes do not change
the copy, and all errors leave output unchanged. Capture writes no diagnostic
words and never invokes metadata erase/program callbacks.

The metadata `read` follows `boot_store`'s truthful read/error contract. The
`running` callback returns zero only for the actually executing, previously
authenticated stored image, including vectors, **not** a selected slot or role
inferred from metadata. Capture checks running identity against its resident
metadata entry, image bounds, exact initial MSP and Thumb reset-vector bounds;
it does not perform cryptographic verification. Verification-to-use stability
and provenance remain caller obligations, not authentication of a C struct.

Both callbacks run with the owner busy and target interrupts masked. They must
be bounded, read-only, nonblocking and allocation-free: no crypto, formatting,
erase/program, owner/port/context/source mutation or reentry except attempted
publisher calls, which return BUSY. Callbacks may write only their requested
output spans (host trace counters can be separate local state). Descriptor,
output, owner and port must be disjoint; callback-managed storage must not alias
output/port and may reference but not mutate the owner. All participants must
serialize through this owner and prevent source/image changes throughout capture
and between verification and use. PRIMASK cannot protect against DMA, debugger,
another owner, or dishonest/stale callbacks.

Diagnostics and availability are copied **verbatim**, including raw reset flags
and latched historical fault, failed slot, sequence, trial and count. Coherent
sampling does not mean historical diagnostic numbers/slots equal live metadata,
roles or running identity: a failed image can legitimately precede a running
fallback. The prior global diagnostic-source cross-match requirement was an
orchestrator overrestriction and was removed; no new source-binding layer,
wire schema or event history was implemented. External availability still must
be trustworthy for the diagnostic record itself, not fabricated from live roles.
Current root-status trial/state sourcing, status formatting and full FTP integration
remain separate work.

Running status must capture metadata/identity/accounting/diagnostics together,
not combine fields sampled across transitions. Halted
SWD readers must check header and published validity under separately authorized
operational procedures. Boot/app linker and startup exclusions, retained-SRAM
behavior, stack/heap/resource fit and timing require separate validation. No
linker/startup path is changed here; existing reviewed layouts remain untouched.
No image hash is duplicated, no reset is classified here, and power loss cannot
restore transient history from this codec.

## Reproducible host and archive checks

```sh
cmake -S tests/boot -B build/diag-debug -DCMAKE_BUILD_TYPE=Debug -DBOOT_SANITIZERS=ON
cmake --build build/diag-debug -j6
ctest --test-dir build/diag-debug --output-on-failure
cmake -S tests/boot -B build/diag-release -DCMAKE_BUILD_TYPE=Release -DBOOT_SANITIZERS=ON
cmake --build build/diag-release -j6
ctest --test-dir build/diag-release --output-on-failure
cmake -S tests/boot -B build/diag-arm -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake"
cmake --build build/diag-arm --target boot_diag boot_diag_publication boot_snapshot -j4
```

The reviewed host project registers **18 CTest tests**, including codec,
publication, snapshot and target publication/snapshot gates. Supplied independent
review reports Debug and Release with ASan/UBSan **18/18**, no skips, and **81
QEMU resource cases**, plus `-Os`/`-Og` publication/snapshot executions. These
are recorded review results, not tests rerun by this documentation checkpoint.
`test_snapshot.c` covers historical diagnostic preservation alongside live metadata,
actual-running identity, guarded callbacks, errors and frozen owned outputs.
`test_diag_publication.c` uses independent literal 16-word records and the exact
19-event write/barrier trace; failures before/after every word store, read
failures at all 16 boundaries, boot overwrite of valid/garbage blocks, unknown
context preservation, malformed fields, alias/alignment and reentry rejection.
`test_diag_publication_target.py` links a test-only reset/vector/SVC fixture with
the unchanged boot linker, executes the LIVE64 block at 0x2000DFC0, verifies
boot/adoption/progress/fault/count/reset/availability and PRIMASK 0/1 restoration,
plus snapshot callbacks under masked interrupts and a busy owner. It invokes an
**actual SVC handler** to reject publication and snapshot APIs without modifying
the 64 bytes. Disassembly at `-Os` and Debug `-Og` requires IPSR, PRIMASK save/restore,
CPSID, DMB and native scaled aligned word STR without STRB/STRH. Run it directly:
`python3 tests/boot/test_diag_publication_target.py`; missing tools fail, not skip.
No physical target, production startup change, linker-fence weakening or change
to the approved FTP 8 KiB heap is included.

The prior codec tests remain unchanged. `test_diag.c` includes a
literal independent sixteen-word golden fixture, independent raw decoder
fixtures, all legal stage/fault/detail combinations, all 27 A/B/unavailable slot
combinations, all health masks/counts, zero and max sequence/trial, every raw
flag bit, every malformed reserved/header bit, invalid field values, span sizes
0..63/exact/larger, unaligned spans, null arguments, unchanged error outputs,
context distinction and static staging/update/published fixtures. Existing
packaging tests may use OpenSSL and the authorized external signing key; this
increment adds no private-key handling. An Arm static archive is compile-only
evidence, not final linking, boot fit or hardware acceptance. Heap, ordered
publication and snapshot foundation are reviewed/approved within these bounded
contracts; target FLASH mocked-register gates are next, not hardware acceptance.
