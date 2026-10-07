# Ethernet reference and evidence

This note records bare-metal Ethernet diagnostics and their evidence limits.
It is not a complete Ethernet study guide.
Physical initialization and successful link negotiation were observed
on 2026-10-05. The installed board application is now the RAW diagnostic.
The user reported elevated helper failures before the frame experiment.
The corrected helper's elevated retry and raw-frame exchange remain unverified.
Bidirectional frame exchange, IP, FTP, and cable-cycle recovery remain untested.

## Distinct diagnostic targets

| Target | Purpose | Board MAC |
|---|---|---|
| `lm3s6965_ethernet_link` | PHY samples, no application TX or RX drain | `02:00:00:69:65:01` |
| `lm3s6965_ethernet_raw` | Bounded request/reply frame experiment | `02:00:00:69:65:02` |

Both targets preserve the existing bare-metal and FreeRTOS examples.
They are not interchangeable. The link evaluator requires the link identity.
Do not use `scripts/ethernet-link-test.sh` with the RAW image.
It resets the board and then rejects the RAW identity.

## RAW frame experiment

The separate source is `examples/ethernet-raw`. Build without hardware access:

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
cmake --build build --target lm3s6965_ethernet_raw
scripts/ethernet-raw-test.sh --build-only
scripts/ethernet-raw-test.sh --self-test
```

The board ELF is `build/examples/ethernet-raw/lm3s6965_ethernet_raw`.
Its directory also contains `.bin`, `.hex`, and `.map` files.
The host helper is `build/host/ethernet_raw_test`.
The script uses native macOS SDK headers and Clang through `xcrun`.
`--self-test` runs protocol and BPF capture tests with address and
undefined-behavior sanitizers. It opens no BPF device and sends nothing.
`--build-only` compiles without BPF access. Compile as a regular user.
The script refuses root compilation. It does not install dependencies.

### Exact frame contract and bounds

Each request and reply is exactly 60 bytes, from destination address through
payload. These bytes exclude preamble and FCS. No extra padding is requested.
EtherType `0x88B5` is a chosen local experiment value.
No verified IEEE assignment or standards claim is made.
The fresh RAW MAC `02:00:00:69:65:02` is locally administered and unicast.
It is test-only, not an assigned hardware or production identity.
Check for address conflicts before using this experiment on another segment.

| Byte offsets | Content |
|---|---|
| 0–5 | Destination MAC: board for request, host for reply |
| 6–11 | Source MAC: host for request, board for reply |
| 12–13 | EtherType `88 B5` |
| 14–21 | ASCII `LM3SRAW1` |
| 22 | Version `01` |
| 23 | Opcode `01` request, `02` reply |
| 24–27 | Big-endian 32-bit sequence |
| 28–35 | Eight-byte nonce |
| 36–59 | Exact payload pattern `00` through `17` hexadecimal |

The host reads the selected interface's actual MAC and chooses one random
nonce per run. It sends three sequential requests, sequences 1, 2, and 3.
Each exchange has a 3000 ms deadline. The reply swaps addresses and changes
only the opcode. The host requires the full exact incoming 60-byte reply,
including source, destination, nonce, sequence, and payload.
The BPF filter checks all 60 bytes. The parser also requires captured and
original lengths of 60, valid records, and the outstanding nonce and sequence.
Outgoing capture is disabled where the native ABI supports that control.
There is no authentication or replay protection.

The board owns one pending reply buffer. It accepts at most 32 requests per
boot, including duplicates and requests whose later submission fails.
Requests received while pending or after that budget become admission drops.
It attempts submission at most eight times, once per observed 1 ms tick.
Eight BUSY results exhaust the reply. Other submission errors release it
immediately. Each foreground pass drains at most eight RX frames, even while
pending or after the acceptance budget is spent. Invalid requests are silent.
Sustained traffic can still overrun hardware.
SysTick COUNTFLAG can coalesce ticks. Console output and bounded driver waits
extend wall-clock time. This is not a hard real-time service or network stack.

`raw reply submitted ... (not delivered)` records driver submission only.
The `submitted` counter does not prove wire delivery or host reception.

### Explicit host permission and later user commands

Native macOS BPF needs read/write access to one available BPF device.
The observed devices were root-owned, mode `0600`. Nonroot access checks
were false. The read-only check opened no BPF device.
This is the current live-test permission blocker, not a build blocker.
Do not apply broad `chmod` access or add automatic `sudo`.
The script and helper change no host network settings or interface flags.
They do not enable promiscuous mode or provide a permissions installer.

After approval, build as your regular user, then explicitly run the helper:

```sh
scripts/ethernet-raw-test.sh --build-only
sudo ./build/host/ethernet_raw_test en7
```

Here `en7` is the example CLI interface. Confirm your board-facing interface.
Do not run the build script with `sudo`.
The helper configures BPF, calls `setgroups(0, NULL)`, and drops to
`SUDO_UID`/`SUDO_GID` before the bounded packet loop.
Ordinary Darwin `getgroups` must then return only the target effective GID,
not an empty list.
Root invocation without valid nonzero sudo identities is refused.
The privilege drop retains the open BPF descriptor's raw-frame capability.
It is not removal of packet access. The corrected elevated path is unverified.
If narrow BPF access is already available, the regular-user command is:

```sh
scripts/ethernet-raw-test.sh en7
```

That script builds and then sends. Its default interface is also `en7`.
Use `--help` for its actual CLI. Build-only and self-test modes do not send.

### Darwin singleton correction and latest user evidence

The user supplied a later 30-second RAW console capture.
It reported `raw init=OK`, link up, and MAC readiness one.
Accepted and submitted counts stayed zero. RX drops reached six.
RX and poll error counts stayed zero. Dropped-frame source/content are unknown.
The accompanying host failure was exactly:

```text
Privilege drop: process supplementary groups not empty (1)
```

The helper stopped before the frame experiment. No raw exchange was tested.
The earlier three-drop readiness capture below remains historical evidence.
The reported count does not identify the user's actual GID.
The corrected runtime retry remains pending.

The previous empty-list requirement was wrong on Darwin, even with ordinary
`getgroups`. Apple official XNU source at pinned commit
`f6217f891ac0bb64f3d375211650a4c1ff8ca1ea` explains the singleton:

- [kern_prot.c lines 414–448][xnu-getgroups] return credential groups.
- [kern_prot.c lines 1334–1373][xnu-setgroups] retain an effective-GID slot
  for a zero-group request.
- [kern_credential.c lines 1864–1917][xnu-egid] maintain the effective GID
  in the first group slot.
- [ucred.h line 211][xnu-ucred] documents that slot as the effective GID.

Commit `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`
[corroborates these semantics][xnu-corroboration].
These references do not establish the exact running-kernel source mapping.
No network lookup ran for this documentation update.

The current verifier requires `getgroups(0, NULL)` to return one.
It then requires `getgroups(1, &actual_gid)` to return one and the fetched
GID to equal the validated target `SUDO_GID`.
It rejects extra groups, count/fetch failures, and a wrong singleton GID.
Real/effective identity checks and `setuid(0)` failure with `EPERM` remain.
Rebuild as a regular user, then run the explicit sudo binary shown above.
No flash is needed. The optional 30-second console command resets RAW.
Wait for init and negotiated readiness before starting the helper.

### Native BPF privilege-drop troubleshooting (earlier correction)

The singleton correction above supersedes the earlier empty-list assumption.

The user ran the old helper with sudo and reported a generic privilege-drop
error. That message does not identify the actual failing operation.
The original runtime failure remains undiagnosed.
Offline inspection verified a separate compile-time API mismatch:
`_DARWIN_C_SOURCE` selects Darwin's extended `getgroups` API.
That API reports account-default groups, not ordinary process supplementary
groups. It cannot verify the process credential after `setgroups(0, NULL)`.

The correction is isolated in `tools/ethernet_raw_privilege.c`.
It undefines `_DARWIN_C_SOURCE` and `_DARWIN_UNLIMITED_GETGROUPS` before
any headers. Other translation units retain native Darwin BPF declarations.
The helper retains nonzero sudo identity validation, group clearing, real
and effective UID/GID checks, and the irreversible-drop check.
It requires `setuid(0)` to fail with `EPERM` before the packet loop.
New errors identify the failing operation or verification condition.

The helper was rebuilt offline. Mocked privilege tests, protocol/capture
sanitizer tests, and native symbol checks passed. The helper references
ordinary `_getgroups`, not `_getgroups$DARWIN_EXTSN`.
Independent review reported no blockers. These checks do not prove an
elevated run, live BPF behavior, or raw-frame exchange.

Retry with the two commands above, compiling as your regular user.
If it fails, paste the precise new operation-specific error and full output.
No board flash is needed for this host-helper-only correction.
The installed RAW image and prior readiness observations remain unchanged.

### Optional two-terminal evidence capture

These are later user operations, not offline validation. Obtain approval.
The RAW image is already installed. If installation is needed on another
board, flash only the explicit RAW ELF after separate approval:

```sh
scripts/flash.sh build/examples/ethernet-raw/lm3s6965_ethernet_raw
```

First build the host helper without elevation. Then, in terminal 1:

```sh
scripts/icdi-console.sh --seconds 30 --baud 1000000
```

This resets the installed RAW image. Keep the probe attached throughout.
Wait for `raw init=OK`, then a healthy status line with `poll=OK`,
`link=up`, `negotiation=complete`, and `mac_ready=1`.
During that capture window, in terminal 2:

```sh
sudo ./build/host/ethernet_raw_test en7
```

Do not reset mid-exchange or start the link-test wrapper.
Preserve the entire console capture and host output, including warnings.
Board request/nonce records and submission records are companion evidence.
Only matching incoming replies establish this host experiment's acceptance.
`CONSOLE CAPTURE OK` still proves startup-marker reception only.

### Actual host output and acceptance

The following template comes from `tools/ethernet_raw_test.c`, not a live run.
`<interface>` and `<16 uppercase hex digits>` denote runtime values.
After setup and privilege drop, the helper emits:

```text
raw interface=<interface> requests=3 bytes=60 (DA through payload, no FCS) timeout_ms=3000 nonce=<16 uppercase hex digits>
raw reply matched=1/3 seq=1 bytes=60
raw reply matched=2/3 seq=2 bytes=60
raw reply matched=3/3 seq=3 bytes=60
RAW FRAME PASS: 3/3 bidirectional exact 60-byte exchanges.
Not proof of other lengths, padding, full MTU, IP or FTP.
```

PASS returns exit status 0 only after all three exact exchanges.
An exchange failure returns 1 and emits this template to stderr:

```text
RAW FRAME FAIL matched=<completed>/3 seq=<failed sequence> (timeout or I/O/validation failure)
```

Setup failures return 1 with their own diagnostic, possibly before the banner.
They do not necessarily emit `RAW FRAME FAIL`. CLI errors return 2.
No host PASS, packet-transfer acceptance, IP, FTP, or full-MTU result exists yet.

### Physical readiness observed: 2026-10-05

The approved RAW flash reported `Verified OK` and reset the target.
The session records this flashed ELF SHA256:

```text
8f972c99cd766d4f92af51dcdb1391e4288902f1bdfeb372fc5f724d81b9c7b9
```

The console identifies `LM3S6965 Ethernet RAW frame diagnostic`.
PLL locked at 50 MHz and `raw init=OK` was observed.
Status changed from down/pending to up/complete with `mac_ready=1`.
The read-only host companion observed active `100baseTX`, full duplex.
The exact adapter model remains unknown.
All observed RAW counters had `accepted=0`, `submitted=0`, and `pending=0`.
Poll, RX, and TX error counters were zero. `rx_dropped` reached three.
The dropped frames' source and content are unknown.
Do not call them matching requests or known unrelated traffic.
This is initialization and negotiated readiness, not successful raw exchange.
The prior readiness capture included no elevated helper or live packet test.
The later user-reported helper failure does not establish packet acceptance.

Protocol/capture sanitizer tests and all four firmware builds passed in the
implementation checks. Independent review reported no blockers.
Those checks do not remove the BPF permission blocker or prove wire transfer.

## Bare-metal link diagnostic

The separate `lm3s6965_ethernet_link` target is in `examples/ethernet-link`.
It uses the BSP startup code and the bare-metal link settings.
It does not change the OLED, switch, or FreeRTOS applications.
The initial physical run observed initialization, but no link up.
The repeat run below observed successful physical negotiation.
A successful build is not a physical link result.

### Build only

Run these commands from the repository root. They do not access hardware.

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake
cmake --build build --target lm3s6965_ethernet_link
```

