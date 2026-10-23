# Slot-B double-blink LED fixture

This default-OFF, LED-only fixture uses PF0 with ON/OFF/ON/OFF-times-five phases.
Its software-loop shape is nominally 100/100/100/500 ms, not measured timing.
It has no timer, network or watchdog service and no new acceptance claim.

From the repository root, with dependencies and official Arm/newlib installed:

```sh
cmake -S . -B build/led-fixtures -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_AB_BOOT_DEMO=ON
cmake --build build/led-fixtures --target lm3s6965_boot_blink_b
```

Entry requires authenticated [startup continuity](../../../docs/boot-startup.md)
and `linker/app_b.ld`, not cold reset or debugger entry. It publishes C_READY
without feeding. A live inherited watchdog can expire. A decision-only boot
image cannot dispatch this fixture. Building does not provision metadata.

For updates, use the separate `lm3s6965_ab_ftp_b` production variant and
[FTP-enabled A/B workflow](../README.md), not this LED-only package. See
[packaging](../../../docs/boot-package.md) and
[boot images](../../../docs/boot-reset-image.md) for exact boundaries.
Programming erases flash. No physical LED/FTP or watchdog timing is established.
