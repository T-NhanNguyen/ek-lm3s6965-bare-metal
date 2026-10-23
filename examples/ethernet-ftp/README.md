# Ethernet FTP example launchpad

The standalone RAM-backed example is opt-in. From the repository root, with
pinned sources populated and the official Arm/newlib toolchain installed:

```sh
cmake -S . -B build/ftp -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_ETHERNET_FTP=ON
cmake --build build/ftp --target lm3s6965_ethernet_ftp
```

See [FTP libraries and platform contracts](../../docs/ftp.md) for reusable APIs,
storage lifetime, TCP ownership, architecture headers, capacity and authorized
round-trip instructions. [Tools and validation](../../docs/tools-testing.md)
contains real lwIP memory-peer and stack procedures.

The slot-linked `lm3s6965_ethernet_ftp_foreground` uses authenticated inherited
[startup continuity](../../docs/boot-startup.md), not cold-reset/debugger entry.
It remains RAM-backed and is not the production flash updater. Physical service,
watchdog timing and runtime memory peaks are not established by static fit.

For production firmware/confirmation, use the separate
[A/B demo](../ab-boot-demo/README.md) and
[PC workflow](../../docs/boot-reset-image.md#pc-demo-and-transport).
They require an already confirmed production application and matching root,
not a blank board or LED-only image. Programming erases flash. No build,
signing, hardware or installed-image result is claimed by this launchpad.
