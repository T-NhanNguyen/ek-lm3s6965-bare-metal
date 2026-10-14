# Offline signed boot packages (demo trust root only)

`tools/boot/package.py` creates **exactly 160 manifest bytes followed by the exact
canonical payload**. It does not write flash, manufacture metadata, implement
an updater, or establish boot/runtime/hardware acceptance. It uses Python 3.9+
standard library and the existing `openssl` executable; no Python dependencies
or build-time downloads. The fixed trust root remains
`config/boot_demo_public_key.h`; there is no key override option.

## Generate one slot-specific artifact

From the repository root, with an **already slot-linked** application ELF and a
new output path whose parent exists:

```sh
KEY="$HOME/.local/share/lm3s6965-boot-demo/LM3S6965-IQC50-A2-arm-cortex-m3/signing-key.pem"
python3 tools/boot/package.py --input build/application-A.elf --input-format elf \
  --slot A --release 42 --private-key "$KEY" --output build/release-42-A.pkg
```

For B, link a separate application at `0x24000`, select `--slot B`, and choose a
separate output. Changing slot flags, vector addresses or VTOR does **not**
relocate code. Existing project applications linked at zero are not eligible
inputs. This increment adds no production A/B linker or startup configuration.
The test-only linker fixtures are not usable applications.

**GNU ELF producer gate:** pass `-Wl,-z,max-page-size=0x100` on the final
slot-specific GCC link, in addition to the application's existing link options:

```sh
arm-none-eabi-gcc ... -T application-A.ld -Wl,-z,max-page-size=0x100 -o application-A.elf
```

Here `...` denotes the application's objects and ordinary link options, not a
complete runnable command. Use the same flag for B. This controls GNU ld's
maximum segment page alignment; it does not relocate code, create a slot linker
script or change runtime/startup ownership. The link script must already select
A/B's exact payload origin and capacity.

