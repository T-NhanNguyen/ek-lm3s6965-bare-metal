# Both-slot fixture metadata — installed decision-only pair

## Current physical outcome — 2026-10-20

The matching-root boot and both FTP A44/B45 payloads, manifests and metadata
are installed and verified. Exactly pages **0–30,32–33,34–66,143,144–176,253**
(**101 pages**) were erased, with **seven writes/seven image verifications,
98652 bytes**. Metadata pages32–33 were erased and read back allFF first;
164 bytes were staged at `0x8000` and fully read back with erased commit/tails
and page1; the four-byte commit at `0x80a4` was the **last flash mutation**,
followed by exact full2048-byte readback. Page31 and unlisted pages were excluded
from commands, not independently compared byte-for-byte.

One SYSRESETREQ reset halt/continue then captured state7 and a successful
validity-gated RUN decision: A/release44, base `0x8800`, payload33532,
SHA256 `f3c924428c3f3bf80a6cf5848034641c2624cf4d9d6586dea6b204a293015ce3`.
The selected boot ELF SHA256 is
`0b99507803656980e77d6c0c1678d91e79f636f49af9caa88031d436e67b1292`;
provision manifest `build/rnd-ftp-led-pair/provision/manifest.json` SHA256 is
`1dc68c507c1f104c217944319ddbdb7c5dbaf78b24b625a80ffe9abf26d255f8`.
The normal sequence's conditional metadata writes were allowed; no manual writes
or independent post-decision metadata-page readback are claimed.

Cleanup last confirmed halted, breakpoint removed, owned server alive; no
persistence guarantee. No manual feed/freeze or dispatch. B fallback, LED/network
execution, retained startup/application initialization and watchdog timing remain
pending. The requested decision-and-stop increment is satisfied; bounded
application dispatch is the next objective, not implemented here. See
[reset-image observation](boot-reset-image.md) for raw clock configuration limits.

## Retained generated outcome — bounded offline evidence

The assigned pair and metadata are generated and accepted in bounded offline
scope: **FTP foreground in both slots**, A/release44 breathing PF0 and
B/release45 double-blink **100 ms ON / 100 ms OFF / 100 ms ON / 500 ms OFF**.
Both payloads are **33532 bytes**, packages **33692 bytes**. Actual ignored
outputs in `build/rnd-ftp-led-pair/` are `slot-a-release44.pkg`,
`slot-b-release45.pkg`, `metadata-page0.bin` and `metadata-page1.bin`.
Retained standalone A42/FTP B43 artifacts and historical results are unchanged.

Both actual packages passed selected-root native real-MbedTLS authentication
and payload checks; metadata was generated with the real encoder and read back
with the real decoder: **sequence1, A current, B previous, both confirmed
(mask3), no candidate/intent, accounting/trial IDs0**. Page0 contains a valid
168-byte record and FF tail through byte1023; page1 is entirely erased FF.
The retained raw65 public root is `build/rnd-breathing-led/public.sec1`, SHA256
`960d06c1aaa1206cf37ce8e8b0cd002dfa441b9324e25c8c5a0735651f12c389`.
The matching boot is now installed as recorded above, replacing the older
RFC-root boot. No replacement key was generated.

Recorded acceptance is **7 pass / 0 fail / 0 invalid**, five logical
representatives/seven configurations, not seven independent test classes.
ARM image checks are static; LED evidence is GPIO-mocked host evidence.
That offline gate alone did not establish runtime selection. The later physical
observation above accepts A44 verification/selection only, **not B fallback,
physical LED/FTP, full inherited network/watchdog runtime or dispatch**. This
documentation update runs no builds/tests/signing/hardware operations.

### Exact installed program/erase envelope — not new authorization