The ELF is `build/examples/ethernet-link/lm3s6965_ethernet_link`.
The same directory contains `.bin`, `.hex`, and `.map` files.
To build the existing applications too, use:

```sh
cmake --build build --target lm3s6965_firmware lm3s6965_freertos_firmware
```

### Later physical operations: approval required

Do not run these commands as part of build validation.
Obtain approval before flash, reset, capture, or cable operations.
Flash overwrites the current firmware. Capture resets the target.
The user approved the flash, reset, and SWO capture recorded below.
This approval did not include host network settings changes.

After approval, flash the explicit diagnostic path:

```sh
scripts/flash.sh build/examples/ethernet-link/lm3s6965_ethernet_link
```

With the diagnostic already flashed, run the link check:

```sh
scripts/ethernet-link-test.sh --seconds 10 --baud 1000000
```

The live command resets the target through `scripts/icdi-console.sh`.
It does not flash, change host network settings, or transmit frames.
The diagnostic uses PHY link signaling, not IP configuration.
The evaluator needs `python3` on `PATH` and uses the standard library.
Live capture also needs `openocd` on `PATH` and Python with `pyftdi`.
The helper tries `PYFTDI_PYTHON`, repository `.venv/bin/python`, then
`python3`. See [console setup](console.md) for capture requirements.

