# Signed packages and trust roots

## Local identity and build wrappers

Populate pinned dependencies first. With the official Arm GNU/newlib toolchain,
CMake 3.20+, Python and OpenSSL installed, a separately authorized new identity
can be created with `python3 scripts/setup-boot-demo-keys.py --init-env`.
This creates external `signing-key.pem` (0600) and raw public SEC1 (65 bytes)
in the resolved sibling `<repo-name>-keys` directory (0700). `--out-dir` selects
another external directory. Existing files, symlinks, unsafe permissions and an
existing `.env` with `--init-env` are refused before generation, not repaired.
Only OpenSSL reads private bytes. Setup never builds, flashes or provisions.
It does not change the fixed public header. CMake owns generated headers.

Recover a lost key from backup before considering migration. Deliberate new
identity setup without `--init-env` requires manually migrating all four config
fields and installing a matching device bootloader root. FTP cannot rotate that
root. If config creation fails after key publication, keep the successful pair
and configure manually rather than generating another pair.

`scripts/build_ab_boot_demo [PUBLIC_KEY_FILE]` always reads the ignored root
`.env`, never inherited configuration variables. It requires exactly one
nonempty assignment for each of `LM3S6965_BOOT_PUBLIC_KEY_FILE`,
`LM3S6965_BOOT_PRIVATE_KEY_FILE`, `LM3S6965_BOOT_RELEASE_A` and
`LM3S6965_BOOT_RELEASE_B`. Releases are decimal uint32 (`0..4294967295`).
The private path must be absolute and outside the repository, including aliases.
Store paths only, never private bytes. Initial releases 1/2 are not uniqueness
or anti-downgrade guarantees. Choose approved releases before signing.

The bounded literal parser accepts at most 4096 bytes, blank lines, full-line
`#` comments, whitespace around `=` and matched simple quotes. No expansion,
escapes, shell execution or inline comments occur. Unknown/duplicate keys,
missing/empty fields, malformed quotes and nonregular files (including symlinks)
fail before CMake. PUBLIC paths in config resolve from the repository root.
The optional single PUBLIC argument resolves from the caller and overrides only
PUBLIC, not the complete config requirement. CMake rejects invalid/demo roots.

The wrapper uses Release/Unix Makefiles in ignored root `build/`, with production
dispatch ON, decision-only reset OFF and diagnostics OFF. It builds only
`lm3s6965_ab_ftp_a` and `lm3s6965_ab_ftp_b`, then invokes `tools/boot/package.py`
for their exact per-slot `.bin` files. The packager owns private permissions,
aliases, key matching, payload validation and signatures. Only after both succeed
is exactly `slot-a.pkg` plus `slot-b.pkg` published in
`build/ab-demo-packages/release-<A>-<B>/`. Existing destinations and symlinks are
refused before building. Publishers are serialized. Signing failure cleans only
the temporary sibling directory. Success prints, but does not execute, the exact
FTP download command. No dependency fetch, hardware or network access occurs.

Direct presets do not read `.env` or inherited PUBLIC configuration and do not
sign packages. From the repository root:

```sh
cmake --preset ab-boot-demo -DLM3S6965_BOOT_PUBLIC_KEY_FILE=/absolute/path/to/public.sec1
cmake --build --preset ab-boot-demo
```

The preset initializes PUBLIC empty, so explicit `-D` is required. Existing root
build artifacts and unrelated feature cache selections are preserved. Resolve
incompatible generator/toolchain caches deliberately. Manual configurations may
use external build directories. Other source descendants outside `build/` are
rejected for production PUBLIC material.

`scripts/build_boot_dispatch [PUBLIC_KEY_FILE]` separately builds only the
dispatch bootloader and prints its flash command. It reads the same bounded
literal file, recognizes the same four names, but requires only nonempty PUBLIC.
It does not sign applications or provision metadata. The optional PUBLIC argument
still requires a valid config file. Neither build wrapper establishes blank-board
provisioning or hardware acceptance.