| Component | Program | Page-rounded erase (1024-byte pages) |
| --- | --- | --- |
| A payload | `[0x8800,0x10afc)` | `[0x8800,0x10c00)` — 33 pages |
| A manifest160 | `[0x23c00,0x23ca0)` | `[0x23c00,0x24000)` — 1 page |
| B payload | `[0x24000,0x2c2fc)` | `[0x24000,0x2c400)` — 33 pages |
| B manifest160 | `[0x3f400,0x3f4a0)` | `[0x3f400,0x3f800)` — 1 page |
| Metadata page0 | stage `[0x8000,0x80a4)`; commit `[0x80a4,0x80a8)` last | `[0x8000,0x8400)` — 1 page |
| Metadata page1 | none; actually erase the entire page | `[0x8400,0x8800)` — 1 page |
| Matching retained boot | `[0,0x797c)`, including data LMA | `[0,0x7c00)` — 31 pages; preserve `[0x7c00,0x8000)` |

A package is manifest+payload transport, not a contiguous flash image. Do not
program the entire metadata page as one staged record: commit the four valid
bytes last after verifying the first164 bytes. The completed installation covered
both metadata pages and allowed normal conditional decision-run writes; this
record grants no repeat authority. Decision-only
boot cannot launch these fixtures. See [build recipe](../examples/ethernet-ftp/README.md).
The following recipes are reference workflows, not instructions to overwrite
accepted outputs or authorization to rerun signing/builds/tests.

## Build the native tool with an explicit PUBLIC root

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
uint32; slots are fixed by `--package-a`/`--package-b`. It calls the real
`boot_verify_stored` against the build-selected root, authenticates both
signatures, hashes every declared payload byte, checks vectors/geometry and
rejects release mismatch, trailing bytes, truncation and swapped slots. There
is no key lookup, signing, runtime trust override or metadata input reuse.

## Verify separately, then generate fresh ignored outputs

Set `PACKAGE_A`, `PACKAGE_B`, `RELEASE_A`, `RELEASE_B` explicitly to the new
actual packages and their intended labels (for example fresh 44/45, not implicit
defaults). All four are required; no filenames or versions are inferred.

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
symlinks checked). Use ignored `build/` as above; `*.bin` is also ignored. Neither
existing output is overwritten. Page0 contains the valid 168-byte record with
FF tail; page1 is entirely FF. This is offline readback material, **not a flash
programming script**. For separately authorized programming, stage exactly
**164 bytes at 0x8000**, verify, then program the **4-byte valid commit at
0x80a4** last. Page1 `[0x8400,0x8800)` must actually be erased; do not merely
leave an older valid page there. Both metadata erase pages and any decision-run
writes need explicit authority. No erase/program/reset/dispatch is done here.
The installed board now has the matching selected public root; the decision-only
boot still cannot launch either fixture. A valid generated output alone is not
runtime acceptance.

## Reference checks (further execution: test-merit-gate only)

```sh
cmake -S tests/ethernet/fixture-pattern -B /tmp/lm3s6965-pattern-tests
cmake --build /tmp/lm3s6965-pattern-tests
ctest --test-dir /tmp/lm3s6965-pattern-tests --output-on-failure
ctest --test-dir /tmp/lm3s6965-metadata-tools --output-on-failure
python3 tests/boot/test_fixture_metadata_tool.py --tool "$TOOL" \
  --package-a "$PACKAGE_A" --release-a "$RELEASE_A" \
  --package-b "$PACKAGE_B" --release-b "$RELEASE_B"
```

The native pattern tests cover four full cycles, brightness endpoints and exact
pulse-density conservation/counts, every double-blink boundary, local wrap,
no-call/no-catch-up semantics and pre-init GPIO guard with mocked GPIO. They do
not model physical missed interrupts or prove visual smoothness. Codec tests
cover roundtrip/select/FF tails, identity staleness, CRC tamper and erased-commit
rejection; these use synthetic manifests and are **not signature evidence**.
The separate supplied-package CLI regression exercises the real selected-root
verifier, both payload/signature tamper rejects, stale releases, swapped slots,
exact lengths, exclusive output preservation and no files on failed verification.
