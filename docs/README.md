# Documentation

These documents cover the detailed topics that the main [README](../README.md) refers to.

- [Verify the build](verifying-the-build.md) — check the ELF architecture and attributes.
- [Run without hardware (QEMU)](qemu.md) — test the firmware with no silicon attached.
- [FreeRTOS target](freertos.md) — build and run the FreeRTOS image.
- [System clock](system-clock.md) — the PLL configuration and the `rcc:` self-check.
- [Console output](console.md) — the SWO and UART console backends.
- [If it fails](troubleshooting.md) — OpenOCD error messages and their causes.
- [Memory map](memory-map.md) — the flash, SRAM, and peripheral address regions.
- [Peripherals](peripherals.md) — the GPIO data mask and the guarded debug pins.
- [OLED display](oled.md) — the recovered 128x96 panel protocol and its memory strategies.
- [Toolchain](toolchain.md) — why the build uses the official Arm toolchain and its startup contract.
- [Conventions](conventions.md) — include guards, constant representation, and datasheet authority.
- [Function index](function-index.md) — the source functions and their descriptions.
