# FTP core API and ownership contract

## Scope and evidence

`src/ftp_core.c` and `include/lm3s6965/ftp_core.h` implement a C11 state
machine with no lwIP, BSP, MMIO, filesystem, flash, heap, or printf dependency.
It owns one static session, but no file buffers, PCB, or timer. It borrows an
explicit immutable `const ftp_storage_t *` descriptor and its context/ops.
Storage operations are synchronous, bounded, serialized, and non-reentrant.
This remains a singleton, not a multi-server or shared-stack abstraction.

The native assertions test the core only. They do not prove TCP behavior,
board IP operation, or an FTP round trip on hardware. Run the offline check:

```sh
./scripts/ethernet/test.sh core
```

The script uses `${CC:-cc}` with address and undefined-behavior sanitizers.
It builds in a temporary directory and removes the directory on exit.
It opens no socket and changes no host setting.
The opt-in archive is `lm3s6965_ftp_core`.
See the [reusable library guide](../../docs/ftp-library.md) for build reuse and the full storage contract.

## Service rules

Accept one control client at a time. Allow only `USER anonymous`, then `PASS`.
The password has no security function and may be empty. Login starts without
binary mode. Require `TYPE I` before STOR or RETR. Reject TYPE A; it also clears
the binary selection. Command names are case insensitive. Arguments are exact
and case sensitive. Do not trim or decode paths.

The backend configures one exact, case-sensitive basename (1..63 bytes),
matched by `matches_name(context, span, byte_length)` as basename or one root
slash plus basename. No trim/decoding is performed. The file does not exist
after explicit reset, but binding and server start preserve committed data. A committed empty upload creates an existing zero-length file. STOR can
replace the file. RETR and SIZE require an existing file. Each transfer needs
a new passive setup. An accepted data connection can precede STOR or RETR.
Only one data connection is allowed. Reject other data peers in transport glue.

Supported commands are USER, PASS, SYST, FEAT, PWD, XPWD, CWD `/`, CDUP,
TYPE I, EPSV, PASV, STOR, RETR, SIZE, NOOP, ABOR, and QUIT.
EPSV and PASV take no argument. PORT, EPRT, ASCII mode, directory listings,
other file names, and other commands are not supported. Before login, only
login commands, SYST, FEAT, NOOP, ABOR, and QUIT are available. During a transfer
only NOOP, SYST, FEAT, ABOR, and QUIT are available. While listener creation
is pending, all further input waits for its result, preserving reply FIFO.

ABOR discards staging and requests teardown. It replies with 426 and 225 for
an active transfer, or 225 when idle. It deliberately does not use 226 for an
abort. **226 means confirmed successful data close only.** QUIT discards any
staging, replies with 221, and requests graceful control close after the reply
queue drains. No command after QUIT in the same input span is executed.

## Call model

Call all functions from one serialized context. The API is not reentrant.
Input buffers must be valid for their stated length. The core does not retain
input pointers. Pass monotonic milliseconds modulo 2^32 to every timed call.
Call `ftp_core_tick()` regularly, including while TCP output is blocked.
Unsigned subtraction handles timer wrap. Do not suspend calls for a full
2^32-ms clock cycle.

1. Initialize caller-owned storage, then call
   `bool ftp_core_init(const ftp_storage_t *storage)`. Binding validates all
   seven callbacks, non-NULL context/ops/descriptor, and capacity in
   `1..min(UINT_MAX, UINT32_MAX)` before any callback or mutation. False
   preserves the previous binding and state; true clears protocol state only,
   with no backend mutation. Rebind only while quiescent: no session, pending
   actions/replies, live transport, independent staging, or retained views.
   The caller guarantees backend invariants and immutable descriptor/ops/context
   lifetime. `bool ftp_core_is_initialized(void)` is a side-effect-free query.
   Unbound session open fails; file inspection returns NULL/0/false.
   `void ftp_core_reset(void)` explicitly clears storage and protocol while
   retaining binding (unbound: protocol only). Release PCBs, retries and views
   first. It is not routine client/link cleanup.
   `ftp_tcp_start()` requires binding (ERR_VAL otherwise), never calls reset,
   and leaves cold-boot clear policy to the application.
2. Accept a control PCB only if `ftp_core_session_open(now)` returns true.
   On success, the core queues 220. A second client must not enter the core.
