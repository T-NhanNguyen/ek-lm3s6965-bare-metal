# Reusable FTP libraries

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

## Build reuse

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
See the [platform port contract](../examples/ethernet-ftp/PLATFORM_CONTRACT.md#lwip-architecture-port) for byte order, alignment, assertions, and NO_SYS limits.
Creating multiple archives does not create multiple runtime servers.

Sources can be reused across MCU examples with caller-selected startup and platform integration.
The repository's top-level CMake still targets the LM3S6965 board and its existing examples.
It is not a generic host-portability build system.
The external consumer fixture selects only `src/` and `include/` and supplies its own build environment.

## Initialization and lifetime

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

## RAM configuration

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

## Storage callbacks: state, timing, and effects

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

## Errors and completion

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
See the [detailed core/event API](../examples/ethernet-ftp/FTP_CORE_API.md) for parser, actions, and deadlines.

## Runtime limits and evidence

The server remains one foreground-serialized singleton with one control client and passive data owner.
It uses a dedicated NO_SYS IPv4 TCP stack pinned to lwIP 2.2.1,
commit `77dcd25a72509eb83f72b033d219b1d40cd8eb95`.
The adapter relies on private `TF_CLOSEPEND` and `tcp_process_refused_data` contracts.
Public `tcp_close` ERR_OK alone is not FIN-enqueue evidence.
An upgrade requires renewed close-state, callback, refused-input, and ownership review.

Link-down sweeps stack-owned active, TIME_WAIT, and bound TCP PCBs.
**No concurrent unrelated TCP service may share this stack.**
Extraction does not add multi-server, thread-safe, RTOS, or shared-stack support.

Earlier extraction verification passed five native ASan/UBSan suites, comprising six executable runs.
Latest independent tooling cleanup verification passed ten native ASan/UBSan executables, three CLI tests, and 41 link Python tests.
Run `./scripts/ethernet/test.sh ftp file platform` for FTP-related coverage, or default/`all` for all offline suites.
Use `./scripts/ethernet/test.sh consumer` for the compile/link probe.
The six removed wrappers duplicated plumbing only. Their unique coverage remains.
See the [tooling guide](../scripts/ethernet/README.md) for suite paths, prerequisites, and live-operation exclusions.
A caller-owned external consumer also compiled and linked with a different lwIP configuration and 113-byte RAM buffers.
That consumer was not executed. It proves compile/link reuse, not alternate-configuration runtime network behavior.
No new hardware evidence exists.
See [board build, settings, resource accounting, and future physical test](ftp.md).
The example owns `hello`, MCU `192.168.7.2/24`, and both 4096-byte buffers in `main.c`.
Those are example policy, not reusable-library configuration defaults.
