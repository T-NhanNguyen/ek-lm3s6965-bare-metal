# Board bring-up and console

Use the console to establish startup visibility before diagnosing a peripheral. A startup marker proves console reception, not Ethernet, display or application health. Physical capture resets the board and requires separate approval.

## Find the contract

- [Console paths and capture](#console-paths-and-capture)
- [Troubleshooting by layer](#troubleshooting-by-layer)


## Console paths and capture

The firmware sends every `printf` byte to **two** consoles at the same time. Each one has its
own setup call in `main`.

| Backend | Path out of the chip | Read it with | Rate |
|---|---|---|---|
| UART0 | the `PA0` and `PA1` header pins | a USB serial adapter | 115200, 8N1 |
| ITM and SWO | the trace pin, tapped by the on-board ICDI | the on-board ICDI | 1000000, 8N1 |

`uart0_initialize` configures the UART. `trace_initialize` configures the ITM and the TPIU.
`_write` in `src/syscalls.c` fans each byte out to both.

#### The SWO console, through the ICDI

This path needs no extra hardware. Two conditions apply.

1. **The probe must use SWD.** In JTAG mode the `TDO` pin carries JTAG data, so the ICDI tap
   cannot see SWO. In SWD mode that pin carries SWO instead. OpenOCD asserts the ICDI's
   `SWD_EN` line for `transport select swd`, and the ICDI switches over.
2. **The rate must match.** The firmware sets 1 Mbaud through `SWO_BAUD_RATE`. The TPIU
   prescaler is `system_clock_hz / SWO_BAUD_RATE - 1`.

An SWD probe does not prevent JTAG from working later. This was measured on the board: with
the trace unit fully enabled, a JTAG connection still read `DID0` and every trace register.
The debug port arbitrates the shared pin by transport. There is no mode to forget to change
back.

#### Which transport the debug pin is running

On this board the CPLD taps the shared `TDO`/`SWO` pin to the ICDI's second channel, so the
raw pin level is directly readable. With the trace unit off, that byte identifies the transport:
`0xff` means JTAG (the `TDO` shift logic drives the pin high), and `0xfd` means SWD (the pin is
undriven and sits low). With no probe session attached, the CPLD defaults to JTAG, so a silent
console should be checked here before anything else.

`SWD_EN` is driven by the debugger, not by the board. It is asserted only while the probe holds
a session and deasserts when the probe disconnects, at which point the CPLD falls back to JTAG.
A capture that resets the target and then shuts the probe down loses that race: the firmware
starts printing, the probe exits, and the SWO stream disappears mid-banner.
`openocd -c "init" -c "reset" -c "shutdown"` fails this way.

Capture the banner. Install `pyftdi` first, as in [Required packages](toolchain.md).

```bash
scripts/icdi-console.sh
```

The script needs `openocd` on `PATH` and a Python interpreter with `pyftdi`.
It tries `PYFTDI_PYTHON`, the repository `.venv/bin/python`, then `python3`.
It opens the ICDI second channel, starts the reader, then resets the target.
The order matters because the firmware prints the banner one time only.
It does not flash or change host network settings.

Success requires `bring-up complete` and confirmed probe command completion.
OpenOCD must remain alive until the helper starts teardown.
The script returns exit status 0. Its final result line joins these
exact strings with one space:

- `CONSOLE CAPTURE OK: startup marker 'bring-up complete'`
- `received over SWO through the ICDI`

The preceding line is `Peripheral operation was not evaluated.`

The marker can arrive even when Ethernet initialization or link fails.
Use [the Ethernet link evaluator](../scripts/ethernet-diagnostics.md) for link acceptance.
Historical captures used `PASS: console received over SWO through the ICDI`.
That old output also meant console reception only.

New captures frame firmware text with `ICDI DECODED TEXT BEGIN` and
`ICDI DECODED TEXT END`. Keep both boundaries and all warning lines
when saving text. `ICDI CAPTURE WARNING:` records flush errors, read
errors, or an unterminated decoded payload. The helper adds a display
newline for an unterminated payload, but preserves its warning.
Warnings do not change the generic startup-marker success criterion.
Probe failures do change that criterion and return nonzero.
The Ethernet evaluator rejects transport warnings even after capture
success. Do not remove them before offline evaluation.

OpenOCD runs `init`, then `reset run`, then checks target examination.
It emits a command receipt only after those commands complete.
The helper requires that receipt and rejects OpenOCD `Error` diagnostics.
Command completion does not independently prove a physical reset.
An unexpected probe exit fails capture, including exit status zero.
A nonzero result after requested termination alone does not fail capture.

`ICDI CAPTURE FAILURE:` records probe exit, command, launch,
or inspection failure.
Missing command receipts and oversized diagnostics also fail capture.
`ICDI PROBE STDERR:` retains diagnostics as JSON strings outside
firmware boundaries.
The helper spools stderr to a temporary file, avoiding an undrained pipe.
It retains at most 65536 diagnostic bytes and rejects larger output.
Keep all failure and probe metadata when saving a capture.

#### Capture resource ownership

Cleanup covers partial device setup, reader startup, and probe launch.
The helper signals reader stop and joins for at most two seconds.
It verifies termination before taking an immutable byte snapshot.
Decoding and `--raw` use that snapshot, not the shared reader buffer.
The device closes only after reader termination is verified.

A reader that cannot stop retains its device handle until process exit.
The helper does not close that handle or snapshot its mutable buffer.
It denies acceptance and retains the first failure as primary.
Without an earlier failure, it reports
`ICDI CAPTURE FAILURE: lifecycle-reader-live`.
With an earlier failure, secondary reader uncertainty still prevents snapshot and closure.
Secondary operation exceptions retain their failure codes and JSON details.
The daemon cannot delay helper exit indefinitely.
This policy avoids concurrent device closure, not every possible USB failure.

The helper terminates a live probe and waits for at most five seconds.
A wait timeout triggers kill and a final five-second reap attempt.
Cleanup errors remain separate from the first failure.
Fixed `lifecycle-` failure records cover acquisition, cleanup, snapshot, and reporting.
Error details use JSON diagnostic strings, not firmware records.
All lifecycle failures deny capture and saved-text link acceptance.
Reporting failures use stderr when stdout cannot carry diagnostics.
An unusable output stream cannot provide a complete saved capture.

The stderr spool closes after the reap attempt, including failed attempts.
A failed kill or reap remains an error, not proof of child termination.
Spooling avoids pipe deadlock but does not impose a disk-usage limit.

| Option | Effect |
|---|---|
| `--seconds N` | Finite, positive capture window in seconds. The default is 4. |
| `--baud RATE` | SWO rate. It must match `SWO_BAUD_RATE`. The default is 1000000. |
| `--raw` | Also print the raw captured bytes as hex. |

> **The probe must stay attached for the whole capture window.** The board's CPLD routes
> SWO only while the debugger asserts `SWD_EN`. A capture that stops OpenOCD too early
> receives nothing.

#### CoreSight registers are locked at reset

The ITM, TPIU, DWT, and ETM ignore writes to most of their registers until the **Lock Access
Register** receives the key `0xC5ACCE55`. While locked, a write is discarded with no error and
no status bit, and a read-back returns the previous value. Usually `0`. The symptom is an ITM
that "looks enabled" while SWO produces nothing:

```
write ITM_TCR = 0x1        ->  read ITM_TCR = 0x00000000
write ITM_LAR = 0xC5ACCE55
write ITM_TCR = 0x1        ->  read ITM_TCR = 0x00000001
```

Each CoreSight component has its own lock access register at offset `0xFB0`:

| Component | Base | Lock access register |
|---|---|---|
| ITM | `0xE0000000` | `0xE0000FB0` |
| TPIU | `0xE0040000` | `0xE0040FB0` |
| DWT | `0xE0001000` | `0xE0001FB0` |

`DEMCR.TRCENA` (bit 24 of `0xE000EDFC`) must be set before any other trace register is
programmed, because the core must claim the trace unit first. After writing any CoreSight
register, read it back and compare: a write that worked and a write that was silently discarded
look identical in the return value alone.

The working sequence is:

```
DEMCR     |= TRCENA
ITM_LAR    = 0xC5ACCE55
TPIU_LAR   = 0xC5ACCE55
ITM_TER    = 0x1         # stimulus port 0
ITM_TCR    = 0x1         # ITMENA
TPIU_CSPSR = 0x1         # 1-bit port = SWO
TPIU_ACPR  = 49          # 50 MHz / 50 = 1 Mbaud
TPIU_SPPR  = 0x2         # NRZ
TPIU_FFCR  = 0x100       # continuous formatting
```

#### The ITM FIFO drops writes

A write to an ITM stimulus port does not wait. When the FIFO is full, the write is discarded
with no error and no status bit. On a fast burst the firmware can empty its whole banner into
the FIFO before the TPIU shifts any of it out, and the missing characters look like a broken
reader. The first line `LM3tal bring-up` instead of `LM3S6965 bare-metal bring-up` is the
signature: the loss is at the start, and every later line is intact.

Raising the SWO rate does not fix this. The loss is constant across rates because the FIFO fills
at core speed, not at the serial rate:

| SWO rate | First line | Lost characters |
|---|---|---|
| 1 Mbaud | `LM3tal bring-up` | 13 |
| 2 Mbaud | `LM35tal bring-up` | 12 |

Constant loss across rates points at a fixed startup overflow, not a slow link. The FT2232D also
rejects 2.5 Mbaud: its tolerance is 4 %, and the highest rate it accepts is 2.4 Mbaud.

Poll bit 0 of the stimulus port's ready bit before each write. It reads `1` only while the ITM
can accept a byte:

```c
while ((REGISTER32(ITM_STIM0) & 1u) == 0u) { }
REGISTER32(ITM_STIM0) = byte;
```

Bound the wait so a stopped TPIU cannot hang the firmware. With the poll in place, four
consecutive captures decoded 250 characters each, byte-identical, with the full first line.

A capture that loses data shows bytes outnumbering decoded characters by far more than the 2:1
that ITM packets need. Here it was about 5:1, and the surplus was `0x70` overflow packets.

#### QEMU cannot drain the ITM

The ITM write waits on the ready bit with a bound of 1,000,000 spins. QEMU does not model the
ITM, so the ready bit never sets and every byte spends the full bound. Each spin is an MMIO
read, and QEMU MMIO reads are slow, so the console advances at roughly 0.1 s per byte and a full
banner takes about 45 seconds. Give a scripted QEMU capture a window of 45 seconds or more. A
short window looks like a hang. On real hardware the SWO consumer drains the FIFO, so the bound
is never reached. See [Run without hardware](applications.md).

#### The UART console

A USB serial adapter on the UART0 header also works. The firmware muxes `PA0` and `PA1` to
UART0, so the header is live.

> **The firmware prints the banner one time only.** It prints from `main()`, directly after
> reset. The reader must therefore listen before the reset. `scripts/console.sh` starts the
> reader first and resets the target second. A manual capture needs the same order.

#### The ICDI virtual COM port is not a serial port

macOS cannot bind the ICDI virtual COM port. The FTDI VCP driver is a system extension. Its
`Info.plist` lists **436** accepted `idVendor:idProduct` pairs. The pair `0x0403:0xbcd9` of
this board is not in that list. The driver therefore never creates a `/dev/cu.usbserial-*`
node. Installing the driver does not help.

That limit does not matter. The SWO console above reaches the ICDI over raw libusb, exactly
as OpenOCD does.

#### Wire the adapter

| Board UART0 header | Adapter |
|---|---|
| PA1 (UART0 TX) | RXD |
| PA0 (UART0 RX) | TXD |
| GND | GND |

Use 115200 baud, 8 data bits, no parity, 1 stop bit, and no flow control.

#### Capture the banner

```bash
scripts/console.sh
```

The script finds the adapter, opens the port, and resets the target.

> **The firmware prints the banner one time only.** It prints from `main()`, directly after
> reset. The reader must therefore listen before the reset. `scripts/console.sh` starts the
> reader first and resets the target second. A manual capture needs the same order.

To read the port by hand, start `picocom` first. Reset the board second.

```bash
picocom -b 115200 /dev/cu.usbserial-XXXX
```

Expect the banner from [Run without hardware](applications.md). It includes
`sysclk:50000000 Hz` and `rcc: 0x01CE1380`.

**Baud rate without a console.** The divisor registers confirm the rate directly. `RCC`
reads back `0x01CE1380`, which is 50 MHz. `UART0_IBRD` and `FBRD` read `27` and `8`. The
relation `50 000 000 / (16 × 115200) = 27.1267` therefore holds. The baud rate is 115200
within the crystal tolerance. This result comes from measured registers, not from observed
characters.


## Troubleshooting by layer

The error message tells you which layer is broken:

| Message | Meaning |
|---|---|
| `unable to open ftdi device with vid 0403, pid bcda` | The config expects the wrong probe. This board is `0403:bcd9`. |
| `Error: JTAG scan chain interrogation failed` | The probe is present, but the target does not answer. Check the target power and the JTAG/SWD ribbon. |
| `Error: unknown command` | Config parse failure. The OpenOCD version does not match. |

Some systems give `0403:bcda` to the built-in FTDI driver in macOS. If the probe enumerates
but OpenOCD cannot open it, unload that driver.

#### The ICDI identity does not match the shipped config

The error `unable to open ftdi device with vid 0403, pid bcda` reads like an unplugged board,
but the board is present. The shipped interface file `ftdi/luminary-icdi.cfg` targets the
LM3S9B9x evaluation kit and matches two fields that this board does not:

| Field | Shipped config expects | EK-LM3S6965 reports |
|---|---|---|
| Vendor ID | `0x0403` | `0x0403` |
| Product ID | `0xbcda` | `0xbcd9` |
| Product string | `"Luminary Micro ICDI Board"` | `"Stellaris Evaluation Board"` |

The on-board ICDI is the same FTDI FT2232 design with the same pin layout, but a different USB
identity. Do not source the shipped config and patch it. Both fields need to change and
`source` ordering gets fragile. Use the dedicated config
`tools/openocd/interface/luminary-icdi-ek-lm3s6965.cfg`.

Compare the numbers directly rather than reading the prose:

```bash
ioreg -p IOUSB -w0 -l | grep -A1 -B4 'idProduct'
```

`ioreg` reports decimal. OpenOCD configs use hex. `1027` is `0x0403`, and `48345` is `0xbcd9`.
On recent macOS, `system_profiler SPUSBDataType` does not report this device at all, so a
detection script built on it returns a confident, wrong "NOT FOUND". Use `ioreg` and treat
`system_profiler` as a display tool, not a query API. A missing serial node also proves nothing:
see [Console output](bring-up.md) for why the ICDI has no `/dev/cu.usbserial-*` node yet still
supports debug access.

> When a USB-backed tool says "device not found", verify the *identity* it is matching on before
> believing the *absence*.

#### Read the schematic before the registers

The debug probe's `BDBUS0`/`BDBUS1` lines do not run as copper to the MCU. On this board they
enter a Lattice LC4032V CPLD (`ispMACH 4000V`) labelled *Debug Interface Logic*, which sits
between the probe and the chip. Every "net" is therefore a route through programmable logic
that can buffer, invert, gate, or synthesise a signal. The CPLD switches the debug pins between
JTAG and SWD (driven by the probe's `SWD_EN` line) and taps the MCU's `TDO`/`SWO` pin out to
the probe's second channel.

No amount of register archaeology reveals this. The schematic does, in seconds. A net name is a
confession: the one that mattered was `VCP_TX_SWO`, whose `_SWO` suffix identified the
Cortex-M3 single-wire trace output before any pin sweep confirmed it.

> Before concluding "not connected", read the schematic and look for a CPLD, FPGA, buffer,
> level shifter, or mux between a debug probe and the MCU.

See [Console output](bring-up.md) for how the CPLD gates the SWO console.

#### A negative result from unvalidated tooling proves nothing

Two failures have the same shape. A check that reports success while measuring nothing:

- A capture script set `pyftdi` timeouts to floats. Floats raise, and the reads sat inside a
  bare `except Exception: pass`, so every read failed silently and the script reported "0
  bytes" while the line was alive.
- A 51-pin sweep found nothing. Because it also probed `PA0`, a known-connected positive
  control, the 50 negatives carried information: the method worked and the silence was real.

> Before believing a negative, prove the measurement *can* produce a positive. Include a
> known-good control.

Never write `except: pass` around an I/O read in a diagnostic: a silent failure in a measuring
instrument is indistinguishable from the thing being measured being absent. Log raw bytes, not
just the interpretation. "captured 0 bytes" is a conclusion, and the raw hex would have shown
the reads never returned.
