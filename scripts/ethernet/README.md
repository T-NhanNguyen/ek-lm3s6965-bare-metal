# Ethernet tooling

Run commands from the repository root. The root runner is safe by default.
It performs offline tests only. It does not download dependencies or access hardware.
Missing prerequisites fail instead of silently skipping coverage.

```sh
./scripts/ethernet/test.sh
./scripts/ethernet/test.sh all
./scripts/ethernet/test.sh ftp file platform
./scripts/ethernet/test.sh consumer
./scripts/ethernet/test.sh driver raw link
./scripts/ethernet/test.sh --help
./scripts/ethernet/test.sh --list
```

Default and `all` select `core tcp lwip file platform consumer driver raw link`.
`ftp` selects `core tcp lwip consumer`. Combine named suites as needed.
Overlapping selections run once, in first-requested order.
Use `--help` and `--list` alone. Invalid arguments fail before build setup.

## Tree and responsibilities

```text
scripts/ethernet/
  test.sh                         Consolidated offline runner
  ftp/
    ftp-lwip-validation.md         Real-stack evidence and limits
    tests/                        Core, mock TCP, real lwIP fixtures
      ftp-library-consumer/       External caller and caller-owned config
  file/tests/                     RAM file fixture
  platform/
    tests/                        Driver, clock/netif, heap, and runner CLI fixtures
    shims/                        Shared native MMIO and lwIP configuration shims
  raw/
    ethernet-raw-test.sh           Separate build/self-test/LIVE launcher
    host/                         Real native Mac executable sources
    tests/                        Protocol, BPF-record, and privilege fixtures
  link/
    ethernet-link-test.sh         Separate saved-text/live capture launcher
    ethernet_link_test.py          Standard-library evaluator
    tests/                        Evaluator and shared ICDI capture fixtures
```

`raw/host/` contains the real Mac helper main, capture, and privilege implementation.
These are executable sources, not MCU sources or test fixtures.
The MCU diagnostic and protocol remain in `examples/ethernet-raw/`.
Shared ICDI fixtures remain under `link/tests/`, despite their console responsibility.
The shared implementation remains `scripts/icdi-console.sh`. It was not relocated.
Example lwIP port headers remain in `examples/ethernet-ftp/arch/`.
Consumers own their port configuration. See the [platform contract](../../examples/ethernet-ftp/PLATFORM_CONTRACT.md#lwip-architecture-port).

| Suite | Retained coverage |
|---|---|
| `core` | Protocol parser, actions, deadlines, storage failures, and binding/reset |
| `tcp` | Production adapter with mock raw TCP API and fault injection |
| `lwip` | Real pinned stack with checksummed ARP/IPv4/TCP packets in memory |
| `file` | RAM names, ranges, upload replacement, aliases, and independent instances |
| `platform` | Clock/wrap, PHY cadence, netif ownership, teardown; separate heap executable; three CLI tests |
| `consumer` | Strict Arm compile/link with custom caller config and src/include-only BSP copy; no ELF execution |
| `driver` | Register/FIFO model, TX/RX, drain/reset, W1C, arguments, and MII |
| `raw` | Three native offline fixtures: mocked privilege syscalls, native BPF records, protocol bytes |
| `link` | 41 Python evaluator/lifecycle tests with mocked capture/hardware and saved-text shell entry |

Six wrappers were removed only because they duplicated compiler/build/run plumbing.
They were core, mock TCP, real lwIP, RAM file, platform, and external consumer wrappers.
Their unique tests remain. No core/mock TCP/real-stack/RAM/platform/consumer coverage was discarded.

Native fixtures use ASan and UBSan. Darwin's unsupported LeakSanitizer is disabled.
Explicit lwIP heap/pool checks remain. Raw requires the installed macOS SDK and a nonroot user.
Consumer requires the installed Arm toolchain with newlib. Stack suites require populated pinned lwIP.
No automatic install, fetch, privilege change, or host configuration occurs.

## Separate launchers: authorization boundary

**The raw launcher defaults to LIVE traffic**, including when called without arguments.
The consolidated runner invokes it only with literal `--self-test`.
These explicit modes are safe offline:

```sh
./scripts/ethernet/raw/ethernet-raw-test.sh --build-only
./scripts/ethernet/raw/ethernet-raw-test.sh --self-test
./scripts/ethernet/link/ethernet-link-test.sh --capture-file saved-capture.txt
```

`--build-only` compiles the real Mac helper without executing it or opening BPF.
`--self-test` executes offline fixtures without packet traffic or device access.
Link saved-text evaluation does not establish live provenance or original capture status.
Link live capture resets the board through the shared console helper.
Raw live operation opens BPF and sends frames. These live paths are excluded from `test.sh`.
Both require fresh authorization. Earlier approvals do not authorize a new live operation.
See [Ethernet evidence and authorization-gated operations](../../docs/ethernet.md).
Do not use the link launcher against RAW or FTP images.

## Latest independent verification

Cleanup verification passed ten native ASan/UBSan executables, three CLI tests, and 41 link Python tests.
The external consumer compiled and linked only. The real Mac helper also compiled with `--build-only` and was not executed.
No new firmware build, hardware, physical link, packet, IP, or FTP result was established.
Earlier firmware evidence and resource figures remain separate historical results.
See [FTP evidence](../../docs/ftp.md) and [real lwIP validation](ftp/ftp-lwip-validation.md).