The capture script starts the reader before reset. It keeps the SWD probe
attached during capture. The board CPLD needs this state to route SWO.
UART0 uses 115200 baud. SWO uses 1000000 baud and ITM stimulus port 0.
Both receive `printf` output through the existing BSP syscall path.

The banner contains `Ethernet diagnostic bring-up complete`.
The capture helper matches `bring-up complete` and reports
`CONSOLE CAPTURE OK`. This is startup-marker reception only, even if
Ethernet init failed. It does not accept link or packet transfer.
Historical helper captures used the old console `PASS` line.
That output had the same console-only scope.
See [console output](console.md) for its exact wording.

### Link acceptance and output

Live evaluation requires a successful capture exit before checking text.
The evaluator then requires:

- The exact identity `LM3S6965 Ethernet link diagnostic` and one
  `ethernet init=OK` record before samples.
- `init=OK`, `poll=OK`, and `poll_errors=0` in every observed sample.
- Complete sequential records starting at sample 0, without gaps,
  duplicates, repeated startup records, or mixed runs.
- At least two consecutive final healthy samples. Each must report
  `link=up`, `negotiation=complete`, and `mac_ready=1`.
- Valid negotiated speed of 10 or 100 Mbps and half or full duplex.
  Unnegotiated samples must report unknown speed and duplex.