Verified producer: official **Arm GNU Toolchain 15.3.Rel1 (Build arm-15.149),
GCC 15.3.1 20260627**. With the real board layout transformed only to slot A's
FLASH origin/capacity, its default link emits a first PT_LOAD with VMA and
`p_paddr=0x8000`, file offset 0, including ELF headers, although `.text` and
vectors start at `0x8800`. A's origin is **not 4 KiB aligned** (`0x8800 % 0x1000
= 0x800`); that default segment mapping extends below the selected payload.
The strict parser correctly rejects it. B's `0x24000` origin is 4 KiB aligned;
the same observed default does not create this particular problem there.
The explicit `0x100` maximum page size gives supported A/B output without
hand-authored PHDRS. GNU defaults may differ across versions/targets: do not
assume every unflagged link fails A, or that an unflagged B link is universally
supported.

Release gates are **all required**, not alternative ways to waive parser checks:

1. Link for the exact selected slot; for the verified GNU ELF path explicitly
   supply the maximum-page-size flag and retain flash LMAs in `p_paddr`.
2. Inspect the actual load map (`arm-none-eabi-readelf -lW application-A.elf`)
   and pass the packager's full ELF checks below. No file-backed PT_LOAD may
   extend outside the selected payload, including a header-containing segment.
3. Check canonical payload equivalence against GNU objcopy and verify generated
   signatures with the native C verifier. The integration tests exercise these
   gates for both slots; they do not certify an arbitrary application build.

If a producer's ELF map does not satisfy these gates, fix its link options or
use the independently checked [canonical raw fallback](#raw-binary-alternative-caller-responsibility).
Do not discard out-of-slot PT_LOAD bytes or relax the parser to accept them.

`--release` is decimal uint32 `0..4294967295`, a label, not anti-downgrade policy.
`--input-format` is required: no extension-based/autodetected conversion.

## Engineering input contract: ELF

The parser reads at most 16 MiB and supports ELF32 little-endian System V ARM
**ET_EXEC**, version 1, ARM EABI5 with no flags other than optional soft-float.
It requires ordinary program and section tables (1–128 program headers and
1–4096 section headers); extended numbering, ELF64, big endian, dynamic,
interpreter, TLS, relocatable and unsupported allocated-section/header types
are rejected. This deliberately narrow contract is not a general ELF loader.

Section structure follows the [ELF gABI section header rules](https://gabi.xinuos.com/elf/03-sheader.html)
and [string table rules](https://gabi.xinuos.com/elf/04-strtab.html), with these
explicit supported-subset restrictions:

- Section 0 must be the **all-zero SHT_NULL sentinel**. ELF permits section 0's
  `sh_size`, `sh_link`, and `sh_info` to carry extended section counts, name-table
  indexes, and program counts. Those valid encodings were already unsupported:
  `e_shnum=0`, `e_shstrndx=SHN_XINDEX`, and `e_phnum=PN_XNUM` are rejected by the
  ordinary table limits. No supported extended-indexing form is lost by requiring
  the canonical sentinel. Other ignored/nonzero sentinel fields are also rejected
  as an explicit canonical-input restriction, not interpreted as payload.
- `e_shstrndx` must name a present, nonempty **SHT_STRTAB** with first and last
  bytes NUL. Every section's `sh_name` must index that table and reach a NUL
  terminator within it. ELF's missing-name-table option (`SHN_UNDEF`) and empty
  tables are deliberately unsupported. An empty name (`sh_name=0`) and indexes
  into string suffixes remain valid; no unique/nonempty/ASCII names are required.
- **All SHF_COMPRESSED sections are rejected**, including nonallocated debug
  metadata: there is no decompression support. ELF itself forbids this flag on
  SHF_ALLOC sections; they must not be accepted as ordinary load bytes.
- Every non-sentinel section's `sh_addralign` is 0, 1, or a power of two;
  0/1 impose no constraint, otherwise `sh_addr` must be a multiple of it, even
  for zero-sized sections. The gABI does **not** impose an independent
  `sh_offset % sh_addralign == 0` rule. File-backed allocated sections instead
  must match the PT_LOAD mapping below. PT_LOAD `p_align` is separately 0/1 or
  a power of two with `p_vaddr` congruent to `p_offset`; it does not require
  either to be individually aligned, nor constrain `p_paddr`. NOBITS has a
  conceptual file offset and occupies no file bytes. These distinctions avoid
  rejecting valid GNU-linked inputs by guessing section/LMA offset alignment.

**The producer must encode flash load addresses in PT_LOAD `p_paddr`.** This is
an explicit input requirement, not an inference from section names, `e_entry`,
VMA, or guessed `.data` placement. It was verified with the installed official
Arm GNU Toolchain 15.3.Rel1: GNU ld `AT(...)` places initialized SRAM `.data`'s
flash LMA in `p_paddr`; with the explicit maximum-page-size flag above,
GNU objcopy `-O binary --gap-fill 0xff` independently produces the same bytes
as this parser. The native tests build both A/B linked ELFs using temporary
copies of `linker/lm3s6965.ld` with only FLASH origin/capacity changed, preserving
`.data > SRAM AT> FLASH` and `.bss (NOLOAD)` without explicit PHDRS, and check
that equivalence. Separate explicit-PHDRS fixtures retain the reviewed mapping
and malformed-input regression checks. Other producers must satisfy the same contract;
a producer that emits SRAM as `p_paddr` for initialized data is rejected, not
silently interpreted or omitted.

1. Each nonempty PT_LOAD has a bounded VMA wholly inside the selected flash
   payload or `[0x20000000,0x20010000)` SRAM. Flash VMA must equal LMA and have
   no zero-filled memory tail. File-backed bytes of **every** PT_LOAD must have
   a flash LMA wholly inside the selected payload, including initialized SRAM
   data loads. SRAM-only initialized loads, boot/metadata/other-slot/manifest
   reserve/spare loads are rejected. Zero-file-size SRAM reservations are
   excluded; their LMA must still be a selected-slot or SRAM address.
2. All file bytes, file/memory sizes, uint32 address arithmetic, alignment and
   tables are checked before access. Overlapping flash LMAs, load VMAs, load
   file ranges, allocated sections, and header/load conflicts are rejected,
   including identical overlaps. Unsupported mapping is an error, not a reason
   to drop a segment.
3. Every nonempty allocated section must map uniquely to one PT_LOAD. Loaded
   PROGBITS, init/fini/preinit arrays and ARM EXIDX must have matching VMA/file
   offset and fit file-backed bytes. NOBITS must be SRAM after the file-backed
   portion, and is never copied into the payload. Nonallocated debug/symbol
   metadata is not payload, but its file ranges are still bounded.
4. The first flash load starts exactly at A `0x8800` or B `0x24000`. Bytes from
   there through the highest file-backed LMA are copied without transformation;
   uncovered internal gaps are `0xff`. The ELF entry must be inside an
   executable file-backed VMA but is **not** used as the reset vector. Up to
   three final `0xff` bytes align the payload to four bytes.

Section names are not authority. No `.bss` bytes or SRAM reservation tails are
included. Segment bytes, including producer-supplied padding, are preserved:
only genuinely uncovered LMA gaps receive `0xff`. This parser cannot prove
source/binary correspondence, correctness of absolute pointers, absence of
references to unsigned memory, or that a malicious producer describes its
intended runtime loads truthfully. It also does not certify SRAM diagnostic,
heap/stack ownership or startup behavior. Those are separate linker/runtime
acceptance obligations.

## Raw binary alternative: caller responsibility

```sh
python3 tools/boot/package.py --input build/application-A.bin --input-format bin \
  --slot A --release 42 --private-key "$KEY" --output build/release-42-A.pkg