A package is a 160-byte signed manifest followed by the exact canonical payload. It is not a contiguous flash image. The device trust root and signing identity must agree. Signing, root rotation, provisioning and transport are separate operations.

## Find the contract

- [Generate a slot artifact](#generate-a-slot-artifact)
- [Public trust root](#public-trust-root)
- [ELF input contract](#elf-input-contract)
- [Raw binary contract](#raw-binary-contract)
- [Manifest and signing bytes](#manifest-and-signing-bytes)
- [Key and output safety](#key-and-output-safety)
- [Demo key custody and rotation](#demo-key-custody-and-rotation)
- [Package verification](#package-verification)
- [Offline factory metadata](#offline-factory-metadata)


## Generate a slot artifact

### Generate one slot-specific artifact

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
inputs. The repository supplies self-contained slot scripts: `linker/app_a.ld`
for A and `linker/app_b.ld` for B. Use the selected script for the final link.

**GNU ELF producer gate:** pass `-Wl,-z,max-page-size=0x100` on the final
slot-specific GCC link, in addition to the application's existing link options:

```sh
arm-none-eabi-gcc ... -T application-A.ld -Wl,-z,max-page-size=0x100 -o application-A.elf
```

Here `...` denotes the application's objects and ordinary link options, not a
complete runnable command. Use the same flag for B. This controls GNU ld's
maximum segment page alignment. It does not relocate code, create a slot linker
script or change runtime/startup ownership. The link script must already select
A/B's exact payload origin and capacity.

Verified producer: official **Arm GNU Toolchain 15.3.Rel1 (Build arm-15.149),
GCC 15.3.1 20260627**. With the real board layout transformed only to slot A's
FLASH origin/capacity, its default link emits a first PT_LOAD with VMA and
`p_paddr=0x8000`, file offset 0, including ELF headers, although `.text` and
vectors start at `0x8800`. A's origin is **not 4 KiB aligned** (`0x8800 % 0x1000
= 0x800`). That default segment mapping extends below the selected payload.
The strict parser correctly rejects it. B's `0x24000` origin is 4 KiB aligned.
the same observed default does not create this particular problem there.
The explicit `0x100` maximum page size gives supported A/B output without
hand-authored PHDRS. GNU defaults may differ across versions/targets: do not
assume every unflagged link fails A, or that an unflagged B link is universally
supported.

Release gates are **all required**, not alternative ways to waive parser checks:

1. Link for the exact selected slot. For the verified GNU ELF path explicitly
   supply the maximum-page-size flag and retain flash LMAs in `p_paddr`.
2. Inspect the actual load map (`arm-none-eabi-readelf -lW application-A.elf`)
   and pass the packager's full ELF checks below. No file-backed PT_LOAD may
   extend outside the selected payload, including a header-containing segment.
3. Check canonical payload equivalence against GNU objcopy and verify generated
   signatures with the native C verifier. The integration tests exercise these
   gates for both slots. They do not certify an arbitrary application build.

If a producer's ELF map does not satisfy these gates, fix its link options or
use the independently checked [canonical raw fallback](#raw-binary-alternative-caller-responsibility).
Do not discard out-of-slot PT_LOAD bytes or relax the parser to accept them.

`--release` is decimal uint32 `0..4294967295`, a label, not anti-downgrade policy.
`--input-format` is required: no extension-based/autodetected conversion.


## Public trust root

### Explicit public trust root (optional)

Add `--public-key /absolute/path/to/authorized-public.sec1` to either ELF or raw
packaging commands to select an authorized root. The file must contain **exactly
65 raw binary bytes**, uncompressed SEC1 `04 || X || Y` for P-256: not PEM, DER,
hex text, or a compressed point. Both coordinates must be below the field prime
and the point must lie on P-256. Validation is read-only and creates no header.
The selected point must exactly equal the public point OpenSSL derives from the
private key. Missing, malformed, wrong-curve or mismatching selection fails
before output creation. There is **no fallback to the demo root**. A public-file
alias of the private key is rejected before reading it. Public files need no
secret custody modes. Private-key requirements below remain unchanged.

Python callers may use `package(input_path, output, slot, release, key,
input_format, public_key=None)` and `validate_key(key, public_key=None)`, with
`Path` arguments for files. The new trailing optional parameter preserves all
existing positional calls. `None` means the fixed demo header, not discovery.
A selected root must independently match the intended device build. Packaging
success is not provisioning or verification on that device.


## ELF input contract

### Engineering input contract: ELF

The parser reads at most 16 MiB and supports ELF32 little-endian System V ARM
**ET_EXEC**, version 1, ARM EABI5 with no flags other than optional soft-float.
It requires ordinary program and section tables (1–128 program headers and
1–4096 section headers). Extended numbering, ELF64, big endian, dynamic,
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
  into string suffixes remain valid. No unique/nonempty/ASCII names are required.
- **All SHF_COMPRESSED sections are rejected**, including nonallocated debug
  metadata: there is no decompression support. ELF itself forbids this flag on
  SHF_ALLOC sections. They must not be accepted as ordinary load bytes.
- Every non-sentinel section's `sh_addralign` is 0, 1, or a power of two.
  0/1 impose no constraint, otherwise `sh_addr` must be a multiple of it, even
  for zero-sized sections. The gABI does **not** impose an independent
  `sh_offset % sh_addralign == 0` rule. File-backed allocated sections instead
  must match the PT_LOAD mapping below. PT_LOAD `p_align` is separately 0/1 or
  a power of two with `p_vaddr` congruent to `p_offset`. It does not require
  either to be individually aligned, nor constrain `p_paddr`. NOBITS has a
  conceptual file offset and occupies no file bytes. These distinctions avoid
  rejecting valid GNU-linked inputs by guessing section/LMA offset alignment.

**The producer must encode flash load addresses in PT_LOAD `p_paddr`.** This is
an explicit input requirement, not an inference from section names, `e_entry`,
VMA, or guessed `.data` placement. It was verified with the installed official
Arm GNU Toolchain 15.3.Rel1: GNU ld `AT(...)` places initialized SRAM `.data`'s
flash LMA in `p_paddr`. With the explicit maximum-page-size flag above,
GNU objcopy `-O binary --gap-fill 0xff` independently produces the same bytes
as this parser. The native tests build both A/B linked ELFs using temporary
copies of `linker/lm3s6965.ld` with only FLASH origin/capacity changed, preserving
`.data > SRAM AT> FLASH` and `.bss (NOLOAD)` without explicit PHDRS, and check
that equivalence. Separate explicit-PHDRS fixtures retain the reviewed mapping
and malformed-input regression checks. Other producers must satisfy the same contract.
a producer that emits SRAM as `p_paddr` for initialized data is rejected, not
silently interpreted or omitted.

1. Each nonempty PT_LOAD has a bounded VMA wholly inside the selected flash
   payload or `[0x20000000,0x20010000)` SRAM. Flash VMA must equal LMA and have
   no zero-filled memory tail. File-backed bytes of **every** PT_LOAD must have
   a flash LMA wholly inside the selected payload, including initialized SRAM
   data loads. SRAM-only initialized loads, boot/metadata/other-slot/manifest
   reserve/spare loads are rejected. Zero-file-size SRAM reservations are
   excluded. Their LMA must still be a selected-slot or SRAM address.
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
   there through the highest file-backed LMA are copied without transformation.
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


## Raw binary contract

### Raw binary alternative: caller responsibility

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
the load map itself. Raw extraction is not permission to ignore wrong linkage.

A raw file carries no address map: the packager cannot detect missing `.data`,
wrong internal fill, absolute code linked for another slot, omitted sections or
an incorrect extraction offset. It **preserves every internal byte**, even zero
bytes in gaps. It does not attempt to infer gaps or repair vectors. It adds only
final alignment `0xff`. Both formats require at least 236 input/load-span bytes
before padding, and at most 111616. Canonical length is a multiple of four.


## Manifest and signing bytes

### Exact bytes and signing

| Manifest offset | Content |
| --- | --- |
| 0–15 | `LM3S6965-FW-V1` and two zero bytes |
| 16–47 | Eight explicit uint32 LE words: format 1, size 160, product 1, A=0/B=1, slot base, canonical payload length, release, flags 0 |
| 48–95 | SHA-256 of the **whole padded payload**, then 16 reserved zero bytes |
| 96–159 | P-256 signature `r || s`, each 32-byte unsigned big-endian |

Payload bounds are A `[0x8800,0x23c00)` and B `[0x24000,0x3f400)`.
Initial MSP must be exactly `0x20010000`. Reset vector must have Thumb bit set,
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
It is converted to fixed-width `r || s`. DER is never packaged. Both high and
low `s` are accepted by the approved format. There is no added low-S policy.
ECDSA signatures are nondeterministic here, so repeated builds need not be
byte-identical even with the same canonical payload.

Package total length is 396–111776 bytes. **There is no 864-byte reserve tail,
wrapper, CRC or post-payload padding.** The package is a transport artifact,
not a directly flashable full-slot image.


## Key and output safety

### Private key and output safety

See [demo key custody](#demo-key-custody-and-rotation) for the documented
external private location. Existing demo public SEC1 SHA-256 fingerprint:
`f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d`.

The key argument must be an absolute path resolving outside the repository,
a regular nonsymlink file, mode 0600, with a mode-0700 containing directory.
Encrypted keys are not supported (explicit empty password, no prompt).
OpenSSL checks the key, derives **only public SPKI DER**, requires the named
P-256 OID/65-byte uncompressed SEC1 encoding, and compares that public point
with the selected public file, or the fixed demo header when omitted. Wrong
P-256 identities, other curves, explicit-parameter keys and malformed keys fail. Python never reads, exports, copies or logs private
bytes. OpenSSL alone reads the original external file. Tool stderr is not
forwarded, and failures avoid logging key paths. Tests generate wrong-curve
and wrong-identity private keys directly through OpenSSL into temporary external
0700 directories and clean them up. New selected-root tests likewise generate
only ephemeral external keys. They never read the existing demo private key.
Legacy demo signing tests optionally reuse that key, not regenerate or modify it.

Outputs must not already exist, including dangling symlinks. Input/key paths
and aliases cannot be overwritten. Private-key input aliases are refused before
input reading. Signing/validation must finish successfully before any output
is published. A mode-0600 same-directory temporary file is written/fsynced,
then hard-linked to the requested name with exclusive creation, and removed.
Concurrent destination creation therefore fails without replacing its contents.
Failures clean up the temporary file and leave no partial destination. Filesystem
support for hard links is required. There is no unsafe overwrite fallback.
This is atomic visibility, **not a filesystem/power-loss durability guarantee**.

Use stable, trusted local directories and do not mutate input, private/public key, header or
parent directories during the command. This is not a hostile concurrent
filesystem/key-management service. Path checks are not a defense against an
account owner replacing a key or directories during signing. Private key paths
are command-line arguments visible to local process inspection. No key bytes
are arguments. Permissions do not defend against the account owner/admin.


## Demo key custody and rotation

This key is an authorized local demonstration key, **not a production trust
root**. Only its public key is in this repository. No hardware was contacted.
this does not implement or validate a bootloader, provisioning, or target fit.
The existing design contract specifies ECDSA P-256 and SEC1 uncompressed public
keys: `0x04 || x || y`, with each coordinate 32 bytes unsigned big-endian.

### Local identity and custody

- Private key: `../<repo>-keys/signing-key.pem`
- Private directory mode: `0700`. Private key mode: `0600`.
- Public include: `config/boot_demo_public_key.h`, symbol
  `lm3s6965_boot_demo_public_key` (65 bytes).
- Public fingerprint: **SHA-256 of the exact 65 SEC1 bytes**, not PEM or SPKI DER:
  `f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d`.

The PEM is an unencrypted OpenSSL private key, protected by local filesystem
permissions. Filesystem permissions do not protect against the account owner,
administrators, malware, or insecure backups. Never copy it into the repository,
vault, firmware, logs, or chat. Preserve it in appropriately protected local
storage if demo artifacts must remain verifiable. Key generation used exclusive
file creation and did not overwrite an existing key. Losing or rotating it
requires updating the public include/fingerprint and re-signing demo artifacts.
old artifacts do not verify under a new key.

### Verify the current key and include

Run from the repository root. This prints validity and the **public** fingerprint,
never private values. Do not add `-text` to a private-key inspection command.

```sh
openssl pkey -in "$HOME/.local/share/lm3s6965-boot-demo/LM3S6965-IQC50-A2-arm-cortex-m3/signing-key.pem" -check -noout
python3 - <<'PY'
import hashlib, pathlib, re, stat, subprocess
repo = pathlib.Path.cwd().resolve()
key = pathlib.Path.home() / '.local/share/lm3s6965-boot-demo' / repo.name / 'signing-key.pem'
assert not key.resolve().is_relative_to(repo)
assert stat.S_IMODE(key.parent.stat().st_mode) == 0o700
assert stat.S_IMODE(key.stat().st_mode) == 0o600
der = subprocess.check_output(['openssl', 'pkey', '-in', str(key),
    '-pubout', '-outform', 'DER', '-ec_conv_form', 'uncompressed'])
# Exact SPKI prefix asserts id-ecPublicKey and prime256v1 (P-256) OIDs.
prefix = bytes.fromhex('3059301306072a8648ce3d020106082a8648ce3d030107034200')
assert len(der) == 91 and der[:26] == prefix
public = der[26:]
assert len(public) == 65 and public[0] == 4
header = (repo / 'config/boot_demo_public_key.h').read_text()
initializer = header.split('= {', 1)[1].split('};', 1)[0]
embedded = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', initializer))
assert embedded == public, 'Header differs from private-derived public key'
fingerprint = hashlib.sha256(public).hexdigest()
assert fingerprint == 'f3ab1a07197e14750365367b1c09a2b0848c7062a419f82d7f1b087d03e7518d'
print('P-256, SEC1/header derivation and permissions verified; SHA-256:', fingerprint)
PY
```

Generation validation also ran `openssl pkey -pubin -pubcheck -noout` on the

### Regeneration (explicit rotation only)

Do not run this for the existing key. The exclusive open below refuses to
replace any file or symlink at the key path. For intentional rotation, choose a
new external directory first. Never delete/overwrite the current key casually.
Afterward derive the uncompressed public key with the verification procedure,
update the header/fingerprint deliberately, and re-sign the demo artifacts.

```sh
python3 - <<'PY'
import os, pathlib, subprocess
repo = pathlib.Path.cwd().resolve()
directory = pathlib.Path.home() / '.local/share/lm3s6965-boot-demo' / repo.name
assert not directory.resolve().is_relative_to(repo)
assert not directory.is_symlink()
directory.mkdir(parents=True, exist_ok=True, mode=0o700)
os.chmod(directory, 0o700)
key = directory / 'signing-key.pem'
fd = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'wb') as output:
    subprocess.run(['openssl', 'genpkey', '-algorithm', 'EC',
        '-pkeyopt', 'ec_paramgen_curve:prime256v1'], stdout=output, check=True)
subprocess.run(['openssl', 'pkey', '-in', str(key), '-check', '-noout'], check=True)
PY
```

### Signing usage (host-only example, not a package generator)

Prepare `manifest-prefix.bin` as exactly the canonical 96-byte `manifest[0,96)`
with validated fields and the payload SHA-256 already at offset 48. Hash those
96 bytes **once**, then use the digest-input OpenSSL API without `-rawin` or
`-digest` (those options select message hashing). OpenSSL emits DER ECDSA, which
is **not** the wire format: convert to fixed 64-byte `r || s` before placing at
manifest offsets `[96,160)`. No private bytes are included in either signature.
Use a scratch directory outside the repository for these inputs/outputs:

```sh
KEY="$HOME/.local/share/lm3s6965-boot-demo/LM3S6965-IQC50-A2-arm-cortex-m3/signing-key.pem"
# Execute sequentially; stop on any failed command.
python3 - <<'PY'
import hashlib, pathlib
prefix = pathlib.Path('manifest-prefix.bin').read_bytes()
assert len(prefix) == 96
pathlib.Path('manifest.sha256').write_bytes(hashlib.sha256(prefix).digest())
PY
openssl pkeyutl -sign -inkey "$KEY" -pkeyopt digest:sha256 -in manifest.sha256 -out signature.der
openssl pkey -in "$KEY" -pubout -ec_conv_form uncompressed -out demo-public.pem
openssl pkey -pubin -in demo-public.pem -pubcheck -noout
openssl pkeyutl -verify -pubin -inkey demo-public.pem -pkeyopt digest:sha256 -in manifest.sha256 -sigfile signature.der
python3 - <<'PY'
import pathlib
# Strict decoder for OpenSSL's short P-256 DER signature, not ECC arithmetic.
der = pathlib.Path('signature.der').read_bytes()
assert 8 <= len(der) <= 72 and der[0] == 0x30 and der[1] == len(der) - 2
n = int('ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551', 16)
position, scalars = 2, []
for _ in range(2):
    assert position + 2 <= len(der) and der[position] == 0x02
    size = der[position + 1]
    position += 2
    encoded = der[position:position + size]
    assert 1 <= size <= 33 and len(encoded) == size and not (encoded[0] & 0x80)
    assert size == 1 or encoded[0] != 0 or (encoded[1] & 0x80)
    value = int.from_bytes(encoded, 'big')
    assert 1 <= value < n
    scalars.append(value.to_bytes(32, 'big'))
    position += size
assert position == len(der)
pathlib.Path('signature.raw').write_bytes(b''.join(scalars))
assert pathlib.Path('signature.raw').stat().st_size == 64
PY
```

This example validates the crypto operation and encoding only. It does not
validate domain/product/slot/base, payload bounds/hash/vectors, build a complete
FTP package, or authorize flashing. Future C verification must validate the
P-256 public point and scalar ranges and verify the same 32-byte digest using
the approved raw digest/MPI verifier contract. No C verifier integration or
production key provisioning is performed here.


## Package verification

### Native validation and C round trips

```sh
BOOT_TEST_BUILD_DIR="$PWD/build/boot-package-debug" bash scripts/boot/test.sh
BOOT_TEST_BUILD_DIR="$PWD/build/boot-package-release" bash scripts/boot/test.sh -DCMAKE_BUILD_TYPE=Release
build/boot-package-debug/package_verify A build/release-42-A.pkg
PYTHONDONTWRITEBYTECODE=1 python3 tests/boot/test_package.py -v
```

`tests/boot/package_verify.c` is a small **native test-only** CLI. It rejects
short/trailing package bytes and then calls the existing reviewed
`boot_verify_stored` with the fixed demo key and a bounded memory reader. It
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
valid original loads. C-verifier negative cases require **exact exit code 1**.
signals, other exit codes, and sanitizer/runtime-error stderr are test failures
with diagnostics, not accepted rejections. A simulated subprocess-result test
exercises the crash/sanitizer oracle without intentionally crashing a binary.

The fixture compiler prefers the installed official Arm bundle, or accepts
`BOOT_ARM_GCC=/absolute/path/to/arm-none-eabi-gcc`. Objcopy is selected beside it.
If no bundle exists, it falls back to PATH. Compiler location and the board fixture compiler version are printed by the
tests. The board tests gate flagged A/B ELF acceptance and objcopy equivalence.
they require strict default-link rejection only when the actual file-backed
PT_LOAD crosses a slot boundary, rather than requiring one toolchain's default
failure on every version. Real-link tests explicitly skip if the compiler is missing. Signing tests
explicitly skip if the documented external demo key is missing. An `OK` run
with those skips is **not** full round-trip evidence. Tests require OpenSSL.
no keys/toolchains/dependencies are silently downloaded. Bare-metal CMake still
returns before native Python/CLI configuration, preserving ARM archive builds.

Selected-root regression cases are provided in `SelectedRootTests`: A/B Python
and CLI signatures checked independently with OpenSSL message-input SHA-256,
malformed/missing/mismatched selections with no output or fallback, default-root
compatibility, and retained custody/alias rejection. The fixed-demo C verifier
is deliberately not used to accept a different root. `PayloadTests` also checks
high-S acceptance in the DER-to-raw decoder. This is not a separate ECC verifier.


## Offline factory metadata

### Build the native tool with an explicit PUBLIC root

Use an out-of-repository build tree, local populated Mbed TLS and an existing
raw65 SEC1 public file. This standalone CMake entry point does not change the
cross-root defaults or read any private key. The tool refuses demo fallback.
The shared root validator checks the public point and prints its SHA256.

```sh
cmake -S tools/boot -B /tmp/lm3s6965-metadata-tools \
  -DCMAKE_BUILD_TYPE=Release \
  -DLM3S6965_BOOT_PUBLIC_KEY_FILE="$PWD/build/rnd-breathing-led/public.sec1" \
  -DBOOT_FIXTURE_TOOL_TESTS=ON
cmake --build /tmp/lm3s6965-metadata-tools
```

`boot_fixture_metadata_tool` consumes two exact existing transport packages,
never ELF/BIN guesses. Both explicit expected release IDs are mandatory decimal
uint32. Slots are fixed by `--package-a`/`--package-b`. It calls the real
`boot_verify_stored` against the build-selected root, authenticates both
signatures, hashes every declared payload byte, checks vectors/geometry and
rejects release mismatch, trailing bytes, truncation and swapped slots. There
is no key lookup, signing, runtime trust override or metadata input reuse.

### Verify separately, then generate fresh ignored outputs

Set `PACKAGE_A`, `PACKAGE_B`, `RELEASE_A`, `RELEASE_B` explicitly to the new
actual packages and their intended labels. There are no implicit defaults. All four are required. No filenames or versions are inferred.

```sh
TOOL=/tmp/lm3s6965-metadata-tools/boot_fixture_metadata_tool
"$TOOL" --package-a "${PACKAGE_A:?}" --release-a "${RELEASE_A:?}" \
  --package-b "${PACKAGE_B:?}" --release-b "${RELEASE_B:?}" --verify-only
mkdir -p build/corrected-ftp-metadata
"$TOOL" --package-a "$PACKAGE_A" --release-a "$RELEASE_A" \
  --package-b "$PACKAGE_B" --release-b "$RELEASE_B" \
  --output-dir build/corrected-ftp-metadata
```

Generation re-verifies both actual packages, then uses `boot_metadata_encode`,
`boot_metadata_decode`, `boot_metadata_select` and image-identity checks in real
C. Fixed policy: **sequence1, A current, B previous, confirmed mask3, no
candidate, no intent, zero accounting/trial IDs**. Package hashes/releases/lengths
populate both entries. No sequence override or stale page selection is accepted.

Outputs are exclusively created `metadata-page0.bin` and `metadata-page1.bin`,
**1024 bytes each**, in an existing directory outside `.dev-vault` (resolved
symlinks checked). Use ignored `build/` as above. `*.bin` is also ignored. Neither
existing output is overwritten. Page0 contains the valid 168-byte record with
FF tail. Page1 is entirely FF. This is offline readback material, **not a flash
programming script**. For separately authorized programming, stage exactly
**164 bytes at 0x8000**, verify, then program the **4-byte valid commit at
0x80a4** last. Page1 `[0x8400,0x8800)` must actually be erased. Do not merely
leave an older valid page there. Both metadata erase pages and any decision-run
writes need explicit authority. No erase/program/reset/dispatch is done here.
The device must independently contain the matching public root. A decision-only
boot cannot launch either fixture. A valid generated output alone is not runtime
acceptance.