3. Feed control bytes with `ftp_core_control_span(bytes, length, now, &accepted)`.
   Each call is at most 256 bytes; split larger transport spans. The accepted
   count pointer is required. OK accepts all bytes. PARTIAL accepts exactly
   the reported prefix, ending at EPSV/PASV; keep the suffix without credit
   and resume after `ftp_core_passive_result()`. While opening, further input
   returns BACKPRESSURE with zero accepted bytes. BACKPRESSURE and REJECTED
   accept none and leave parser, login, transfer, replies, and timers unchanged.
   QUIT accepts/discards the rest of its span and never executes its tail.
4. Drain replies and transport actions. Report transport events as described
   below. Check actions after every event and after each timer tick.
5. On control EOF/RST or link loss, call `ftp_core_session_lost()`. Do not treat
   a control FIN as QUIT. On confirmed graceful control close, call
   `ftp_core_control_closed()`. Both retain the committed file.

Glue owns PCB pointers, callback registration, pending retry flags, receive
credits, and peer validation. **Detach old callbacks and clear old pending
retries before processing a new session or passive request.** The core does
not have generation tokens. Never deliver an old connection's result to a new
operation. Do not call a success completion after abort or an error.

## Bounded control parser and replies

The parser accepts fragmented and coalesced CRLF lines. The maximum line is
256 bytes including CRLF: 254 content bytes. NUL, other nonprintable bytes,
bare LF, embedded CR, and oversized lines mark a line invalid. The parser
keeps bounded state and discards excess content until CRLF. At CRLF it queues
500 and starts a new line. It never executes a malformed line. If a peer never
sends CRLF, the partial-line deadline closes the session.

The reply ring has eight 128-byte slots. No reply exceeds a slot. One slot can
contain multiple complete reply lines (FEAT or ABOR). Control input preflights
the entire span on a bounded parser copy. It reserves one slot per complete
line, plus one spare slot for a deferred result. It may return BACKPRESSURE
conservatively for a line that would not itself need a reply. Do not retry a
call that returned OK; on PARTIAL, never replay the accepted prefix. A deferred
passive reply is queued before any command in the retained suffix is executed.
On BACKPRESSURE, retain the bytes without receiving
credit and retry after output drains. If a span alone needs more than seven
reply slots, split that unaccepted span further before retry. If a larger pbuf
was split, track the accepted prefix; do not replay that prefix.

`ftp_core_reply_peek(&length)` returns the pending suffix of the head reply,
or NULL with zero length. Use a copying transport write. After that write
accepts N bytes, call `ftp_core_reply_consume(N, now)`. Partial consume is
supported. Zero or excessive consume returns false without change. Do not
consume on ERR_MEM or on a failed write. Reply memory can be reused after
consume; a no-copy control write is not safe. These counts concern bytes
accepted by TCP, not ACKs. A final control `tcp_close` must preserve the queued
TCP bytes. A queue overflow from an asynchronous event fails closed.

A 150 reply is queued before transfer work is enabled. The complete 150 must
be consumed and the data connection must be accepted before
`ftp_core_transfer_ready()` becomes true. An old aborted transfer's queued
150 cannot enable a new transfer. The core can retain old status replies
across ABOR; glue must preserve reply order.

## Transport actions

`ftp_core_take_actions()` returns and clears a bit mask. The actions are
requests, not evidence of network success. Process teardown actions before
open actions. CLOSE_PASSIVE can occur with OPEN_PASSIVE when a new request
replaces an old listener. A close request for an already detached listener is
harmless. Do not close a newly opened listener for the old close request.

| Action | Glue responsibility |
|---|---|
| OPEN_PASSIVE | Create and bind a passive listener. Call `ftp_core_passive_result()` once. |
| CLOSE_PASSIVE | Close/detach the old passive listener and cancel pending creation. |
| ABORT_DATA | Cancel pending data writes/close retries and abort/detach the old data PCB. |
| CLOSE_DATA | Flush pending output and enqueue FIN, retaining callbacks/ownership and a bounded retry if FIN allocation fails. Release ownership only after FIN enqueue succeeds. |
| CLOSE_CONTROL | Continuously credit/discard all held, refused, and new QUIT input; flush 221 and enqueue FIN with bounded retries, then release ownership. |
| ABORT_CONTROL | Abort/detach control PCB and release held control input. |

