# Slot-A breathing LED fixture

This default-OFF, LED-only fixture uses PF0 and a bounded software PWM envelope.
It has no timer, network or watchdog service. Physical polarity, frequency and
sustained visible behavior are not accepted by a successful build.

From the repository root, with dependencies and official Arm/newlib installed:

```sh
cmake -S . -B build/led-fixtures -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_AB_BOOT_DEMO=ON
cmake --build build/led-fixtures --target lm3s6965_boot_breathing_led
```

Entry requires authenticated [startup continuity](../../../docs/boot-startup.md)
and `linker/app_a.ld`, not cold reset or debugger entry. It publishes C_READY
without feeding. A live inherited watchdog can expire. A decision-only boot
image cannot dispatch this fixture. Building does not provision metadata.

For updates, use the separate `lm3s6965_ab_ftp_a` production variant and
[FTP-enabled A/B workflow](../README.md). LED-only packages do not serve FTP.
See [packaging](../../../docs/boot-package.md) and
[boot images](../../../docs/boot-reset-image.md) for exact boundaries.
Programming erases flash. This launchpad establishes no new evidence or authority.
