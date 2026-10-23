# FTP libraries and platform contracts

The RAM service and firmware receiver share a protocol core, not a storage transaction model. RAM replacement commits after accepted FIN. Async firmware success becomes durable before FIN. One serialized foreground owner services Ethernet, network timers and FTP. The dedicated pinned TCP stack cannot host unrelated TCP services.

## Find the contract

- [Library reuse and storage lifetime](#library-reuse-and-storage-lifetime)
- [Core events and deadlines](#core-events-and-deadlines)
- [TCP ownership](#tcp-ownership)
- [Platform integration](#platform-integration)
- [Platform capacity configuration](#platform-capacity-configuration)
- [Boot operation binding](#boot-operation-binding)
- [RAM example and authorized round trip](#ram-example-and-authorized-round-trip)
- [Measured-upload diagnostic API](#measured-upload-diagnostic-api)


## Library reuse and storage lifetime

The reusable sources are separate from the board example:

| Source | Public header | Responsibility |
|---|---|---|
| `src/ftp_core.c` | `include/lm3s6965/ftp_core.h` | Singleton protocol state, parsing, replies, deadlines |
| `src/ftp_tcp.c` | `include/lm3s6965/ftp_tcp.h` | Pinned lwIP raw TCP adapter and ownership |
| `src/ram_file.c` | `include/lm3s6965/ram_file.h` | Caller-owned double-buffered RAM storage |
| Storage interface | `include/lm3s6965/ftp_storage.h` | Synchronous single-file callbacks |

Use includes such as `#include "lm3s6965/ram_file.h"`.
The core owns no file buffers and has no lwIP or board dependency.
The RAM service has no permanent backing arrays and allocates no heap.
No flash, filesystem, asynchronous storage, or firmware-update support was added.

### Build reuse

`lm3s6965_ftp_core` and `lm3s6965_ram_file` are opt-in static archives.
They are excluded from the default build and do not link board startup, syscalls, or a linker script.
Build them explicitly, or link them from a consumer target.
The TCP factory compiles a separate adapter against the caller's selected lwIP target:

```cmake
# Define caller_lwip first, with its own stack sources/configuration.
# Export lwIP headers, lwipopts.h, arch/cc.h paths and configuration definitions
# through PUBLIC requirements (or INTERFACE requirements for an interface target).
lm3s6965_add_ftp_tcp_library(my_ftp_tcp caller_lwip)
target_link_libraries(my_application PRIVATE my_ftp_tcp lm3s6965_ram_file)
```

The adapter publicly links the core and caller lwIP target.
Their includes and configuration definitions reach the application transitively.
The factory does not import `examples/ethernet-ftp/lwipopts.h` or its architecture headers.
All stack sources, adapter, and application must use the same configuration.
The consumer owns `lwipopts.h`, `arch/cc.h`, and any `arch/sys_arch.h` required by its port.
The example port remains in `examples/ethernet-ftp/arch/`. Do not relocate it into tooling.
See the [platform port contract](#lwip-architecture-port) for byte order, alignment, assertions, and NO_SYS limits.
Creating multiple archives does not create multiple runtime servers.

Sources can be reused across MCU examples with caller-selected startup and platform integration.
The repository's top-level CMake still targets the LM3S6965 board and its existing examples.
It is not a generic host-portability build system.
The external consumer fixture selects only `src/` and `include/` and supplies its own build environment.

### Initialization and lifetime

Initialize storage before binding the core or starting TCP:

```c
static ram_file_t file;
static uint8_t buffer_a[4096];
static uint8_t buffer_b[4096];
static const char basename[] = "hello";

/* On either false result, stop startup and report a configuration failure. */
if (!ram_file_init(&file, buffer_a, buffer_b, sizeof buffer_a, basename)) {
    return;
}
if (!ftp_core_init(ram_file_storage(&file))) {
    return;
}
ftp_core_reset(); /* Optional explicit cold-boot clear policy. */
/* Initialize lwIP/netif; start TCP only when the local netif is ready. */
```

This is ordering guidance, not a complete application.
`ftp_core_init` validates the descriptor, non-NULL context, all seven callbacks, and capacity before mutation.
False preserves the prior binding and protocol/backend state.
True borrows the descriptor and resets protocol state without calling clear or abort.
Existing committed bytes therefore survive binding and the first `ftp_tcp_start`.
The backend must have no active upload.

Rebind only when quiescent: no session, pending replies/actions, live transport, staging upload, or retained views.
The core checks its protocol state but cannot detect all external references.
Keep descriptor, ops, context, buffers, and immutable basename alive while bound and while references remain.
Do not copy or move an initialized `ram_file_t`, edit its fields, or access its buffers independently while bound.
Its descriptor points back to that object.

`ftp_core_reset` clears the backend and protocol state but retains the binding.
When unbound, it resets protocol only. Release PCBs, retries, and views first.
Reset is not routine session or link cleanup.
`ftp_core_is_initialized` is a side-effect-free query. Unbound session open fails.
`ftp_tcp_start(NULL)` returns `ERR_ARG`. Missing binding returns `ERR_VAL`.
Start never binds or clears storage. Session loss and link-down/restart preserve committed bytes.

### RAM configuration

Capacity must be positive, fit `size_t`, and not exceed either `UINT_MAX` or `UINT32_MAX`.
Both buffers must provide that many writable contiguous bytes.
The two buffers, object, and basename including NUL must have disjoint address ranges.
Initialization rejects detectable overlap and address wrap before changing the object or buffers.
Allocation extents, alignment, writability, and basename readability remain caller preconditions.

The borrowed basename contains 1..63 ASCII letters, digits, periods, underscores, or hyphens.
`.` and `..` are invalid. The NUL terminator must occur within 64 readable bytes.
Matching is exact and case-sensitive: basename or one root slash plus basename only.
There is no trimming, decoding, or path normalization.
Never reinitialize while bound or while an old view remains.
`ram_file_storage(NULL)` returns NULL. A non-NULL object must have initialized successfully.

### Storage callbacks: state, timing, and effects

All callbacks are required, synchronous, bounded, serialized, and non-reentrant.
Use one foreground context. No ISR calls, concurrent reads/mutation, pending results, or worker threads are supported.
Atomic publication means indivisible replacement in this call model, not thread safety or power-loss durability.

| Callback | Required behavior |
|---|---|
| `matches_name` | Pure bounded byte-span match. Reject invalid names. |
| `clear` | Infallible. Cancel staging and make the file absent. Retain configuration. |
| `inspect` | Infallible for non-NULL output. Return a stable contiguous committed view. |
| `begin` | Start empty staging. Active upload returns `INVALID_STATE`. |
| `append` | Active upload required. Copy the whole chunk or none. Never retain input. |
| `abort` | Infallible and idempotent. Discard staging, retain committed bytes. |
| `commit` | Active upload required. OK atomically publishes all staging and ends upload. |

Failed begin, append, or commit leaves backend state unchanged.
Failed append/commit leaves staging active until abort.
`FTP_STORAGE_ERROR` is a recoverable synchronous failure, not a pending status or assertion.
Only append uses `FTP_STORAGE_CAPACITY` for insufficient space.
NULL append data is allowed only for zero length. Zero-length append is an active-upload no-op, not progress.
Input must not alias backend buffers or mutable state. Binary NUL bytes are allowed.

Absent inspect is `{NULL, 0, false}`.
An existing empty file has non-NULL data, zero length, and `exists=true`.
Lengths must not exceed capacity.
Views stay stable through begin, append, abort, failed operations, and repeated inspect.
Successful commit, clear, or permitted reinitialization invalidates them.
RAM commit swaps buffer roles. Clear and abort discard logically, not by secure erasure.
Release read/no-copy transport references before replacement, clear, or reinitialization.
A backend unable to meet these rules does not fit this interface.

### Errors and completion

STOR begins storage before entering upload or queuing 150.
Begin failure reports 451. Append capacity failure reports 552.
Other non-OK or unexpected begin/append/commit results fail closed with 451 and abort staging.
The old committed file remains available. There are no asynchronous backend retries.

Orderly receive EOF only requests data close.
After successful server FIN enqueue and safe PCB ownership release, the core calls commit once.
Reply-space preflight occurs before publication. If completion cannot be reported safely, staging is aborted.
Only commit OK permits upload 226. Commit failure returns false and never produces successful completion.
This does not wait for the remote FIN ACK. Later control-delivery failure does not undo a successful commit.
RST, timeout, ABOR, QUIT, control loss, and link loss before accepted close retain the old file.
Download completion requires all payload bytes enqueued and acknowledged, then accepted graceful close.
See the [detailed core/event API](#core-events-and-deadlines) for parser, actions, and deadlines.

### Runtime limits and evidence

The server remains one foreground-serialized singleton with one control client and passive data owner.
It uses a dedicated NO_SYS IPv4 TCP stack pinned to lwIP 2.2.1,
commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`.
The adapter relies on private `TF_CLOSEPEND` and `tcp_process_refused_data` contracts.
Public `tcp_close` ERR_OK alone is not FIN-enqueue evidence.
An upgrade requires renewed close-state, callback, refused-input, and ownership review.

Link-down sweeps stack-owned active, TIME_WAIT, and bound TCP PCBs.
**No concurrent unrelated TCP service may share this stack.**
Extraction does not add multi-server, thread-safe, RTOS, or shared-stack support.

Run `./scripts/ethernet/test.sh ftp file platform` for FTP-related coverage, or default/`all` for all offline suites.
Use `./scripts/ethernet/test.sh consumer` for the compile/link probe.


## Core events and deadlines

### Service rules

Accept one control client at a time. Allow only `USER anonymous`, then `PASS`.
The password has no security function and may be empty. Login starts without
binary mode. Require `TYPE I` before STOR or RETR. Reject TYPE A. It also clears
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

### Call model

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
   preserves the previous binding and state. True clears protocol state only,
   with no backend mutation. Rebind only while quiescent: no session, pending
   actions/replies, live transport, independent staging, or retained views.
   The caller guarantees backend invariants and immutable descriptor/ops/context
   lifetime. `bool ftp_core_is_initialized(void)` is a side-effect-free query.
   Unbound session open fails. File inspection returns NULL/0/false.
   `void ftp_core_reset(void)` explicitly clears storage and protocol while
   retaining binding (unbound: protocol only). Release PCBs, retries and views
   first. It is not routine client/link cleanup.
   `ftp_tcp_start()` requires binding (ERR_VAL otherwise), never calls reset,
   and leaves cold-boot clear policy to the application.
2. Accept a control PCB only if `ftp_core_session_open(now)` returns true.
   On success, the core queues 220. A second client must not enter the core.
3. Feed control bytes with `ftp_core_control_span(bytes, length, now, &accepted)`.
   Each call is at most 256 bytes. Split larger transport spans. The accepted
   count pointer is required. OK accepts all bytes. PARTIAL accepts exactly
   the reported prefix, ending at EPSV/PASV. Keep the suffix without credit
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

### Bounded control parser and replies

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
call that returned OK. On PARTIAL, never replay the accepted prefix. A deferred
passive reply is queued before any command in the retained suffix is executed.
On BACKPRESSURE, retain the bytes without receiving
credit and retry after output drains. If a span alone needs more than seven
reply slots, split that unaccepted span further before retry. If a larger pbuf
was split, track the accepted prefix. Do not replay that prefix.

`ftp_core_reply_peek(&length)` returns the pending suffix of the head reply,
or NULL with zero length. Use a copying transport write. After that write
accepts N bytes, call `ftp_core_reply_consume(N, now)`. Partial consume is
supported. Zero or excessive consume returns false without change. Do not
consume on ERR_MEM or on a failed write. Reply memory can be reused after
consume. A no-copy control write is not safe. These counts concern bytes
accepted by TCP, not ACKs. A final control `tcp_close` must preserve the queued
TCP bytes. A queue overflow from an asynchronous event fails closed.

A 150 reply is queued before transfer work is enabled. The complete 150 must
be consumed and the data connection must be accepted before
`ftp_core_transfer_ready()` becomes true. An old aborted transfer's queued
150 cannot enable a new transfer. The core can retain old status replies
across ABOR. Glue must preserve reply order.

### Transport actions

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
| CLOSE_CONTROL | Continuously credit/discard all held, refused, and new QUIT input. Flush 221 and enqueue FIN with bounded retries, then release ownership. |
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

### Upload and atomic replacement

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
ACK. A later transport failure does not undo an accepted close. Only this
confirmation calls backend `commit` once, after reply-space preflight. Only
commit OK permits 226. Commit failure queues 451, aborts staging, preserves the
old file and returns false. It never reports successful completion. If reply
space is exhausted, abort rather than publish. A later control-delivery failure
does not roll back a successful commit. A false close result
leaves staging pending for retry. After a close request has been taken, glue
owns its retry. The core does not re-emit it on every tick.

Do not call the successful close result for RST, a failed connection, or an
abort. Call `ftp_core_data_error(now)` for such events. A pending EOF remains
uncommitted if close never succeeds. Capacity error, RST, timeout, ABOR, QUIT,
control loss, and link loss all discard staging and retain the committed file.

### Download accounting

RETR captures the backend's stable committed `inspect` view and length. SIZE
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
independently while bound. Release views before clear, commit, or reinit. The diagnostic `ftp_core_file()` pointer must not
be retained across a later successful upload or reset.

### Deadlines

All comparisons are `uint32_t(now - since) >= limit`. Zero-byte calls do not
extend progress. A limit fires at the exact boundary.

| Deadline | Limit | Start or refresh |
|---|---:|---|
| Control inactivity | 60 s | Session open. Refresh on accepted nonempty control input |
| Total transfer | 60 s | Accepted STOR or RETR. Never refresh |
| Partial control line | 10 s | First byte of a new line. Never refresh |
| Passive wait | 15 s | EPSV/PASV command. Covers open/listen and idle accepted data |
| Transfer progress | 15 s | STOR/RETR. Refresh on connect, nonempty upload, enqueue, or ACK |
| Pending control output | 5 s | Queue becomes nonempty. Refresh on actual consume progress |
| Pending download output | 5 s | Data connected and 150 drained. Refresh on nonempty enqueue |
| Pending data close | 5 s | CLOSE_DATA request. Never refresh on failed retries |
| Pending control close | 5 s | 221 queue drains. Never refresh on failed retries |

The adapter also keeps a separate 5-s deadline per channel for failed
`tcp_output`: it starts on the first successful write needing output and clears
only on successful output. Further writes do not extend it. This covers both
uploads (blocked control replies) and downloads, even if the reply ring is
empty or all file bytes have been enqueued. Control expiry aborts the session.
data expiry aborts that transfer with 426. QUIT is subject to this bound too.

The core's pending download output means unsent file bytes remain. Waiting only for data
ACKs uses the 15-s progress deadline. Transfer progress/total deadlines still
apply during pending close. Control, partial-line, and control-output expiry
abort the session immediately. Data/passive expiry queues 426/425 and tears
down that operation. If that error reply cannot drain within 5 s, the control
session also closes. The glue must retry failed `tcp_output` or close calls
without busy loops, call tick regularly, and honor abort actions immediately.

### Pinned lwIP ownership contract

The production adapter is compile-time restricted to lwIP 2.2.1. Inspection of
`src/core/tcp.c` (`tcp_close_shutdown_fin`, `tcp_close`, `tcp_shutdown`) shows
that **both** `tcp_close` and TX-only `tcp_shutdown` convert FIN allocation
ERR_MEM to ERR_OK plus `TF_CLOSEPEND`. Public ERR_OK alone is not proof of FIN
enqueue. TX-only shutdown retains application ownership and avoids the unread
receive-data RST branch. The adapter checks the pinned private flag after
shutdown and, on allocation failure, clears it to prevent timer-driven close
behind the application's deadline/error checks. It retains callbacks and
retries in foreground. On success the PCB is in FIN_WAIT_1/LAST_ACK. Callbacks
are detached and `tcp_close` releases ownership without another FIN allocation
or unread-data RST. No stale application pointer survives successful release.
The original 5-s close deadline is never refreshed by allocation failures.

While quitting, `ftp_core_quitting()` tells glue to credit/discard input rather
than parse it. The adapter drains its retained chain and uses the pinned private
`tcp_process_refused_data` helper outside TCP input to reoffer lwIP's refused
chain through that discard callback before closing. Every new chain is also
credited/discarded while QUIT is pending. Post-close traffic uses lwIP's default
receive handling. Upgrading lwIP requires re-review of these private contracts.


## TCP ownership

`src/ftp_tcp.c` implements `include/lm3s6965/ftp_tcp.h` against the populated
lwIP 2.2.1 pin. `ftp_tcp_start(const ip4_addr_t *address)` requires prior
`ftp_core_init`: ERR_ARG for NULL address, ERR_VAL for missing binding. It never
initializes/clears storage. Explicit boot reset is caller policy. Link-down and
restart retain the binding and committed file. Storage ownership/lifetime are
not transport responsibilities. Public headers use `lm3s6965/` include paths.
All entry points and raw callbacks belong to the serialized foreground. SysTick
only supplies time. There is one port-21 listener, one control owner and one
passive data owner. Passive listeners bind port zero and advertise the actual
nonzero port after listen succeeds. Data peers must match the control IPv4.
Extra connections are aborted with `ERR_ABRT` without entering the core.

Static callback channels hold at most one accepted pbuf chain each. Further
chains return `ERR_MEM` without any effects. Each chain's accepted prefix is
credited exactly once. Its cursor survives parser backpressure. Control work is
bounded to 256 bytes per service and upload work to one 256-byte copying chunk.
Receive chains are freed once on complete consumption or teardown. Early data
and FIN wait for STOR and the complete 150 reply. Failed control output also
holds data work. QUIT continuously credits/discards retained, lwIP-refused,
and newly arriving control input, not only at the close action. The pinned
private `tcp_process_refused_data` helper reoffers refused chains in foreground
before close, so 221 is preserved without an unread-data RST.

Both reply and download writes use COPY. Cursors advance only on successful
`tcp_write`. Output failures retain output work, never replay accepted bytes.
Each service makes at most one write/output/close attempt per channel. Download
close is enabled only after all payload ACKs (including the empty-file case).
Normal download peer transmit FIN is a half-close, not an ACK or failure.
Upload FIN does not commit: only accepted FIN enqueue confirms completion to
the core, not a remote FIN ACK. In pinned lwIP 2.2.1, both `tcp_close` and TX-only
`tcp_shutdown` mask FIN allocation ERR_MEM as ERR_OK plus TF_CLOSEPEND. This is
**not** sufficient for success. TX-only shutdown retains application ownership.
the adapter checks and clears the pinned private TF_CLOSEPEND flag on failure,
preventing autonomous timer retries while keeping callbacks/error observation.
Core's original 5-s close deadline bounds foreground retries. After successful
FIN enqueue, the PCB is in FIN_WAIT_1/LAST_ACK. Detach callbacks and release it
with `tcp_close` (no allocation or unread-data RST in those states). No app
pointer is used after release. A compile-time version check requires re-review
before upgrading lwIP.

Each channel separately bounds failed `tcp_output` at 5 s from its first pending
write, without resetting the deadline on further writes or reply-ring draining.
Successful output clears it. Control expiry aborts the session. Data expiry
aborts the transfer with 426. This applies consistently to uploads/downloads.
The core's whole-span input API reports accepted prefixes and stops at deferred
EPSV/PASV until the listener result is queued, preserving reply FIFO.

Abort invalidates ownership before calling TCP. `tcp_err` operates only on
static state, never on the already-freed PCB. Successful close detaches all
callbacks. Link-down teardown uses `tcp_abandon(reset=0)` to avoid network
submission. It also clears lwIP-owned active, TIME_WAIT and bound PCB lists so
previous successful closes cannot leave queued FIN/data into the next epoch.
**This sweep assumes this dedicated firmware has no other TCP users**, as in
its current build. Other TCP services cannot coexist on this stack. This singleton extraction
does not introduce a coexistence or multi-server abstraction. Listening PCBs use unconditional lwIP LISTEN close,
never abort. Link loss, control loss, timeout and ABOR discard staging and retain
the committed file. Teardown is idempotent.


## Platform integration

### Ownership and build

`LM3S6965_BUILD_ETHERNET_FTP` defaults OFF. Only the guarded examples
subdirectory references lwIP. Existing targets do not depend on it. No dependency
fetch is performed. Inspected populated pin: lwIP 2.2.1,
`77dcd25a72509eb83f72b033d219b1d40cd8eb95`. Upstream `Filelists.cmake` is deliberately
not included because it generates files inside the submodule.

The ON configuration builds `lm3s6965_ftp_lwip` and
`lm3s6965_ftp_platform` OBJECT libraries plus the real `lm3s6965_ethernet_ftp`
executable. It links those objects and the reusable core, RAM, and TCP archives.
Sources are `src/ftp_core.c`, `src/ram_file.c`, and `src/ftp_tcp.c`.
The TCP factory consumes the lwIP target's public usage requirements.
See the [library guide](ftp.md) for caller-config reuse.
`main.c` owns the immutable `hello` basename, address policy, and two 4096-byte buffers.
It initializes RAM, binds the core, then explicitly resets for cold boot before networking.
First start and link-down/restart never clear the bound file.

The FTP link selects only BSP startup, system_control, gpio, and ethernet sources.
it does not link `lm3s6965_bsp`, old syscalls, UART, trace, OLED, or FreeRTOS.
Old targets keep their old SysTick, heap and linker behavior. Do not replace
this with the shared BSP object library: it carries incompatible syscall and
linker ownership. The target-local `_sbrk` overrides nosys for this executable.
No runtime stdio is used. LwIP diagnostics are suppressed and assertions trap.

### lwIP architecture port

`arch/cc.h` belongs to this example, not the reusable library or test tooling.
It selects `BYTE_ORDER LITTLE_ENDIAN` for Cortex-M3/GCC and the deterministic little-endian native test host.
`LWIP_DECLARE_MEMORY_ALIGNED` uses GCC's aligned attribute with `MEM_ALIGNMENT`.
The firmware alignment is four bytes. Native shared shims use eight bytes for 64-bit pointers.
`LWIP_PLATFORM_DIAG` suppresses diagnostics. `LWIP_PLATFORM_ASSERT` traps with `__builtin_trap()`.
Neither macro adds stdio, allocation, UART/ITM waits, or ISR protocol work.
Native builds disable lwIP byteorder aliases to avoid Darwin header collisions.
That build setting does not change the firmware byte order.

`arch/sys_arch.h` documents the NO_SYS port. It provides no semaphore, mailbox, thread, or interrupt-protection services.
`sys_now()` comes from `net_clock.c`. All lwIP calls remain serialized in foreground.
A reusable consumer supplies its own port configuration and exports it through its lwIP target.
The factory does not import this example's architecture headers. Keep these files with the example.

### Transport worker contract

`ftp_tcp.c` implements the three foreground-only functions in `ftp_tcp.h`:

- `err_t ftp_tcp_start(const ip4_addr_t *)`: start listeners bound to the supplied
  board address. Main calls it only when MAC ready and retries on failure on
  subsequent loops. Failure must leave no partial listeners. Success must be
  idempotent within the readiness epoch.
- `void ftp_tcp_service(uint32_t)`: bounded, nonblocking service every loop,
  including while down. Time is modular unsigned milliseconds. No printf,
  delays, socket/netconn calls or protocol work from interrupts.
- `void ftp_tcp_link_down(void)`: synchronous teardown after administrative and
  link down and ARP flush, before processing further RX. Abort and release all
  control/data/listening PCBs, retained pbufs and per-connection state. Do not
  submit network traffic. Safe before successful start. A new ready epoch can
  start cleanly. TCP abort callback rules and pbuf ownership remain transport
  adapter responsibilities. Do not rely on lwIP netif down to close TCP PCBs.

Main starts listeners after the ready transition, calls `sys_check_timeouts()`
and FTP service **every** iteration. The netif service performs at most one PHY
poll per 10 ms and consumes at most eight RX results per iteration, including
invalid/dropped frames. A timeout or `mac_ready == false` means not ready even if
PHY link and autonegotiation flags are true. Link loss sets both flags down,
flushes ARP, and calls teardown once. Down RX is consumed/discarded without pbuf
allocation. Runtime link recovery does not reinitialize the driver.

Address `192.168.7.2/24`, gateway zero, no DNS. Local test MAC
`02:00:00:69:65:02`. Single netif, one foreground owner. Separate 1514-byte RX/TX
scratch arrays are necessary because RX processing can trigger TX. Driver reads
copy into RX scratch. `pbuf_take` supports chained buffers. Input ERR_OK transfers
ownership. Input error frees the packet. Allocation failure drops the consumed
frame. TX copies the chain with `pbuf_copy_partial`. 14..1514 bytes only. The
caller retains all TX pbufs on every return. Driver BUSY maps to ERR_MEM. Other
driver failures map ERR_IF. Successful submission is not delivery evidence.
Firmware's 1536-byte pool payload normally makes one RX pbuf per frame, but the
adapter still handles chains. Checksums are software-generated/validated.

### Timer contract and limits

`net_clock.c` owns a 4-byte-aligned volatile uint32 counter. Only SysTick writes
it after initialization. The ISR increments time only. Aligned Cortex-M3 word
loads avoid tearing. Volatile preserves asynchronous compiler visibility, not
general C synchronization or compound-operation atomicity. Initialize once
before networking. No concurrent clock resets or CPU clock changes.

PLL configuration selects 50 MHz on success, 8 MHz main crystal on bounded PLL
failure. Main verifies RCC source/bypass/divider readback and rejects RCC2
USERCC2 override before enabling networking. This verifies configuration, not
physical frequency. CPU-clock SysTick CTRL=7 (ENABLE, TICKINT, CLKSOURCE),
CURRENT=0, RELOAD=49999 or 7999 respectively. Initialization rejects zero,
nonintegral-kHz and out-of-field periods. No COUNTFLAG polling. `sys_now()` reads
the accumulated ISR counter once. Native tests explicitly run ticks without
foreground sampling and force uint32 wrap.

`net_clock_elapsed` uses `(uint32_t)(now - since) >= interval`, with interval and
observation gaps both below 2^31 ms. Ambiguous long intervals are rejected.
PHY reschedules from the latest sample. No catch-up polling burst. Transport
must obey the same bound for its own elapsed comparisons. LwIP 2.2.1 timeouts use
`sys_now`, signed modular deadline ordering, and foreground timeout checking.
TCP has 250 ms timer cadence and ARP has 1 s cadence. Four timeout objects cover
this fixed configuration. No custom FTP lwIP timers are provisioned. FTP uses its own foreground timestamps. Adding lwIP timers requires revisiting the timeout budget.

Foreground service is bounded by eight driver reads, one bounded MII poll when
due, protocol input and service callbacks, not by measured wall time. Driver
MII waits are finite iteration counts, not latency guarantees. Interrupt
masking, disabled interrupts, pending tick coalescing and debug halt can lose
elapsed time. Source and host tests do not measure ISR latency, oscillator rate,
or worst-case timeout gaps on hardware.

### Fixed stack capacities

`lwipopts.h` fixes: lwIP heap 12 KiB. Pbuf pool 8 x 1536. Separate pbuf metadata
pool 8. TCP PCB 8, listen PCB 2, segments 24, timeouts 4. TCP MSS 536,
window/send buffer 2144, send queue 16, out-of-order queue off, oversize zero.


## Platform capacity configuration

ARP entries are 4 with queueing off. The fixed configuration has no UDP, DHCP,
DNS, IPv6, IP fragmentation/reassembly, raw-IP, sockets, netconn, ALTCP, FreeRTOS
or libc allocation for lwIP. The standalone C heap is 8192 bytes total and the
stack reservation is 8192 bytes. See [memory ownership](boot-memory-layout.md).

lwIP heap, pools, file buffers, packet scratch and their metadata are already
static BSS allocations. Do not add them again to final BSS. GNU `size` BSS can
also include NOLOAD heap and stack reservations, so it is not the actual static
BSS count. Inspect the final map and file-backed PT_LOAD extents. Neither those
counts nor configured capacities establish runtime high-water or fragmentation.


## Boot operation binding

### Boundary and build

`include/lm3s6965/ftp_boot.h` / `src/ftp_boot.c` bind **real** status capture/render,
confirmation parser/lifecycle/store and firmware verifier/flash/store/receiver to
production `ftp_core` and pinned raw lwIP 2.2.1. `ftp_storage_ops_t` is unchanged:
RAM `hello` remains synchronous, whole-chunk append and commit-after-FIN.
`ftp_operation.h` is a separate optional async interface, not flash pretending
to be RAM. Link opt-in `lm3s6965_ftp_boot` plus the existing TCP factory target
and verifier/crypto closure. Neither archive is an enabled updater/boot image.
Standalone HOST archive entry: `cmake -S tests/boot -B /tmp/ftp-binding-build-debug
-DCMAKE_BUILD_TYPE=Debug`, then `cmake --build /tmp/ftp-binding-build-debug`.

### Startup, ownership and foreground schedule

1. The actual application owner supplies an initialized real flash controller,
   a fresh `BOOT_RECEIVE_INIT` receiver, initialized/preserved diagnostic
   publication and coherent metadata/running sources. Running evidence must be
   the authenticated **actually executing** image, never a selected-role guess.
2. Zero-initialize `ftp_boot_t` once and call `ftp_boot_init`. Initialize RAM
   core storage as before, then bind `ftp_core_bind_operation(ftp_boot_operation(&binding))`
   while the transport/core are quiescent. Immutable descriptors and all source
   objects outlive the binding. No stack-local borrowed descriptors.
3. Service Ethernet, timers, then `ftp_tcp_service(now)` each foreground turn.
   The latter drains a bounded input prefix and calls `ftp_core_foreground(now)`
   once. Do not also call it separately in the same turn. Non-TCP owners call
   `ftp_core_foreground` themselves after servicing Ethernet/timers.
4. Continue the **same APPLICATION watchdog object, epoch, stream history and
   partially accumulated genuine evidence**. The binding does not expose, copy,
   initialize, feed, service or transfer that object, fabricate progress, replay
   reset accounting or write metadata per handoff. Caller application lifetime
   and watchdog scheduling remain explicit obligations.

Every service path through the binding shares an active owner and callback
reentry guard. Status/confirm cannot bypass a receiving transaction. The core
rejects concurrent commands and the binding rejects concurrent begins. External
users must also obey that owner: direct `boot_confirm_apply`, snapshot/receive,
flash or diagnostic calls, alternate bindings, ISR/DMA/debugger mutation cannot
bypass it. C structs are not a provenance/authentication mechanism.

The default `examples/ethernet-ftp/main.c` path binds **RAM hello only**.
Opt-in foreground/A/B variants supply different application owners and real boot
binding. The binding library itself does not install startup, controller ports,
a candidate deadline or a reset actuator. Those consumers must preserve the
[startup contract](boot-startup.md) and [update contract](boot-reset-image.md).
Library compilation alone does not prove physical timing, durability or fit.

### Wire and cancellation decisions

All new names require TYPE I: `RETR boot-status`, `STOR firmware`, `STOR confirm`
(with optional single leading slash). Status is rendered into owned 1024-byte
binding storage at begin and remains stable until ACK/FIN/teardown. No SIZE
extension is invented. Confirmation remains restricted-bench unsigned.

Firmware feed reports the exact `boot_receive_feed` accepted prefix end-to-end.
Only those bytes receive TCP credit. The adapter retains the suffix, including
zero-accept WAIT, without failure, timer refresh or synthetic progress. It holds
one pbuf chain and lwIP may refuse the next. Retained and refused chains drain
before exactly-once EOF. FIN-bearing refused chains are reoffered explicitly.
There is no erase/program/crypto loop in a raw callback. One foreground step
may still be an **opaque synchronous signature/store/full stored-verification
gap**. Bounded bytes/actions do not prove elapsed service or watchdog margins.

Pinned lwIP receive accounting includes FIN sequence space in `TCP_TCPLEN`.
When data is refused, `PBUF_FLAG_TCP_FIN` defers FIN delivery and its window
correction. Successful reoffer restores FIN's one byte and delivers the NULL
callback. That is transport ownership acceptance, not consumption of all retained
payload. Delay backend EOF until retained payload drains. Credit payload only
as the core consumes it and never call `tcp_recved` for FIN itself. Check these
boundaries explicitly when validating partial WAIT and refused chains.

WAIT keeps existing **15 s accepted-byte idle / 60 s total transfer** limits.
foreground erase/crypto activity is not accepted-byte progress. Existing 5 s
output/close, 15 s passive and finite reply/buffer limits remain. This may reject
a slow physical device rather than silently extending a safety deadline.

Async success is durable **before FIN**: firmware must publish READY with stored
verification and committed readback. Confirmation must durably apply/readback
or match an existing confirmed identity with no write. Only then is FIN
requested. **226 requires accepted graceful FIN plus durable success**. This
intentionally differs from RAM's commit-after-FIN and avoids pretending flash
can roll back after terminal acceptance. No actual reset occurs here.
`reset_requested` and owned firmware acceptance are sticky from durable acceptance,
independent of final reply delivery. The application reset actuator must eventually
honor that intent even if FIN/reply is lost. It must not wait forever for 226.
The binding does not choose a physical reset time or claim trial execution.

ABOR/QUIT/control EOF/RST, data error, linkdown, timeout or failed close end a
successful begin exactly once. Before firmware acceptance this invokes receiver
abort once, without cleanup IO. Terminal receiver duplicates cannot replay IO.
After acceptance end never calls abort, clears intent or claims rollback. ABOR
while accepted says 'Operation already accepted'. Other transport errors describe
transport failure only. A lost reply is an uncertain client outcome to resolve
by fresh status, never grounds for destructive blind retry.

One receiver lifetime permits **one admitted firmware attempt**, successful or
not. It is never silently reinitialized. A new independently authorized lifetime
is a caller action, not an automatic retry. RECONCILE or blocked-controller
failure quarantines **all three** binding services even after disconnect. There
is no clear-quarantine/revive/replay API. External explicit authority/controller
reconciliation and new lifetime policy are required. Reconciliation cannot turn
an old failed transaction into accepted publication.


## RAM example and authorized round trip

### Opt-in build

`LM3S6965_BUILD_ETHERNET_FTP` defaults to `OFF`.
Existing bare-metal, FreeRTOS, link, and RAW targets do not depend on lwIP.
The separate executable is `lm3s6965_ethernet_ftp`.
CMake does not fetch dependencies. Use the official Arm GNU Toolchain with newlib.
The final offline verification used Arm GNU Toolchain 15.3.rel1.
See [toolchain setup](toolchain.md).

For a later authorized dependency initialization and offline build, run from the repository root:

```sh
git submodule update --init third_party/lwip
git -C third_party/lwip rev-parse HEAD
cmake -S . -B build/ftp -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_ETHERNET_FTP=ON
cmake --build build/ftp --target lm3s6965_ethernet_ftp --parallel 4
```

Initialization can download sources. Use the preservation checks in
[dependency setup](toolchain.md#pinned-dependency-setup) before changing an
existing checkout.
The required lwIP 2.2.1 pin is `77dcd25a72509eb83f72b033d219b1d40cd8eb95`, tag `STABLE-2_2_1_RELEASE`.
The gitlink pins the commit. Do not update to a moving branch or a different release.
The ELF is `build/ftp/examples/ethernet-ftp/lm3s6965_ethernet_ftp`.
Its directory also contains `.bin`, `.hex`, and `.map` files.
Building does not flash or test the board.

### Approved service contract

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
See the [core API](#core-events-and-deadlines) for command ordering and deadlines.

### Direct-link settings and evidence

The bench topology is a direct cable from a host USB Ethernet adapter to the MCU.
The firmware selects MCU `192.168.7.2/24`, gateway zero and no DNS, with local test
MAC `02:00:00:69:65:02`. Select a compatible host address such as `192.168.7.1/24`
only under separate host-configuration authority. Do not assume the example
interface `en7` identifies the adapter on another host. See
[Ethernet diagnostics](../scripts/ethernet-diagnostics.md) for distinct link and
RAW procedures. Configured addresses and PHY negotiation do not establish IP or FTP.

### Future client round trip: separate authorization required

**Start this procedure ONLY after separately authorized flashing and network testing.**
Confirm the FTP image, direct-link settings, address uniqueness, and network readiness first.
This page does not authorize flashing, reset, host configuration changes, or traffic.
Do not use RAW/link reset helpers against the FTP image.

Check `curl --version` on the test host and stop if `ftp` is absent from
`Protocols`.
No working standalone `ftp` CLI syntax is claimed.

Use an existing local `hello` file. Run the following only in the separately authorized physical test.
The download goes into a new temporary directory, never over the input file.
`--disable` ignores curl configuration. `--noproxy '*'` prevents proxy use.
Passive mode is explicit. Curl uses binary transfer by default for these URLs.
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


## Measured-upload diagnostic API

`LM3S6965_FTP_DIAGNOSTIC` defaults to OFF. This is instrumentation only: no
FTP timeout, boot security, scheduling, watchdog, flash or reset policy changes.
No console output, endpoint, allocation, new hardware register or timer exists.

### Record and lifetime

With ON, halt the foreground owner and inspect the symbol
`ftp_diagnostic_record`. It is 52 bytes: thirteen consecutive `uint32_t` fields
in the order defined in `include/lm3s6965/ftp_diagnostic.h`:

- `version` (1), `armed`.
- `auth_entry_valid`, `auth_exit_valid`, `cause_valid`.
- `auth_entry_ms`, `auth_exit_ms`, `auth_duration_ms`.
- `auth_entry_phase`, `auth_exit_phase`, `auth_result` (boot_receive enum values).
- `first_cause` (ftp_diagnostic_cause_t), `cause_ms`.

Valid flags publish after their fields with compiler fences. Inspect while
halted, not through a concurrent live reader. This is ordinary static RAM,
retained across status requests, session changes and teardown, **not across
reset/startup**. No linker-retained region is added.

Only a successful firmware operation begin arms/clears it. Status, report,
confirm, RAM uploads and rejected firmware begins do not. Firmware end disarms
without clearing evidence. The first selected instrumented failure branch
freezes the cause, before teardown. Subsequent errors cannot overwrite it.
Normal success has no failure cause. This is not a general log of every public
core API call (for example externally requested core reset/session_lost).

AUTH samples wrap the entire synchronous `boot_receive_step` selected while
its incoming phase is AUTHENTICATE, including success/failure paths. Exit phase
and result distinguish INVALIDATE/OK from TERMINAL/failure. The production A/B
binder supplies a context clock callback returning existing `sys_now()`.
**These are software tick milliseconds, not wallclock measurements.** A blocked
foreground software-clock producer can under-report a synchronous gap, even as
zero. Duration uses unsigned exit minus entry modulo 2^32. TCP/core cause times
use their existing service/tick argument. They are not a fresh post-AUTH sample.

TCP preserves control-error/deadline/FIN precedence over data error and core
tick. An already-set error is identified as error. Otherwise an expired output
deadline sets failure before FIN selection. Core tick preserves control idle,
parser, reply output, control close, data close, data output, total transfer,
progress and passive deadline precedence. ABOR/QUIT and operation failures are
also identified before teardown. The record does not alter those decisions.

### Offline gate

Run separately so ON/OFF boot archives never share a build directory:

```sh
FTP_TEST_DIAGNOSTIC=0 ./scripts/ethernet/test.sh core tcp lwip
FTP_TEST_DIAGNOSTIC=1 ./scripts/ethernet/test.sh core tcp lwip
```

Workers may prefix each with `FTP_TEST_BUILD_ONLY=1`. That compiles/links but
executes no tests. Unit coverage includes unarmed/OFF no-effects, entry/exit,
unsigned wrap, failure fields and first-cause freeze. TCP/real-lwIP fixtures
cover FIN, output-deadline-before-FIN, post-AUTH progress expiry before commit,
and status reads preserving evidence.

### Isolated later diagnostic build

Do not reuse or reconfigure a measured baseline directory. For a key-free
archive/platform compile check (not a production image):

```sh
cmake -S . -B build/ftp-diagnostic-compile \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-lm3s6965.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLM3S6965_BUILD_ETHERNET_FTP=ON \
  -DLM3S6965_FTP_FIRMWARE_RECEIVE=ON -DLM3S6965_FTP_DIAGNOSTIC=ON
cmake --build build/ftp-diagnostic-compile --parallel 4 --target \
  lm3s6965_ftp_core lm3s6965_ftp_boot lm3s6965_ftp_tcp lm3s6965_ftp_platform
```

For the future approved production build, configure a **new**
`build/ab-ftp-diagnostic` directory using the same approved production options,
public trust-root selection and compiler/build type as the baseline, adding
`-DLM3S6965_FTP_DIAGNOSTIC=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. Build only the
intended `lm3s6965_ab_ftp_a` or `lm3s6965_ab_ftp_b` target. The core's PUBLIC
compile definition propagates to the boot/TCP archives and A/B platform object
compilations, including `ftp_production.c`. Inspect compile_commands to confirm.
Image fit, debugger symbol retention and the approval for any subsequent
upload/reset remain separate gates. These instructions do not authorize a
hardware action.