Startup down/pending samples are allowed. Any observed polling error
fails acceptance, even if later samples are healthy. Malformed, unknown,
or truncated firmware text also fails acceptance.
The evaluator rejects `ICDI CAPTURE WARNING:` and `warning:` lines,
including flush errors, read errors, and unterminated decoded text.
It also rejects `ICDI CAPTURE FAILURE:` records despite healthy samples.
These include fixed `lifecycle-` acquisition, reader, cleanup, and reporting failures.
A live reader timeout denies acceptance and prevents an unsafe device close.
Successful decoding uses an immutable snapshot after verified reader termination.
Malformed or unknown probe metadata cannot establish acceptance.
Probe stderr uses JSON strings outside decoded firmware boundaries.
See [console capture](console.md) for command receipts and probe status limits.
A generic `CONSOLE CAPTURE OK` can therefore precede a link failure.

On success, exit status is 0. The result line starts with
`ETHERNET LINK PASS: `, then reports negotiated modes, sample count,
and consecutive final healthy sample count.
For the historical repeat text, its summary is the following two strings
joined with one space:

- `100 Mbps full duplex, 10 samples observed,`
- `8 consecutive final healthy samples`

Success then prints these exact lines:

```text
Link only. TX/RX, IP, FTP, and cable-cycle operation were not evaluated.
Sample indices do not measure wall-clock duration.
```