```

**Precondition:** the caller has independently verified slot-specific linkage
and extracted *all* flash load bytes, including initialized-data LMAs, with
`0xff` internal gaps, starting at the exact slot origin. The canonical GNU extraction is:

```sh
arm-none-eabi-objcopy -O binary --gap-fill 0xff application-A.elf application-A.bin
```

Use the separately linked B ELF for B. GNU objcopy extracts loadable section
bytes at their LMAs, not the ELF-header bytes that a default PT_LOAD can include.
Thus this is also a valid fallback for the observed default-link A ELF rejected
above, **provided the caller independently checks** that the lowest extracted
load address is the exact slot origin, all initialized-data flash LMAs are
included, and the complete span fits that slot's payload. The board-layout tests
verify default-link A/B raw extraction against the supported flagged ELF path,
including signatures through the C verifier. No section-selection flags should
silently omit initialized data. Prefer the supported ELF path because it checks
the load map itself; raw extraction is not permission to ignore wrong linkage.

A raw file carries no address map: the packager cannot detect missing `.data`,
wrong internal fill, absolute code linked for another slot, omitted sections or
an incorrect extraction offset. It **preserves every internal byte**, even zero
bytes in gaps; it does not attempt to infer gaps or repair vectors. It adds only
final alignment `0xff`. Both formats require at least 236 input/load-span bytes
before padding, and at most 111616; canonical length is a multiple of four.

## Exact bytes and signing

| Manifest offset | Content |
| --- | --- |
| 0–15 | `LM3S6965-FW-V1` and two zero bytes |
| 16–47 | Eight explicit uint32 LE words: format 1, size 160, product 1, A=0/B=1, slot base, canonical payload length, release, flags 0 |
| 48–95 | SHA-256 of the **whole padded payload**, then 16 reserved zero bytes |
| 96–159 | P-256 signature `r || s`, each 32-byte unsigned big-endian |

Payload bounds are A `[0x8800,0x23c00)` and B `[0x24000,0x3f400)`.
Initial MSP must be exactly `0x20010000`; reset vector must have Thumb bit set,
and its cleared-bit target must be inside the authenticated canonical payload.
These are checked before signing. No vector rewriting is performed.

SHA-256 of exactly the 96-byte manifest prefix is computed **once**. OpenSSL
receives that 32-byte digest on stdin using:

```text
openssl pkeyutl -sign -inkey EXTERNAL_KEY -passin pass: -pkeyopt digest:sha256
```

There is no `-rawin`/`-digest` message-hashing option and no double hashing.
OpenSSL's DER signature is strictly decoded: short-form exact sequence length,
two minimal nonnegative INTEGERs, no extra bytes, and `1 <= r,s < P-256 order`.
It is converted to fixed-width `r || s`; DER is never packaged. Both high and
low `s` are accepted by the approved format; there is no added low-S policy.
ECDSA signatures are nondeterministic here, so repeated builds need not be
byte-identical even with the same canonical payload.

Package total length is 396–111776 bytes. **There is no 864-byte reserve tail,
wrapper, CRC or post-payload padding.** The package is a transport artifact,
not a directly flashable full-slot image.

## Private key and output safety

See [boot-demo-key.md](boot-demo-key.md) for custody and the exact documented
external private location. Existing demo public SEC1 SHA-256 fingerprint:
`f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d`.

The key argument must be an absolute path resolving outside the repository,
a regular nonsymlink file, mode 0600, with a mode-0700 containing directory.
Encrypted keys are not supported (explicit empty password, no prompt).
OpenSSL checks the key, derives **only public SPKI DER**, requires the named
P-256 OID/65-byte uncompressed SEC1 encoding, and compares that public point
with the fixed header. Wrong P-256 keys, other curves, explicit-parameter keys
and malformed keys fail. Python never reads, exports, copies or logs private
bytes; OpenSSL alone reads the original external file. Tool stderr is not
forwarded, and failures avoid logging key paths. Tests generate wrong-curve
and wrong-identity private keys directly through OpenSSL into temporary external
0700 directories and clean them up; the authorized existing demo key is reused,
not regenerated or modified.

Outputs must not already exist, including dangling symlinks. Input/key paths
and aliases cannot be overwritten; private-key input aliases are refused before
input reading. Signing/validation must finish successfully before any output
is published. A mode-0600 same-directory temporary file is written/fsynced,
then hard-linked to the requested name with exclusive creation, and removed.
Concurrent destination creation therefore fails without replacing its contents.
Failures clean up the temporary file and leave no partial destination. Filesystem
support for hard links is required; there is no unsafe overwrite fallback.
This is atomic visibility, **not a filesystem/power-loss durability guarantee**.

Use stable, trusted local directories and do not mutate input, key, header or
parent directories during the command. This is not a hostile concurrent
filesystem/key-management service; path checks are not a defense against an
account owner replacing a key or directories during signing. Private key paths
are command-line arguments visible to local process inspection; no key bytes
are arguments. Permissions do not defend against the account owner/admin.

## Native validation and C round trips

```sh
BOOT_TEST_BUILD_DIR="$PWD/build/boot-package-debug" bash scripts/boot/test.sh
BOOT_TEST_BUILD_DIR="$PWD/build/boot-package-release" bash scripts/boot/test.sh -DCMAKE_BUILD_TYPE=Release
build/boot-package-debug/package_verify A build/release-42-A.pkg
PYTHONDONTWRITEBYTECODE=1 python3 tests/boot/test_package.py -v
```

`tests/boot/package_verify.c` is a small **native test-only** CLI. It rejects
short/trailing package bytes and then calls the existing reviewed
`boot_verify_stored` with the fixed demo key and a bounded memory reader; it
does not replace or modify the production format/crypto/verifier APIs. Native
CTest runs the Python tests with this CLI under ASan/UBSan alongside the four
existing suites. Round trips cover A/B real board-layout ELF/raw producer equivalence and
signatures with the explicit maximum-page-size flag, default-link canonical raw
fallback, data LMAs, bss exclusion,
internal gaps, raw minimum/maximum/misalignment, tampering, physical wrong slot
and exact trailing lengths. OpenSSL independently verifies each generated
signature using `dgst -sha256` on the original prefix (message-input verification,
separate from the generator's digest-input signing). Review regressions mutate
real A/B section-zero/type/name-table/compression/alignment fields and preserve
valid original loads. C-verifier negative cases require **exact exit code 1**;
signals, other exit codes, and sanitizer/runtime-error stderr are test failures
with diagnostics, not accepted rejections. A simulated subprocess-result test
exercises the crash/sanitizer oracle without intentionally crashing a binary.

The fixture compiler prefers the installed official Arm bundle, or accepts
`BOOT_ARM_GCC=/absolute/path/to/arm-none-eabi-gcc`; objcopy is selected beside it.
If no bundle exists, it falls back to PATH. Compiler location and the board fixture compiler version are printed by the
tests. The board tests gate flagged A/B ELF acceptance and objcopy equivalence;
they require strict default-link rejection only when the actual file-backed
PT_LOAD crosses a slot boundary, rather than requiring one toolchain's default
failure on every version. Real-link tests explicitly skip if the compiler is missing; signing tests
explicitly skip if the documented external demo key is missing. An `OK` run
with those skips is **not** full round-trip evidence. Tests require OpenSSL;
no keys/toolchains/dependencies are silently downloaded. Bare-metal CMake still
returns before native Python/CLI configuration, preserving ARM archive builds.

These are host interoperability/rejection checks, not target final-link/32 KiB
fit, allocator/stack/timing/watchdog evidence or flash/lifecycle/runtime/FTP
acceptance. Independent review is the next gate for this increment.
