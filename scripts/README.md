# Script launchpad

Run from the repository root. See the [manual](../docs/README.md) for contracts.
Commands are instructions, not evidence or permission for device/key access.

| Outcome | Canonical procedure |
|---|---|
| Install pinned sources | [Toolchain and dependencies](../docs/toolchain.md) |
| Build/sign A/B and configure literal local paths | [Identity and wrappers](../docs/boot-package.md#local-identity-and-build-wrappers) |
| Build dispatch or update a confirmed app | [Boot images and PC workflow](../docs/boot-reset-image.md) |
| Flash, probe or capture console | [Board bring-up](../docs/bring-up.md) |
| Source fingerprints and optional tool hashes | [Tools and validation](../docs/tools-testing.md#public-source-fingerprints) |

Source-only manifest, without builds, network, keys or tools:

```sh
python3 scripts/boot_fingerprints.py --output /tmp/boot-fingerprints.json
```

This identifies selected bytes, not test success. Documentation changes invalidate
prior source identities without renewing proof. Required chapter paths remain.

[Ethernet suites](ethernet/README.md) are offline by default. Separate RAW defaults
to live traffic and link capture resets the board. Consult
[Ethernet diagnostics](ethernet-diagnostics.md) before either live path.
Programming erases flash. FTP requires an existing confirmed production app,
not a blank board. Recovery is one-slot only, not complete provisioning.