Evaluation failure returns exit status 1 and prints
`ETHERNET LINK FAIL: ` followed by the reason, then the link-only line.
For all-down samples, the exact result line is:

```text
ETHERNET LINK FAIL: need at least two consecutive final healthy samples
```

A failed capture instead prints `ETHERNET LINK FAIL: console capture
exit status N` on one line, with the actual status in place of `N`.
A positive capture status is returned unchanged.
A signal status is converted to 128 plus the signal number.
Argument errors use the parser usage/error output and exit status 2.

### Offline saved-text evaluation

```sh
scripts/ethernet-link-test.sh --capture-file saved-capture.txt
```

This mode reads saved decoded text only. It does not reset or access devices.
It needs `python3` on `PATH`, but not OpenOCD or `pyftdi`.
Do not combine `--capture-file` with `--seconds` or `--baud`.
Those options apply only to live capture. Defaults are 10 seconds and
1000000 baud. Seconds must be finite and positive. Baud must be a
positive decimal integer and must match the flashed firmware.

Offline output starts with these exact lines:

```text
OFFLINE: text-only evaluation, no live capture provenance.
Capture exit status is unknown and is not evaluated.
```

The evaluator accepts bare firmware text, new framed helper output,
and historical helper output with two separator lines.
For new captures, retain both `ICDI DECODED TEXT BEGIN` and
`ICDI DECODED TEXT END` boundaries and all warning lines.
Retain all capture failure, probe status, and probe stderr metadata.
Do not trim metadata or convert a damaged capture into apparent success.
Historical captures cannot prove that the old helper did not normalize
an absent firmware newline. Offline PASS checks the saved text only.
It cannot establish live provenance or the original capture exit status.

Historical evaluator checkpoint: prior real capture logs and 15 offline
fixture/process-seam tests were checked; no fresh physical test ran.
That count is superseded by the latest offline validation: 23 link tests
and 18 capture-lifecycle tests passed with mocked hardware and capture
operations. These results make no hardware or current user-activity claim.

### Historical initial physical run: 2026-10-05

The separate diagnostic and both existing firmware targets built successfully.
Host driver tests passed with address and undefined-behavior sanitizers.
The diagnostic used 6668 bytes of text, 92 bytes of data, and 376 bytes of BSS.
Independent review found no safety blockers. Style findings were corrected.
The long pinned reference URL has an explicit line-length exception.

The approved flash used the explicit diagnostic ELF path shown above.
OpenOCD reported `Verified OK` and reset the target.
The flashed ELF SHA256 was:

```text
ebe9745a3a127df706a2bc1e55c91c3c77f174f954fb1db0a6bf481d11b4a28f
```

