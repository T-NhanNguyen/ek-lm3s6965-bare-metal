# PC FTP client (restricted bench)

Use `python3 tools/boot/client.py --help` for the command syntax. The signer
`tools/boot/package.py` stays offline. This client does not sign images.

## Boundary

This is plain FTP for an isolated, trusted bench. Status and confirmation are
unsigned. Neither passwords nor local package checks authenticate the server.
Local `Artifact` checks verify framing, bounds, hash, and vectors, **not ECDSA**.
The device must authenticate firmware before mutation. Production startup,
reset scheduling, watchdog timing, physical flash durability, and final boot
link/32 KiB fit remain separate work. The example application is still RAM-only;
it does not expose the opt-in boot binding until the application wires it.

Use only after separate approval for device and network access. These examples
are usage instructions, not evidence of a live run:

```sh
python3 tools/boot/client.py --host 192.0.2.2 status
python3 tools/boot/client.py --host 192.0.2.2 update \
  --slot-a release-a.pkg --slot-b release-b.pkg --polls 60
python3 tools/boot/client.py --host 192.0.2.2 update \
  --slot-a release-a.pkg --slot-b release-b.pkg --polls 60 --confirm
python3 tools/boot/client.py --host 192.0.2.2 resolve \
  --package release-b.pkg --trial-id 21 --polls 60
```

`update` validates both bounded package files before it opens FTP. It reads
permission and selects the physical slot artifact. It makes **one** firmware
attempt. It polls with one second between reads, up to `--polls` (1..120).
`--confirm` is allowed only on `update`. It acts on a healthy matching trial
within that same live workflow object. It rereads status before confirmation.
It then polls again, with the same bound, to resolve durable confirmation.
A lost firmware or confirmation completion reply is not success. No automatic
firmware retry occurs. Polling preserves the pre-upload sequence and pinned
trial ID across reconnects. Fresh acceptance still requires
`pre_upload_sequence < trial_id <= observed_sequence`.

Do not rerun `update` as a retry for a lost reply. There is no saved upload
session or resumable fresh-upload state. `resolve` is read-only: supply a
previously known exact package and retained trial ID. It cannot authorize a
trial or send either STOR. Factory retained ID zero is valid. The existing
resolver's defensive sequence-zero/retained-zero limit remains unchanged:
a matching confirmed status with sequence zero can still resolve. The factory
contract starts sequence at one. Do not infer a retained ID from an arbitrary
first response and call it upload provenance.

## Endpoint and credentials

Only numeric IPv4 is accepted. DNS and IPv6 are not supported. Both control
and passive data connections construct AF_INET/SOCK_STREAM sockets directly;
neither uses create_connection or getaddrinfo. The remaining budget is set
immediately before connect. Port defaults to 21.
`--user` or `BOOT_FTP_USER` sets the username. The existing server accepts
`USER anonymous` and a PASS body, so defaults are `anonymous` and `bench@`.
These defaults provide no access control. Set `BOOT_FTP_PASSWORD` from a secure
shell environment, or use `--prompt-password` for a non-echoed prompt. Do not
put a password in command-line arguments. There is no password argument.
Credentials must be nonempty printable ASCII, at most 256 bytes. They are
sent literally, including password `-` for `anonymous` (no ftplib rewrite).
Control characters are rejected before the FTP factory runs. Server error text,
passwords, and filesystem paths are not printed on failure.

## Deadlines and transport

`ftp_transport.FTPTransport` uses stdlib `ftplib`. It accepts an injected FTP
factory and monotonic clock. The factory supplies `arm(remaining)` in addition
to the used ftplib methods. The default `DeadlineFTP` enforces the budget
inside control reads and partial writes. It does not use buffered control
`readline`, which could restart a timeout on each read indefinitely.

