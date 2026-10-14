# Public-only signed verifier fixtures

`verify_fixtures.h` contains 14 canonical 160-byte manifests, signed independently
of the C codec/verifier with the existing external demonstration key documented
in `docs/boot-demo-key.md`. No private key, nonce, or signing implementation is
embedded in the repository or tests. The production verifier uses only the fixed
`config/boot_demo_public_key.h` trust root; there is no test-key override API.

## Exact public payload definition

The fixture index is `slot * 7 + mode`, with A=0, B=1. Base is A=`0x8800`,
B=`0x24000`. Length is 111616 for mode 1, otherwise 236. Reconstruct bytes in this
order (the reader tests materialize these public bytes in host memory):

1. For every offset `i` in `[0,length)`, byte = `(i * 17 + slot * 31) & 255`.
2. Set bytes `[100,108)` and `[length-4,length)` to `0xFF` (internal gap and final pad).
3. Store vector words at offsets 0 and 4 as little-endian uint32:

| Mode | Initial MSP | Reset vector | Stored validation |
|---:|---|---|---|
| 0 | `0x20010000` | `base+9` | Valid minimum |
| 1 | `0x20010000` | `base+9` | Valid maximum |
| 2 | `0x2000FFFC` | `base+9` | MSP error |
| 3 | `0x20010000` | `base+8` | Thumb error |
| 4 | `0x20010000` | `base-1` | Target below payload |
| 5 | `0x20010000` | `base+length+1` | Target at exclusive end |
| 6 | `0x20010000` | `base+length-1` | Valid last halfword target |

All fixtures have valid signatures, including intentionally invalid vectors.
Release is `UINT32_MAX` (label only), flags/reserved zero, product/version 1,
manifest length 160. The stored hash is Python standard-library SHA-256 over
exactly the public payload described above. Prefix fields were encoded using
Python `struct.pack('<8I',...)`, not the project codec.

## Independent signing evidence

Generation ran locally using OpenSSL 3.6.3 and Python standard library in an
external temporary directory; no network or dependency installation. The
existing external demo key was passed to OpenSSL only, never read into Python,
printed, exported, or copied. For each fixture:

- Python hashed the exact 96-byte canonical prefix **once**.
- `openssl pkeyutl -sign -inkey <external-key> -pkeyopt digest:sha256 -in <digest> -out <DER-signature>` signed that digest without `-rawin` or message hashing.
- `openssl pkeyutl -verify -pubin -inkey <exported-public-key> -pkeyopt digest:sha256 -in <digest> -sigfile <DER-signature>` independently succeeded for each fixture.
- DER integers were decoded into unsigned fixed 32-byte big-endian `r || s`.
  Only canonical prefix and public signatures were retained.

ECDSA signatures need not be reproducible on re-signing; the checked-in public
fixtures are stable known-answer inputs. These are not executable firmware,
transport packages, a packager, or permission to flash.

## Test and evidence limits

`test_verify.c` checks signed A/B min/max payloads, signed invalid vectors,
mutation of every manifest byte, signature scalar zero/order/all-ones/1/n-1,
all short manifest lengths, invalid geometry/overflow/domain/version/reserved,
physical target mismatch, payload vector/code/gap/final-pad corruption,
short/zero/oversize reader results, progress ordering, cancellation before/after
signature and during/final hashing, exact bounds, and unchanged failure outputs.
Admission alone is explicitly tested as successful for signed bad vectors;
stored validation rejects them. Invalid admission invokes zero reader calls;
structurally invalid admission invokes zero precrypto progress events.

A separate host-only crypto archive substitutes calloc/free symbols without
changing the reviewed production archive/configuration. The same tests count
all successful outstanding allocations and require zero outstanding allocations
at the end. Allocation failure injection runs **96 sampled cases per slot** (endpoint
positions intentionally repeat):
first/last 16 plus 64 evenly spaced positions in a successful verification path.
Every sampled failure must return CRYPTO, leave output unchanged, read no
payload, and free all successful allocations. This is bounded sampling, **not
exhaustive failure at every one of the tens of thousands of arithmetic allocation
calls**, and not MCU heap/stack evidence. Apple ASan has no LeakSanitizer;
allocation accounting complements ASan/UBSan here. Linux additionally enables
LeakSanitizer using the existing test-project configuration.

The reader contract requires stable storage during verification and until use;
there is no lock, flash transaction or TOCTOU protection implemented here.
Progress callbacks do not interrupt ECDSA and imply no unconditional watchdog
feeding policy, measured latency, or 32 KiB boot fit. Cross-built static archives
are compilation evidence only, not final linked bootloader or hardware evidence.
