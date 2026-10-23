# Manual

Start with the outcome you need. Each chapter gives the mental model first, then maps procedures and reference contracts to source files. Commands assume the repository root unless stated otherwise. Build and model checks do not grant permission for hardware, network, key access or provisioning.

## Build and understand the board

| Chapter | Outcome |
|---|---|
| [Toolchain and dependencies](toolchain.md) | Select a complete Arm toolchain and populate exact dependency pins explicitly. |
| [Board bring-up and console](bring-up.md) | Capture startup text and diagnose the correct failure layer. |
| [BSP registers and conventions](bsp.md) | Find the memory map, header rules and function reference. |
| [Clocks, GPIO and OLED](peripherals.md) | Configure clock consumers, protected pins and write-only display rendering. |
| [Applications and emulation](applications.md) | Integrate FreeRTOS and interpret QEMU evidence within its model. |

## Authenticate, enter and update applications

| Chapter | Outcome |
|---|---|
| [Boot architecture and diagnostic ABI](boot-architecture.md) | Follow reset accounting, selection, candidate transactions, handoff and coherent diagnostics. |
| [Boot memory layout and resource validation](boot-memory-layout.md) | Enforce flash ownership, retained SRAM, allocator bounds and final-artifact checks. |
| [Signed packages and trust roots](boot-package.md) | Produce exact slot artifacts, preserve key custody and generate offline factory metadata. |
| [Startup continuity and watchdog ownership](boot-startup.md) | Preserve one watchdog and retained provenance through application entry and clock-consumer transition. |
| [Firmware update and PC workflow](boot-reset-image.md) | Separate durable reception, reset, observed execution and explicit confirmation. |

## Integrate and validate

| Chapter | Outcome |
|---|---|
| [FTP libraries and platform contracts](ftp.md) | Reuse core/TCP/RAM APIs and bind asynchronous boot operations without weakening storage or transport ownership. |
| [Tools and validation](tools-testing.md) | Choose static, native, memory-peer and stack-instrumentation checks by their evidence boundary. |
| [Why this works and where proof stops](limitations.md) | Distinguish architectural stability, model evidence and physical acceptance. |

Operational Ethernet procedures live in [scripts/ethernet-diagnostics.md](../scripts/ethernet-diagnostics.md). That guide retains the separate RAW and link identities, frame contracts, diagnostic commands and reset/traffic warnings. The [script launchpad](../scripts/README.md), [test launchpad](../tests/README.md) and example READMEs describe their local entry points.

Dated progress, installed-image observations and fixture results are not current manual contracts. A procedure here does not identify the image currently on the board. Revalidate identity and obtain separate approval before live operations.
