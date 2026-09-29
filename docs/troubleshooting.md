## If it fails

The error message tells you which layer is broken:

| Message | Meaning |
|---|---|
| `unable to open ftdi device with vid 0403, pid bcda` | The config expects the wrong probe. This board is `0403:bcd9`. |
| `Error: JTAG scan chain interrogation failed` | The probe is present, but the target does not answer. Check the target power and the JTAG/SWD ribbon. |
| `Error: unknown command` | Config parse failure. The OpenOCD version does not match. |

Some systems give `0403:bcda` to the built-in FTDI driver in macOS. If the probe enumerates
but OpenOCD cannot open it, unload that driver.

### The ICDI identity does not match the shipped config

The error `unable to open ftdi device with vid 0403, pid bcda` reads like an unplugged board,
but the board is present. The shipped interface file `ftdi/luminary-icdi.cfg` targets the
LM3S9B9x evaluation kit and matches two fields that this board does not:

| Field | Shipped config expects | EK-LM3S6965 reports |
|---|---|---|
| Vendor ID | `0x0403` | `0x0403` |
| Product ID | `0xbcda` | `0xbcd9` |
| Product string | `"Luminary Micro ICDI Board"` | `"Stellaris Evaluation Board"` |

The on-board ICDI is the same FTDI FT2232 design with the same pin layout, but a different USB
identity. Do not source the shipped config and patch it — both fields need to change and
`source` ordering gets fragile. Use the dedicated config
`openocd/interface/luminary-icdi-ek-lm3s6965.cfg`.

Compare the numbers directly rather than reading the prose:

```bash
ioreg -p IOUSB -w0 -l | grep -A1 -B4 'idProduct'
```

`ioreg` reports decimal; OpenOCD configs use hex. `1027` is `0x0403`, and `48345` is `0xbcd9`.
On recent macOS, `system_profiler SPUSBDataType` does not report this device at all, so a
detection script built on it returns a confident, wrong "NOT FOUND". Use `ioreg` and treat
`system_profiler` as a display tool, not a query API. A missing serial node also proves nothing:
see [Console output](console.md) for why the ICDI has no `/dev/cu.usbserial-*` node yet still
supports debug access.

> When a USB-backed tool says "device not found", verify the *identity* it is matching on before
> believing the *absence*.

### Read the schematic before the registers

The debug probe's `BDBUS0`/`BDBUS1` lines do not run as copper to the MCU. On this board they
enter a Lattice LC4032V CPLD (`ispMACH 4000V`) labelled *Debug Interface Logic*, which sits
between the probe and the chip. Every "net" is therefore a route through programmable logic
that can buffer, invert, gate, or synthesise a signal. The CPLD switches the debug pins between
JTAG and SWD (driven by the probe's `SWD_EN` line) and taps the MCU's `TDO`/`SWO` pin out to
the probe's second channel.

No amount of register archaeology reveals this; the schematic does, in seconds. A net name is a
confession: the one that mattered was `VCP_TX_SWO`, whose `_SWO` suffix identified the
Cortex-M3 single-wire trace output before any pin sweep confirmed it.

> Before concluding "not connected", read the schematic and look for a CPLD, FPGA, buffer,
> level shifter, or mux between a debug probe and the MCU.

See [Console output](console.md) for how the CPLD gates the SWO console.

### A negative result from unvalidated tooling proves nothing

Two failures have the same shape — a check that reports success while measuring nothing:

- A capture script set `pyftdi` timeouts to floats. Floats raise, and the reads sat inside a
  bare `except Exception: pass`, so every read failed silently and the script reported "0
  bytes" while the line was alive.
- A 51-pin sweep found nothing. Because it also probed `PA0`, a known-connected positive
  control, the 50 negatives carried information: the method worked and the silence was real.

> Before believing a negative, prove the measurement *can* produce a positive. Include a
> known-good control.

Never write `except: pass` around an I/O read in a diagnostic: a silent failure in a measuring
instrument is indistinguishable from the thing being measured being absent. Log raw bytes, not
just the interpretation — "captured 0 bytes" is a conclusion, and the raw hex would have shown
the reads never returned.