That link diagnostic replaced the previous demo at the time.
The RAW diagnostic now supersedes it on the board.
A 10-second SWO capture at 1000000 baud returned exit status 0.
The console reported PLL lock at 50 MHz and `init=OK`.
All ten samples, numbered 0–9, reported `poll=OK` and `poll_errors=0`.
Each reported `link=down`, `negotiation=pending`, and `mac_ready=0`.
Speed and duplex remained `unknown`. No transitions were observed.
The script PASS confirms console reception only, not physical link acceptance.

No inspected wired host interface reported an active link.
The connected adapter mapping, model, and actual compatibility remain unverified.
No diagnosis is established. Frame transfer and FTP were not tested.
No cable-cycle test ran, and no host network settings changed.
These down-link observations are historical, not the current link result.
PHY-only link testing needs no IP address configuration.

### Repeat physical link result: 2026-10-05 — SUCCESS

The repeat console capture reports PLL lock at 50 MHz and `init=OK`.
All ten samples, numbered 0–9, report `poll=OK` and `poll_errors=0`.
Samples 0–1 report down/pending, unknown speed and duplex, and `mac_ready=0`.
Samples 2–9 report up/complete, 100 Mbps, full duplex, and `mac_ready=1`.
One startup up transition was observed between samples 1 and 2.
This is successful physical negotiation, not packet-transfer acceptance.
The startup banner and script PASS remain console evidence only.

The read-only host checks before and after capture report an active link.
Both report `100baseTX` and full duplex. No host settings changed.
The exact adapter model remains unknown.
The user reported cable reseating and working green/amber LEDs.
These are user observations, not independent optical measurements.
The evidence does not prove that cable reseating caused the successful link.

Frame RX/TX, IP, and FTP remain untested.
No deliberate unplug/replug recovery test ran during this capture.
Deliberate cable-cycle recovery remains pending and requires approval.

### State and limits

The application configures the PLL first. It uses 50 MHz on PLL lock, or
8 MHz from the external crystal on PLL timeout. It reports this choice.
It passes that clock to UART, trace, SysTick, and Ethernet initialization.

The test MAC is `02:00:00:69:65:01`. It is a locally administered unicast
address. It is link-test-only, not an assigned hardware MAC or production
identity. MAC uniqueness is not critical for this PHY-only test.
Select a suitable unique identity before any future packet test.

Initialization uses the driver's bounded reset and MII iteration waits.
`init=OK` means initialized, not linked. The application calls init once.
If it fails, the application keeps running and reports the original failure.
It does not retry init. A later poll cannot repair failed initialization.

Each `ethernet sample=...` line reports these key-value fields:

- `sample`: heartbeat sequence, not elapsed seconds. It wraps at 32 bits.
- `init`: original initialization result.
- `poll`: current poll result, including `TIMEOUT` or `NOT_READY`.
- `poll_errors`: cumulative non-OK polls. It wraps at 32 bits.
- `link`: `up`, `down`, or `unknown` after a poll error.
- `negotiation`: `complete`, `pending`, or `unknown` after a poll error.
- `speed_mbps` and `duplex`: negotiated mode, or `unknown` until valid.
- `mac_ready`: driver readiness. Zero on a poll error, not proof of link down.

There is an immediate sample, then one poll and report per blocking
1000 ms delay. This heartbeat reports unchanged state too. It limits SWO
output without an interrupt clock. It can miss transitions between polls.
Poll and console time extend the interval. SysTick COUNTFLAG coalesces
missed ticks. This delay is not a monotonic clock or a network timer.
A separate timer design for future lwIP remains pending.

The diagnostic submits no frames and makes no host network changes.
It has no lwIP or FTP code. PHY autonegotiation still uses link signaling.
The driver assigns PF2/PF3 to PHY LEDs after successful initialization.
A successful poll can enable MAC TX/RX, but the application never requests TX.
It does not drain RX. Incoming traffic can fill the RX FIFO.
Do not treat this diagnostic as an RX or packet-transfer test.
On init timeout, clocks remain enabled and reset is released, as specified
by the driver. The application performs no rollback.

