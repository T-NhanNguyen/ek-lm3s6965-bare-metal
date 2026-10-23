# Mbed TLS source provenance

## Approved artifact

- Version: **Mbed TLS 3.6.7**, release tag `mbedtls-3.6.7`, published upstream **2026-07-07T14:43:13Z**.
- Retrieved and checked: **2026-10-13 UTC**.
- Official repository: https://github.com/Mbed-TLS/mbedtls
- Official release: https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7
- Official source archive: https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
- Archive SHA256: `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`

The specification's `/releases/tag/v3.6.7` reference is not the release API tag: the API returned HTTP 404 for `v3.6.7`. The official repository's release listing identifies **the same approved version** under `mbedtls-3.6.7`; no version substitution was made. The release explicitly identifies `mbedtls-3.6.7.tar.bz2` as its official release file and warns that GitHub-generated source snapshots omit submodules/generated files. Those snapshots were not used.

## Integrity evidence and limits

Fetched over HTTPS from the official release assets:

https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7-sha256sum.txt

Its exact checksum line is:

```text
a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6  mbedtls-3.6.7.tar.bz2
```

`shasum -a 256` on the downloaded archive matched this line. The checksum file itself hashed to `7a9caa41bceabbbf4d3cbf6819d44440edb4f608ce2d5bc589eaa280e910da34`.

Independent metadata representation (same upstream trust boundary):
https://api.github.com/repos/Mbed-TLS/mbedtls/releases/tags/mbedtls-3.6.7

The release body publishes the same archive SHA256. Its archive asset `digest` is `sha256:a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`; its checksum-file asset `digest` matches the checksum-file hash above. Both were compared successfully. The API listed only the archive and checksum file, with **no detached signature asset**. No cryptographic signature or independently authenticated maintainer key was verified. These checks establish agreement with official HTTPS-hosted release metadata, not an independent signed supply-chain attestation.

## Local source and offline use

`third_party/mbedtls/` contains all **2039 regular files** from the verified archive, with its top-level directory stripped. All files were compared by SHA256 to the extracted archive and match byte-for-byte. Archive paths were checked for absolute paths/traversal and nonregular members before extraction; none were present. No ELF/Mach-O binaries or `.o`, `.a`, `.so`, `.exe` build artifacts were found. Upstream test fixtures, generated source, framework and third-party source are preserved for completeness. The compressed download and temporary verification files are not vendored.

Upstream licenses are preserved, including `third_party/mbedtls/LICENSE` and component license files. Source has not been configured, patched or built. Project CMake/source files are unchanged. Integration must consume this local tree: **no build-time download, FetchContent acquisition or submodule fetch is required or authorized by this acquisition**.

## Boot MVP API/prerequisite inspection

Verified directly in the pinned source (not an integration/fit test):

- `include/mbedtls/build_info.h` reports `MBEDTLS_VERSION_STRING "3.6.7"` and `0x03060700`.
- `include/mbedtls/ecdsa.h` declares `mbedtls_ecdsa_verify`: input is the already-computed message hash plus MPI `r/s`, matching the approved SHA256(manifest[0,96)) digest-input contract; no DER conversion or second hashing is needed.
- `include/mbedtls/bignum.h` declares `mbedtls_mpi_read_binary`; `include/mbedtls/ecp.h` declares `mbedtls_ecp_point_read_binary`, `mbedtls_ecp_check_pubkey` and group-loading APIs. Point import explicitly does not validate group membership; the wrapper must validate the fixed SEC1 point and scalar ranges.
- `include/mbedtls/check_config.h` requires ECP, an enabled suitable curve (including `MBEDTLS_ECP_DP_SECP256R1_ENABLED`), **both `MBEDTLS_ASN1_PARSE_C` and `MBEDTLS_ASN1_WRITE_C`** when `MBEDTLS_ECDSA_C` is enabled. Its ECP-light prerequisite check requires `MBEDTLS_BIGNUM_C` and an enabled curve. Required library sources and SHA256 sources are included. Raw signature format does not remove ASN.1 configuration prerequisites.

The final library configuration, complete compiled/link dependency closure, allocator/platform glue, signature test vectors, target stack/RAM/latency and **32 KiB total boot fit remain unverified**. No project build changes or hardware access were performed.

## Support and security caveats

The freshly retrieved official 3.6.7 release states that 3.6 is LTS with bug/security fixes **until at least March 2027**. At retrieval, the official five-release listing showed 3.6.7 as the newest listed 3.6 release; 4.x releases also exist and were not downloaded. This is dated evidence, not a promise that the pin will remain current or safe indefinitely. The specification's support-policy URL returned HTTP 404 during retrieval; the support statement here comes from the official release body instead.

3.6.7 includes security fixes; the release body links the advisories and documents ECC optimized-modp side-channel and malformed ECC-public-key fixes among many TLS/RSA/X.509 fixes. Acquisition alone is not a full advisory audit or assurance that no newer/unpublished vulnerability affects the boot verifier. Recheck maintained-branch/security status before deployment; any version change needs explicit approval rather than silently replacing this pin.

Retained `SECURITY.md` urges latest maintained releases, documents limited timing-side-channel protection, no physical/fault-injection protection, and compiler caveats (common `-O2`/`-Os` recommended). Signed firmware does not authenticate the restricted-bench FTP/status/confirmation channel, protect a replaceable bootloader trust root from physical debug access, or establish production security.