For passive success, supply the actual nonzero listener port and the MCU's
local IPv4 octets. PASV formats the octets and high/low port bytes. EPSV formats
only the port. Never advertise a port before bind/listen succeeds. Failure
queues 425 and tears down passive state. The passive deadline begins at the
command, not at successful creation. If the core rejects a result, close any
PCB made for that stale result.

Call `ftp_core_data_connected(now)` for an accepted passive data peer.
A false result means the connection must be rejected. A true result requests
listener close and sets the connected flag. A single passive operation never
accepts a second data connection.

## Upload and atomic replacement

For STOR, hold incoming data until `ftp_core_transfer_ready()` is true.
STOR calls backend `begin` before entering upload or queuing 150. A non-OK
result tears down the transfer with 451 and aborts staging.
`ftp_core_upload_chunk(bytes, length, now)` calls `append(context, bytes, length)`
with a byte count, not a string length. The backend copies all bytes or none. Binary NUL is valid.
Zero-length chunks are valid but do not refresh progress. A capacity violation
queues 552, aborts data, and discards staging. It never copies a chunk prefix.
Backend ERROR/INVALID_ARGUMENT/INVALID_STATE (and unexpected results) fail
closed with 451, abort staging, and preserve committed data. There are no
asynchronous backend retries. Invalid event/API calls return false without
modifying state.

On an orderly data receive EOF, call `ftp_core_upload_eof(now)`. If EOF arrives
before readiness, retain the event and retry after 150 drains. EOF sets
`ftp_core_eof_pending()` and requests CLOSE_DATA. It **does not commit**.
After **FIN enqueue succeeds** and PCB ownership is safely released, call
`ftp_core_data_close_result(true, now)`. This is not a wait for the remote FIN
ACK; a later transport failure does not undo an accepted close. Only this
confirmation calls backend `commit` once, after reply-space preflight. Only
commit OK permits 226. Commit failure queues 451, aborts staging, preserves the
old file and returns false; it never reports successful completion. If reply
space is exhausted, abort rather than publish. A later control-delivery failure
does not roll back a successful commit. A false close result
leaves staging pending for retry. After a close request has been taken, glue
owns its retry; the core does not re-emit it on every tick.

Do not call the successful close result for RST, a failed connection, or an
abort. Call `ftp_core_data_error(now)` for such events. A pending EOF remains
uncommitted if close never succeeds. Capacity error, RST, timeout, ABOR, QUIT,
control loss, and link loss all discard staging and retain the committed file.

## Download accounting

RETR captures the backend's stable committed `inspect` view and length; SIZE
and diagnostic `ftp_core_file` also use inspect, with `size_t` byte lengths.
For RETR, `ftp_core_download_span(&length)` returns the unsent committed suffix
only when ready. Prefer copying TCP writes. After a write accepts N bytes,
call `ftp_core_download_enqueued(N, now)`. On data TCP ACK callbacks, call
`ftp_core_download_acked(N, now)`. ACK counts must not exceed enqueued bytes.
Both calls reject invalid counts without side effects. Enqueued bytes are
not proof of delivery. Glue must not include SYN/FIN or control bytes in ACK
counts. The core requests CLOSE_DATA only when all file bytes are enqueued
and acknowledged. For an empty file, explicitly enqueue zero bytes to request
close. No 226 is queued until graceful close is confirmed.

The download span stays valid until transfer teardown. If glue uses no-copy
data writes, it must retain the backing storage until all references are
released. Do not start a replacement session while a detached TCP buffer still
references backend file memory. Backend buffers must not be accessed or mutated
independently while bound; release views before clear, commit, or reinit. The diagnostic `ftp_core_file()` pointer must not
be retained across a later successful upload or reset.

## Deadlines

All comparisons are `uint32_t(now - since) >= limit`. Zero-byte calls do not
extend progress. A limit fires at the exact boundary.

| Deadline | Limit | Start or refresh |
|---|---:|---|
| Control inactivity | 60 s | Session open; refresh on accepted nonempty control input |
| Total transfer | 60 s | Accepted STOR or RETR; never refresh |
| Partial control line | 10 s | First byte of a new line; never refresh |
| Passive wait | 15 s | EPSV/PASV command; covers open/listen and idle accepted data |
| Transfer progress | 15 s | STOR/RETR; refresh on connect, nonempty upload, enqueue, or ACK |
| Pending control output | 5 s | Queue becomes nonempty; refresh on actual consume progress |
| Pending download output | 5 s | Data connected and 150 drained; refresh on nonempty enqueue |
| Pending data close | 5 s | CLOSE_DATA request; never refresh on failed retries |
| Pending control close | 5 s | 221 queue drains; never refresh on failed retries |