## Accepted DriverLib reference

The user accepts this third-party reference for an iterative proof of concept
(PoC). Official TI package authentication is not a progress gate.
Do not seek an official TI distribution for this PoC.

Reference: [VENGEL/StellarisWare, pinned Ethernet source][driverlib].

- Commit: `a7454118bb72992342759dc084f426d4707935c2`.
- Source path: `StellarisWare/driverlib/ethernet.c`.
- The header identifies the legacy integrated-MAC DriverLib, revision 10636.
- The header states TI copyright 2006–2013.
- The source comes from a third-party mirror.
  It has not been authenticated against an official TI package.
- Use it as corroborating evidence. Do not import vendor code.

The inspected reference source is not a repository dependency.
Its verified SHA256 is:

```text
f8da619db84b6196dab0c96f21e0cd84d62f11c6073916f5fd2f3b0c374c7341
```

### RX count and FIFO drain

`EthernetPacketGetInternal` implements the receive logic used by
`EthernetPacketGet`. Line numbers below refer to the pinned source.
Let `L` be the reported FIFO byte count.

| Source lines | Evidence |
|---|---|
| 530 | The count includes a two-byte prefix, frame, and four-byte FCS. |
| 554–562 | The code extracts `L` and selects `min(buffer length, L - 6)`. |
| 598–601 | The code drains remaining words while `i < L - 2`. |
| 607–616 | Returns `L - 6`, negated when the buffer is too small. |

The first FIFO word contains the prefix and two frame bytes.
After that read, `i` is 2. Each later FIFO read advances `i` by four.
The total FIFO read count is `ceil(L / 4)` words.
The code drains a truncated packet too.
These source observations corroborate the driver's RX convention.
They do not prove physical FIFO behavior.

## Evidence limits and local documents

- **Documentary evidence:** The [MCU datasheet][datasheet] defines the part.
  RX references include pages 554–555, 560, 568, and 578.
  Printed and PDF page numbers match in these sections.
  The accepted DriverLib mirror supplies separate source corroboration.
- **Board documentation:** The [board guide][board] describes Ethernet on
  printed/PDF pages 11–12. PDF page 18 shows Rev-D schematic sheet 1 of 3.
  That sheet has no separate printed book-page number.
  A schematic documents a circuit. It does not verify board operation.
- **Host-model evidence:** `tests/ethernet_test.c` checks software against a
  simulated register and FIFO model. Its assumptions are not independent
  hardware evidence. Passing host tests cannot prove the RX count convention.
- **Physical evidence:** The initial run observed initialization and ten
  down-link polls. The repeat observed successful 100 Mbps full-duplex negotiation.
  The later RAW readiness run also observed initialization and negotiated
  readiness, with no accepted requests or submitted replies.
  These runs do not establish bidirectional frame exchange, IP, FTP, or
  deliberate cable-cycle recovery.

[xnu-getgroups]: https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_prot.c#L414-L448
[xnu-setgroups]: https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_prot.c#L1334-L1373
[xnu-egid]: https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_credential.c#L1864-L1917
[xnu-ucred]: https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/ucred.h#L211
[xnu-corroboration]: https://github.com/apple-oss-distributions/xnu/blob/ac9718fb1af618d5ce8678d0dc6e8a58f252216f/bsd/kern/kern_prot.c
[driverlib]: https://github.com/VENGEL/StellarisWare/blob/a7454118bb72992342759dc084f426d4707935c2/StellarisWare/driverlib/ethernet.c
[datasheet]: ../datasheets/LM3S6965%20Microcontroller%20Data%20Sheet.pdf
[board]: ../datasheets/Stellaris%20LM3S6965%20Evaluation%20Board.pdf
