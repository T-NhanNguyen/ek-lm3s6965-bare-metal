# Ethernet suite launchpad

Run from the repository root. Default and `all` are offline, with no device,
network, dependency fetch or automatic privilege/configuration changes.

```sh
./scripts/ethernet/test.sh --list
./scripts/ethernet/test.sh
./scripts/ethernet/test.sh all
./scripts/ethernet/test.sh ftp file platform
./scripts/ethernet/test.sh consumer
```

Default/`all` select `core tcp lwip file platform consumer driver raw link`.
`ftp` aliases `core tcp lwip consumer`. Overlapping requests run once in first
requested order. Use `--help` and `--list` alone. Invalid arguments fail before
build setup. Missing prerequisites fail rather than skip. `consumer` is Arm
compile/link only, not execution.

| Suite | Boundary |
|---|---|
| `core`, `tcp`, `lwip` | Parser, fake raw TCP and real-stack memory peer |
| `file`, `platform` | RAM replacement, clock/netif, heap, CLI and stack scanner |
| `consumer`, `driver` | Caller-owned configuration link and register/FIFO model |
| `raw`, `link` | Native BPF/protocol/privilege fixtures and saved-text evaluator |

Native tests use ASan/UBSan. Raw requires the installed macOS SDK and a nonroot
user. Consumer requires an installed Arm/newlib toolchain. lwIP suites require
populated pinned sources. See [tools and testing](../../docs/tools-testing.md)
for scenarios, prerequisites, stack measurement and evidence limits, and
[FTP contracts](../../docs/ftp.md) for integration ownership.

**Separate RAW defaults to LIVE traffic. Link live capture resets the board.**
The runner uses only offline modes. Neither live path is included or authorized.
See [Ethernet diagnostics](../ethernet-diagnostics.md) for exact RAW/link frame,
image-identity and operational procedures. Never use the link launcher on RAW/FTP.