The adapter also keeps a separate 5-s deadline per channel for failed
`tcp_output`: it starts on the first successful write needing output and clears
only on successful output. Further writes do not extend it. This covers both
uploads (blocked control replies) and downloads, even if the reply ring is
empty or all file bytes have been enqueued. Control expiry aborts the session;
data expiry aborts that transfer with 426. QUIT is subject to this bound too.

The core's pending download output means unsent file bytes remain. Waiting only for data
ACKs uses the 15-s progress deadline. Transfer progress/total deadlines still
apply during pending close. Control, partial-line, and control-output expiry
abort the session immediately. Data/passive expiry queues 426/425 and tears
down that operation. If that error reply cannot drain within 5 s, the control
session also closes. The glue must retry failed `tcp_output` or close calls
without busy loops, call tick regularly, and honor abort actions immediately.

## Pinned lwIP ownership contract

The production adapter is compile-time restricted to lwIP 2.2.1. Inspection of
`src/core/tcp.c` (`tcp_close_shutdown_fin`, `tcp_close`, `tcp_shutdown`) shows
that **both** `tcp_close` and TX-only `tcp_shutdown` convert FIN allocation
ERR_MEM to ERR_OK plus `TF_CLOSEPEND`. Public ERR_OK alone is not proof of FIN
enqueue. TX-only shutdown retains application ownership and avoids the unread
receive-data RST branch. The adapter checks the pinned private flag after
shutdown and, on allocation failure, clears it to prevent timer-driven close
behind the application's deadline/error checks. It retains callbacks and
retries in foreground. On success the PCB is in FIN_WAIT_1/LAST_ACK; callbacks
are detached and `tcp_close` releases ownership without another FIN allocation
or unread-data RST. No stale application pointer survives successful release.
The original 5-s close deadline is never refreshed by allocation failures.

While quitting, `ftp_core_quitting()` tells glue to credit/discard input rather
than parse it. The adapter drains its retained chain and uses the pinned private
`tcp_process_refused_data` helper outside TCP input to reoffer lwIP's refused
chain through that discard callback before closing. Every new chain is also
credited/discarded while QUIT is pending. Post-close traffic uses lwIP's default
receive handling. Upgrading lwIP requires re-review of these private contracts.

## Final extracted native test coverage

Builds and tests now use the relocated sources and public includes.
They explicitly initialize caller-owned storage.
The core suite also checks generic begin/append/commit failures, failed binding preservation,
unbound behavior, configurable names/capacity, and explicit reset.

The assertions cover login and command order; EPSV/PASV formatting; fragmented
and coalesced input; exact 256-byte line bounds; NUL and malformed lines;
queue backpressure with no partial effects; partial reply consumption; stale
150 isolation; empty files; binary NUL; 4096/4097-byte upload limits; repeated
replacement; download enqueue/ACK counts; close retries; and preservation on
capacity error, RST, ABOR, QUIT, control loss, and timeout. Deadline boundaries
run both at zero and across unsigned clock wrap. Reset clears the file.

The core suite is complemented by fake raw-API adapter, real lwIP memory-peer,
and platform suites; see [real lwIP validation](../../scripts/ethernet/ftp/ftp-lwip-validation.md).
Earlier extraction verification passed all five FTP suites (six executables) with ASan/UBSan
and compiled/linked a caller-owned external consumer with a different lwIP configuration
and built the real Arm Release FTP executable plus all four existing OFF targets.
See [final transport accounting](TRANSPORT_VALIDATION.md) and [public FTP guide](../../docs/ftp.md).
Latest independent cleanup verification passed ten native ASan/UBSan executables, three CLI tests, and 41 link Python tests.
The consumer remains compile/link only. No new firmware or physical evidence was added.
See the [current tooling guide](../../scripts/ethernet/README.md) for retained coverage and safe suite commands.
Physical link behavior for this image and an authorized hardware/client round trip remain unverified.
