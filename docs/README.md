# Documentation

These documents cover the detailed topics that the main [README](../README.md) refers to.

- [Verify the build](verifying-the-build.md) — check the ELF architecture and attributes.
- [Run without hardware (QEMU)](qemu.md) — test the firmware with no silicon attached.
- [FreeRTOS target](freertos.md) — build and run the FreeRTOS image.
- [System clock](system-clock.md) — the PLL configuration and the `rcc:` self-check.
- [Console output](console.md) — the SWO and UART console backends.
- [If it fails](troubleshooting.md) — OpenOCD error messages and their causes.
- [Memory map](memory-map.md) — the flash, SRAM, and peripheral address regions.
- [Boot verifier resources](boot-resources.md) — reviewed Arm Release footprint,
  isolated QEMU heap/stack measurements and the approved 8 KiB FTP heap;
  the historical 2 KiB OOM fixture is not production capacity or full fit acceptance.
- [Boot diagnostics and snapshot foundation](boot-diagnostics.md) — ABD1 64-byte ABI,
  ordered foreground publication, external availability and same-owner read-only
  metadata/actually-running snapshot; not full FTP status or hardware acceptance.
- [Injected boot flash adapter](boot-flash.md) — mocked-register controller,
  metadata-only store callbacks, blocked-operation recovery and manifest-first
  inactive erase steps; no physical MMIO binding or hardware acceptance.
- [Injected boot watchdog policy](boot-watchdog.md) — mocked-register protection,
  one-way boot/application ownership and single-epoch genuine foreground progress;
  no physical timing, ISR/MMIO binding or full handoff composition.
- [Logical boot handoff gate](boot-handoff.md) — structural admission, ordered
  quiescence trace and one-way watchdog ownership at terminal entry intent;
  no MSP-changing assembly, application execution or physical binding.
- [Verified candidate-launch decision](boot-launch.md) — independent image verification,
  durable READY to TRIAL and zero-write authoritative retries; no reset composition
  or physical handoff.
- [Verified boot selection](boot-select.md) — candidate-first, zero-change confirmed
  fallback, durable previous promotion and explicit no-executable recovery;
  HOST composition only, execution gate pending.
- [Bounded boot reset and selection](boot-runtime.md) — once-per-boot reset handling
  and verified selection composition; HOST decision scope, no physical handoff.
- [Protected runtime selection and launch intent](boot-runtime-watchdog.md) —
  adopt caller-established protection and service genuine verifier progress.
  Compose selection with final intent on the same watchdog and current epoch.
  HOST scope only. Physical timing and service gaps remain unmeasured.
- [HOST application service and PC workflow](boot-service.md) — coherent status,
  strict confirm parsing, durable confirmation, and scripted PC transport.
  Physical flash/startup wiring and measured scheduling remain open.
- [HOST firmware receiver](boot-receive.md) — separate bounded manifest-first
  transaction through durable invalidation, streaming, stored verification and
  READY/reset intent. No synchronous RAM backend replacement or hardware claim.
- [Opt-in FTP boot binding](ftp-boot-binding.md) — production core/raw-lwIP
  routing, exact-prefix WAIT/EOF and foreground real boot operations; RAM remains
  synchronous. No enabled updater/startup/MMIO/reset actuator or hardware claim.
- [Peripherals](peripherals.md) — the GPIO data mask and the guarded debug pins.
- [OLED display](oled.md) — the recovered 128x96 panel protocol and its memory strategies.
- [Ethernet diagnostics and evidence](ethernet.md) — distinct link/RAW targets,
  native macOS host script, exact frame contract, permissions, and readiness.
  It preserves historical host and physical link observations without claiming IP or FTP acceptance.
- [Offline FTP source and future test](ftp.md) — opt-in build, pinned lwIP, approved RAM contract,
  final resource accounting, offline packet tests, and an authorization-gated curl byte comparison.
- [Reusable FTP libraries](ftp-library.md) — public core, TCP, and RAM APIs, caller-owned storage,
  binding/reset lifetimes, CMake reuse, and dedicated pinned lwIP limits.
- [Ethernet tooling](../scripts/ethernet/README.md) — relocated tree, consolidated offline suites,
  retained coverage, compile/link-only consumer, and separate authorization-gated live launchers.
- [Toolchain](toolchain.md) — why the build uses the official Arm toolchain and its startup contract.
- [Conventions](conventions.md) — include guards, constant representation, and datasheet authority.
- [Function index](function-index.md) — the source functions and their descriptions.