Defaults: each blocking call is limited to **5 seconds**, and each connect,
binary selection, RETR, or STOR operation has a **75 second total deadline**.
The CLI accepts integer overrides of 1..120 seconds. A monotonic remaining
budget is recomputed before/after data chunks and control steps, for each
control byte/write, and after PASV before the numeric data connection. No
worker threads or timeout retries are used. Bounds apply to network work,
not local filesystem operations, OS scheduling pauses, or password prompts.
Each reconnect discards and closes the old connection, then requires greeting
220, USER reply 331, PASS reply 230, selects passive mode, and requires TYPE I
reply 200. Preliminary greeting 120, early USER success, and ACCT are not
supported; they fail closed rather than queue replies for the next command. Each of the three transfers selects
TYPE I again. Passive data addresses use ftplib's default control-peer policy;
the client does not opt into trusting a foreign PASV address.

Status reads preserve exact binary bytes, read at most 1025 bytes, and reject
byte 1025 without draining an unbounded body. Firmware is 396..111776 bytes;
confirmation is 1..128 bytes. The workflow produces the exact package and
strict confirmation body; the transport does not translate or append bytes.
Writes handle partial sends. Data EOF/close is followed by the final control
reply, which must be exactly 226 (not 200, 221, or ABOR's 225). PASV requires
227 with six decimal octets in 0..255 and a nonzero port; transfer preliminary
reply must be 150, not arbitrary 1xx or an initial 2xx. Active mode, REST, and
125 are not supported by this restricted adapter. Server numeric conversion
failures are normalized at the parser boundary. The data socket remains
explicitly owned through preliminary parsing and closes on parsing failure.
Protocol errors, socket errors, lost EOF/replies, or deadline expiry raise a
safe workflow `OSError` and discard the control stream. Cleanup does
not hide the original error. There is no ABOR or risky reuse of a failed stream.

Control replies (including errors) are limited to **65536 aggregate wire
bytes** and **256 lines** per reply, inclusive of opening/terminating codes,
separators, content, and every CR/LF. Each line remains limited to **8192 wire
bytes**, including LF or CRLF. The first byte beyond either byte limit closes
the control socket immediately, before retaining that byte; a required 257th
line closes it before reading that line. These conservative limits accommodate
this restricted service without retaining an unbounded multiline transcript.
Bounded lines are collected and joined once, not repeatedly concatenated.
All reads still share the operation's total deadline; progress does not reset it.

RFC 959 multiline replies are supported: an ASCII three-digit opening code
followed by `-` ends only at the **same code followed by SPACE**. Matching-code
lines with `-` continue; bare matching codes and other matching-code separators
are protocol errors, not successful terminators. Foreign-code and arbitrary
interior lines remain content, not standalone replies or terminators. A valid
single-line reply requires a three-digit code and SPACE. The expected command
state is checked after complete framing; malformed/oversized replies close
control and any owned data socket and produce only the redacted `OSError`.

## Results

| Output | Exit | Meaning |
| --- | --- | --- |
| Status body | 0 | A parsed status was read, not update success |
| `CONFIRMED` | 0 | Matching durable confirmed status was observed |
| Error on stderr | 2 | Invalid arguments, package, admission, or status |
| `TRIAL` | 3 | Matching healthy trial, not confirmed success |
| `UNKNOWN` | 4 | No outcome established; includes transport/filesystem errors |
| `MISMATCH` | 5 | Known identity conflict |
| `UNHEALTHY` | 6 | Matching trial without all required health bits |

No success message is printed for UNKNOWN, MISMATCH, or UNHEALTHY. A final FTP
226 alone is not reported as update success. Poll bounds count operations,
not seconds: default worst-case polling includes up to 60 connect/read pairs
plus 59 one-second sleeps. Confirmation can add a second bounded poll phase.

## Offline validation

`boot_pc_workflow` retains its 23 methods. `boot_pc_ftp` registers the separate
transport and CLI suite (45 methods). Its FTP, control, data, and clock doubles
open no sockets or listeners. Nineteen methods exercise the production
DeadlineFTP orchestration, bounded multiline reader, and actual inherited
getresp/sendcmd/transfercmd and parse227/parse150 paths with only lower-level
socket primitives replaced. Proposed coverage includes valid multiline setup
and transfers, malformed matching-code terminators, foreign-code content,
exact aggregate byte/line limits and overflow with a frozen clock, error replies,
multiline deadline expiry, data ownership, discard and fresh reconnection.
Test execution belongs to test-merit-gate. Syntax and HOST build checks do
not establish behavioral, live transport or hardware acceptance.
