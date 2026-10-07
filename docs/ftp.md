# Offline FTP source and future physical test

The separate bare-metal FTP source is complete and tested offline.
Physical MCU IP communication and FTP transfers remain unverified.
The installed board image was last observed as the RAW diagnostic, not FTP.
Do not infer a physical result from a build or a simulated packet exchange.

## Opt-in build

`LM3S6965_BUILD_ETHERNET_FTP` defaults to `OFF`.
Existing bare-metal, FreeRTOS, link, and RAW targets do not depend on lwIP.
The separate executable is `lm3s6965_ethernet_ftp`.
CMake does not fetch dependencies. Use the official Arm GNU Toolchain with newlib.
The final offline verification used Arm GNU Toolchain 15.3.rel1.
See [toolchain setup](../README.md#installing-the-arm-toolchain).

For a later authorized dependency initialization and offline build, run from the repository root:

```sh
git submodule update --init third_party/lwip
git -C third_party/lwip rev-parse HEAD
cmake -S . -B build/ftp -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_ETHERNET_FTP=ON
cmake --build build/ftp --target lm3s6965_ethernet_ftp --parallel 4
```

Initialization can download sources. It was not run during this documentation update.
The required lwIP 2.2.1 pin is `77dcd25a72509eb83f72b033d219b1d40cd8eb95`, tag `STABLE-2_2_1_RELEASE`.
The gitlink pins the commit. Do not update to a moving branch or a different release.
The ELF is `build/ftp/examples/ethernet-ftp/lm3s6965_ethernet_ftp`.
Its directory also contains `.bin`, `.hex`, and `.map` files.
Building does not flash or test the board.

## Source layout and reuse

The reusable implementation lives in `src/ftp_core.c`, `src/ftp_tcp.c`, and `src/ram_file.c`.
Public headers live under `include/lm3s6965/`, including the storage callback interface.
The example owns `hello`, its address, and both 4096-byte buffers in `examples/ethernet-ftp/main.c`.
Core binding and first transport start preserve data. This example explicitly clears storage at cold boot.
Link/session cleanup retains the file.
See the [reusable library guide](ftp-library.md) for targets, caller lwIP configuration, storage contracts, and lifetimes.

## Approved service contract

This is a local test service, not a production FTP server.
FTP commands, passwords, and data are plaintext. Anonymous login provides no authentication or confidentiality.
Use an isolated direct cable. Do not expose the service to an untrusted network.

- One control client and one passive data connection are allowed.
  The data peer must have the control client's IPv4 address.
- Login uses `USER anonymous`, then `PASS`. The password can be empty and has no security function.
- Select binary transfer with `TYPE I` before upload or download.
- Only `hello` and `/hello` are accepted file names.
  Upload at most 4096 bytes. An empty upload is allowed.
- Upload bytes enter staging RAM. Orderly receive EOF alone does not commit them.
  Successful server FIN enqueue and safe PCB release commit the replacement and queue `226`.
  This does not wait for the remote FIN ACK.
- Overflow, RST, timeout, ABOR, QUIT, control loss, or link loss before accepted close discard staging.
  A failed upload retains the previous committed file.
- Download completion requires all file bytes to be enqueued and acknowledged, then successful FIN enqueue.
  Login, a `150` reply, or a driver submission alone does not prove completion.
- Reset clears the file. A successful empty upload creates an existing zero-length file.
  There is no filesystem, flash storage, runtime flash write, or firmware update path.

Supported commands: `USER`, `PASS`, `SYST`, `FEAT`, `PWD`, `XPWD`, `CWD /`, `CDUP`,
`TYPE I`, `EPSV`, `PASV`, `STOR`, `RETR`, `SIZE`, `NOOP`, `ABOR`, and `QUIT`.
Each transfer needs a new EPSV or PASV setup.
`LIST`, other directory listings, active `PORT`/`EPRT`, ASCII `TYPE A`, and other paths are unsupported.
ABOR reports `426` and `225` during a transfer, or `225` while idle. It does not report successful `226`.
QUIT discards staging, sends `221`, and closes after bounded output/close retries.
See the [core API](../examples/ethernet-ftp/FTP_CORE_API.md) for command ordering and deadlines.

## Direct-link settings and evidence

The selected topology is Mac USB Ethernet adapter directly connected to the MCU.
Earlier host observations identify `en7`, active `100baseTX` full duplex, and Mac `192.168.7.1/24`.
The firmware selects MCU `192.168.7.2/24`, gateway zero, and no DNS.
Its local test MAC is `02:00:00:69:65:02`.
These settings and observed link negotiation do not prove physical MCU IP or FTP operation.
The observations were not repeated in this documentation update.
See [Ethernet evidence](ethernet.md) for the historical host, link, and RAW boundaries.

## Future client round trip: separate authorization required

**Start this procedure ONLY after separately authorized flashing and network testing.**
Confirm the FTP image, direct-link settings, address uniqueness, and network readiness first.
This page does not authorize flashing, reset, host configuration changes, or traffic.
Do not use RAW/link reset helpers against the FTP image.

The local documentation check ran `curl --version` without network access on 2026-10-07.
Installed curl 8.7.1 lists `ftp` in `Protocols`. No client was installed.
Recheck `curl --version` on the test host and stop if `ftp` is absent.
No working standalone `ftp` CLI syntax is claimed.

Use an existing local `hello` file. Run the following only in the separately authorized physical test.
The download goes into a new temporary directory, never over the input file.
`--disable` ignores curl configuration. `--noproxy '*'` prevents proxy use.
Passive mode is explicit. curl uses binary transfer by default for these URLs.
EPSV is attempted first, with PASV fallback. No LIST command is needed.

```sh
(
  set -eu
  input="$PWD/hello"
  test -f "$input"
  bytes=$(wc -c < "$input" | tr -d '[:space:]')
  test "$bytes" -le 4096
  out=$(mktemp -d "${TMPDIR:-/tmp}/lm3s6965-ftp.XXXXXX")
  printf 'Evidence directory: %s\n' "$out"
  cp "$input" "$out/upload.bin"
  curl --disable --noproxy '*' --fail --show-error --verbose --connect-timeout 5 --max-time 60 --ftp-pasv --user 'anonymous:' --upload-file "$out/upload.bin" 'ftp://192.168.7.2/hello' 2> "$out/upload.log"
  curl --disable --noproxy '*' --fail --show-error --verbose --connect-timeout 5 --max-time 60 --ftp-pasv --user 'anonymous:' --output "$out/download.bin" 'ftp://192.168.7.2/hello' 2> "$out/download.log"
  wc -c "$out/upload.bin" "$out/download.bin"
  cmp "$out/upload.bin" "$out/download.bin"
  printf 'Byte comparison passed. Review both logs for final 226 replies.\n'
)
```

Require both curl commands to exit zero, final `226` replies in both logs, and an exact byte comparison.
Retain the logs, image identity, byte counts, and test conditions.
A future reset test must separately verify that the file is absent, then upload again.
Failed-upload retention, empty files, maximum size, and cable-cycle recovery need authorized physical evidence too.

## Offline validation and resource boundary

Final independent verification built FTP ON and all four existing targets OFF from fresh snapshots.
The OFF snapshot physically omitted lwIP. All builds passed.
All five native suites passed with ASan and UBSan (six executables, including the separate heap test):

```sh
./scripts/ethernet/test.sh file core tcp platform lwip
```

The external consumer also strictly compiled and linked through the consumer fixture (current command: `./scripts/ethernet/test.sh consumer`).
It supplied a different caller-owned lwIP configuration and 113-byte RAM buffers, using only `src/` and `include/`.
This was a link check, not consumer runtime or physical network validation.
The OFF snapshot also omitted the FTP example and built the opt-in core/RAM archives.
These are recorded prior validation results, not commands run in this documentation update.
The commands above are current equivalents of the six removed wrappers.
Only duplicated build/run plumbing was removed. Core, mock TCP, real stack, RAM, platform, and consumer coverage remains.
Use `./scripts/ethernet/test.sh ftp file platform` for the FTP-related checks.
Default or `all` also runs driver, raw offline, and link fixtures.
Latest independent cleanup verification passed ten native ASan/UBSan executables, three CLI tests, and 41 link Python tests.
Consumer coverage remains compile/link only. Cleanup adds no new firmware or hardware evidence.
See the [tooling guide](../scripts/ethernet/README.md) for safety and prerequisites.
The real lwIP suite uses actual Ethernet ARP and checksummed IPv4/TCP packets in memory, not sockets or a cable.
It covers EPSV/PASV, binary 0/17/4096-byte round trips, overflow/RST preservation, ACK lag, retransmission, and teardown.
It also covers actual TCP_SEG exhaustion during FIN allocation and QUIT input/output pressure.
Darwin ASan leak detection is disabled because the runtime does not support it.
Bounds/use-after-free checks and UBSan remain enabled. Explicit heap/pool checks verify quiescent teardown.
See [real lwIP validation](../scripts/ethernet/ftp/ftp-lwip-validation.md) and [transport evidence](../examples/ethernet-ftp/TRANSPORT_VALIDATION.md).

Final Arm Release accounting:

| Allocation | Bytes |
|---|---:|
| GNU size text (`.text + .ARM.exidx`) | 27604 |
| Flash load total (text + data) | 27612 |
| `.data` | 8 |
| Actual static `.bss` | 39976 |
| Static SRAM (`.data + .bss`) | 39984 |
| C heap reservation | 2048 |
| Stack reservation | 8192 |
| Static SRAM plus reservations | 50224 |
| Unassigned heap-to-stack gap | 15312 |

The SRAM equation is `39984 + 2048 + 8192 + 15312 = 65536`.
lwIP heap, pools, file buffers, and packet scratch are already in static BSS. Do not add them again.
GNU `size` reports BSS 50216 because it includes the NOLOAD heap/stack reservations.
That figure is not actual static BSS. Its combined `dec` is not flash usage.
The final ELF vector resolves to the strong FTP SysTick handler.
No FreeRTOS heap, console, OLED, or libc allocator implementation is retained.

The interrupt tick counter and RCC configuration checks are implemented and tested offline.
Runtime stack high-water, peak live heap/pools, worst-case service gaps, physical clock frequency,
and deliberate cable-cycle recovery remain pending. Link fit is not measured runtime safety.

## Pinned private lwIP contracts

lwIP 2.2.1 can return `ERR_OK` with private `TF_CLOSEPEND` when FIN allocation fails.
The adapter checks and clears that flag, retains ownership, and retries within the original close deadline.
It also uses private `tcp_process_refused_data` to drain QUIT input before graceful close.
Upgrading lwIP requires review of both contracts, close states, callbacks, and ownership tests.
The compile-time version guard is not a substitute for that review.
The dedicated link-down PCB sweep assumes there are no other TCP services sharing this stack.
