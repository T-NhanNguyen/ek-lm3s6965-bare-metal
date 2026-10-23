# A/B FTP demo launchpad

Use [root build/flash commands](../../README.md#ab-boot-build-and-flash),
[identity and wrapper configuration](../../docs/boot-package.md#local-identity-and-build-wrappers)
and [boot images / PC workflow](../../docs/boot-reset-image.md).
Run from the repository root with separately authorized signing access:

```sh
scripts/build_ab_boot_demo
scripts/build_boot_dispatch
```

The first wrapper reads complete literal root `.env`, builds FTP A/B, forces
diagnostics OFF and signs a non-overwriting release pair. The second builds only
the dispatch bootloader. Neither flashes or provisions initial metadata.
Direct presets produce unsigned applications. New keys require a matching device
bootloader root. FTP cannot rotate the root or bootstrap a blank board.

## Choose FTP-enabled packages, not the LED-only fixtures

Only an existing durably confirmed production FTP app with a permitted update
slot can serve this client. Supply exactly one signed A and one signed B package
at the top level of a directory, on an approved isolated bench:

```sh
scripts/ab-boot-firmwareflash-ftp.sh status
scripts/ab-boot-firmwareflash-ftp.sh download /path/to/signed-ftp-packages
```

Reception erases the inactive destination. Follow the manual-reset prompt.
Success requires matching healthy execution/report and durable confirmation,
not a final transfer reply. Never retry firmware after an ambiguous transfer.
Plain FTP does not authenticate the server. This launchpad claims no new result.

[Slot A](slot-a-breathing/README.md) breathes and
[slot B](slot-b-blink/README.md) double-blinks. Their LED-only targets do not serve
FTP or provide production watchdog service. Recovery via
`scripts/ab-boot-provision-recovery.sh` writes one slot only and preserves root
and metadata. It is not complete blank-board provisioning or automatic fallback.
