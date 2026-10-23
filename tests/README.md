# Test launchpad

Choose checks for the changed contract, not every roster in sequence.
The [tools and validation chapter](../docs/tools-testing.md) owns procedures,
prerequisites, reporting and evidence limits.

| Topic | Entry point |
|---|---|
| Bare-metal and FreeRTOS | [Applications](../docs/applications.md), [peripherals](../docs/peripherals.md), [console](../docs/bring-up.md) |
| Offline HOST roster | [HOST runner procedure](../docs/tools-testing.md#host-integrity-runner), [local launchpad](harness/README.md) |
| Ethernet, RAM and FTP | [Suite launcher](../scripts/ethernet/README.md), [FTP contracts](../docs/ftp.md) |
| Boot continuity, resources and owner gates | [Startup](../docs/boot-startup.md), [memory layout](../docs/boot-memory-layout.md), [focused gates](../docs/tools-testing.md#focused-boot-owner-gates) |
| Public fixtures and source identity | [Verifier fixtures](../docs/tools-testing.md#public-only-signed-verifier-fixtures), [fingerprints](../docs/tools-testing.md#public-source-fingerprints) |

List offline Ethernet coverage without running it:

```sh
./scripts/ethernet/test.sh --list
```

For exact boot selections, read each runner's `CASES` and the canonical owner-gate
procedure. HOST and Ethernet rosters overlap, so do not run both full rosters by
default. Builds, signing, emulator runs and live device/network access are distinct
operations. Historical passes are not current proof. This documentation cleanup
changes source-identity bytes without renewing fingerprints or tests.
