# Raw TCP transport integration (offline)

`src/ftp_tcp.c` implements `include/lm3s6965/ftp_tcp.h` against the populated
lwIP 2.2.1 pin. `ftp_tcp_start(const ip4_addr_t *address)` requires prior
`ftp_core_init`: ERR_ARG for NULL address, ERR_VAL for missing binding. It never
initializes/clears storage. Explicit boot reset is caller policy; link-down and
restart retain the binding and committed file. Storage ownership/lifetime are
not transport responsibilities. Public headers use `lm3s6965/` include paths.
All entry points and raw callbacks belong to the serialized foreground; SysTick
only supplies time. There is one port-21 listener, one control owner and one
passive data owner. Passive listeners bind port zero and advertise the actual
nonzero port after listen succeeds. Data peers must match the control IPv4.
Extra connections are aborted with `ERR_ABRT` without entering the core.

Static callback channels hold at most one accepted pbuf chain each. Further
chains return `ERR_MEM` without any effects. Each chain's accepted prefix is
credited exactly once; its cursor survives parser backpressure. Control work is
bounded to 256 bytes per service and upload work to one 256-byte copying chunk.
Receive chains are freed once on complete consumption or teardown. Early data
and FIN wait for STOR and the complete 150 reply; failed control output also
holds data work. QUIT continuously credits/discards retained, lwIP-refused,
and newly arriving control input, not only at the close action. The pinned
private `tcp_process_refused_data` helper reoffers refused chains in foreground
before close, so 221 is preserved without an unread-data RST.

Both reply and download writes use COPY. Cursors advance only on successful
`tcp_write`; output failures retain output work, never replay accepted bytes.
Each service makes at most one write/output/close attempt per channel. Download
close is enabled only after all payload ACKs (including the empty-file case).
Normal download peer transmit FIN is a half-close, not an ACK or failure.
Upload FIN does not commit: only accepted FIN enqueue confirms completion to
the core, not a remote FIN ACK. In pinned lwIP 2.2.1, both `tcp_close` and TX-only
`tcp_shutdown` mask FIN allocation ERR_MEM as ERR_OK plus TF_CLOSEPEND. This is
**not** sufficient for success. TX-only shutdown retains application ownership;
the adapter checks and clears the pinned private TF_CLOSEPEND flag on failure,
preventing autonomous timer retries while keeping callbacks/error observation.
Core's original 5-s close deadline bounds foreground retries. After successful
FIN enqueue, the PCB is in FIN_WAIT_1/LAST_ACK; detach callbacks and release it
with `tcp_close` (no allocation or unread-data RST in those states). No app
pointer is used after release. A compile-time version check requires re-review
before upgrading lwIP.

Each channel separately bounds failed `tcp_output` at 5 s from its first pending
write, without resetting the deadline on further writes or reply-ring draining.
Successful output clears it. Control expiry aborts the session; data expiry
aborts the transfer with 426. This applies consistently to uploads/downloads.
The core's whole-span input API reports accepted prefixes and stops at deferred
EPSV/PASV until the listener result is queued, preserving reply FIFO.

Abort invalidates ownership before calling TCP. `tcp_err` operates only on
static state, never on the already-freed PCB. Successful close detaches all
callbacks. Link-down teardown uses `tcp_abandon(reset=0)` to avoid network
submission. It also clears lwIP-owned active, TIME_WAIT and bound PCB lists so
previous successful closes cannot leave queued FIN/data into the next epoch.
**This sweep assumes this dedicated firmware has no other TCP users**, as in
its current build; other TCP services cannot coexist on this stack. This singleton extraction
does not introduce a coexistence or multi-server abstraction. Listening PCBs use unconditional lwIP LISTEN close,
never abort. Link loss, control loss, timeout and ABOR discard staging and retain
the committed file; teardown is idempotent.

## Historical baseline validation (before extraction)

The baseline commands/results and image sizes below describe the pre-extraction
revision, not the latest relocated library. Relocation and final verification are now complete.
Current builds use `src/ftp_core.c`, `src/ftp_tcp.c`, `src/ram_file.c`, public
`include/lm3s6965/` headers, and explicit caller-owned storage initialization.
See the final extraction section below and the [library guide](../../docs/ftp-library.md).

Previously successful commands (historical paths, not current instructions):
Current equivalents are `./scripts/ethernet/test.sh tcp lwip core platform`.
Use `./scripts/ethernet/test.sh ftp file platform` to include RAM and consumer checks.

```sh
scripts/ftp-tcp-test.sh
scripts/ftp-lwip-test.sh
scripts/ftp-core-test.sh
scripts/ftp-platform-test.sh
cmake -S . -B build/ftp-platform -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-lm3s6965.cmake -DCMAKE_BUILD_TYPE=Release -DLM3S6965_BUILD_ETHERNET_FTP=ON
cmake --build build/ftp-platform --target lm3s6965_ethernet_ftp -j4
cmake --build build/ftp-off -j4
arm-none-eabi-size -A build/ftp-platform/examples/ethernet-ftp/lm3s6965_ethernet_ftp
arm-none-eabi-nm -u build/ftp-platform/examples/ethernet-ftp/lm3s6965_ethernet_ftp
git diff --check
```

Native transport assertions use real raw API headers with a deterministic fake
implementation, ASan and UBSan; no sockets or hardware. They cover new/bind/listen
allocation failures, extra/wrong-peer connections, early data/FIN, write/output
failures without replay, upload close retries without premature commit, download
partial ACKs and close retries, RST while upload close is pending, chained input
with an accepted prefix and backpressure/retry, 4096/4097-byte limits, empty files,
ABOR, data-close timeout, control-output timeout, passive timeout across wrap,
control FIN, QUIT with retained/refused/new trailing input under write/close
pressure, repeated link-down and cleanup of stack-owned closing PCBs. Fake
shutdown matches real ERR_OK + TF_CLOSEPEND rather than fictional close ERR_MEM.
Tests also cover control/link loss before FIN acceptance and independent 5-s
output deadlines after the reply ring or unsent file suffix has drained.

Real lwIP regressions exhaust the actual 24-entry TCP_SEG pool to prevent FIN:
no premature FIN/226/commit, RST rollback, exact 5-s timeout, and recovery after
pool release. QUIT under refused-data, write allocation and output pressure
produces exactly 221 and FIN without RST; indefinite output failure is bounded.
See [real lwIP validation](../../scripts/ethernet/ftp/ftp-lwip-validation.md). All four native suites and the opt-in ARM
Release target pass; no shared BSP or dependency changes are part of these fixes.

## Historical pre-extraction ARM Release image

The local build produces `build/ftp-platform/examples/ethernet-ftp/lm3s6965_ethernet_ftp.map`.
Final independent verification inspected a fresh snapshot build, not that existing cache.
FTP ON and all four OFF executables passed. The OFF snapshot physically omitted lwIP.
All four offline suites passed with ASan/UBSan after the FIN/QUIT repairs.

Verified ELF SHA256: `239acd1ac4448fe7618534170205e4ecd0b2639fd773fe970936a0aa6b60d92b`.
Verified map SHA256: `dcbbaba063404e310cd36c6a0686ed2b0ca378a1fb7366bf250b2482a3b78e16`.
These identify that run. Temporary source paths can affect artifact hashes.

| Allocation | Bytes |
| --- | ---: |
| `.text` | 26748 |
| `.ARM.exidx` | 8 |
| `.data` | 8 |
| `.bss` (actual static state) | 39936 |
| `.c_heap` reserve | 2048 |
| `.stack` reserve | 8192 |

Flash load is **26764 bytes**; initialized/static SRAM is **39944 bytes**.
GNU size reports text=26756, data=8, bss=50176 (the latter includes both NOLOAD
reserves). Static RAM plus reserves totals **50184 / 65536 bytes**; the unassigned
heap-to-stack gap is 15352 bytes. Heap spans `0x20009c08..0x2000a408`; stack spans
`0x2000e000..0x20010000`. Linker fit assertions pass and there are no undefined
symbols. Vector word at `0x3c` is `0x00001535`, the strong SysTick handler Thumb
address (`SysTick_Handler=0x1534`). Relative to the previous image, flash grew
292 bytes and static SRAM grew 8 bytes (one output deadline per channel);
all file, reply, TCP pool, heap and stack capacities are unchanged. No RTOS/console/libc allocator implementation
is retained (only weak UART vector aliases and lwIP's own mem/memp allocators).

These are link-fit and offline ownership results, not physical acceptance.
Stack high-water, physical clock frequency/ISR timing, service gaps, peak live lwIP memory, physical link
recovery and authorized FTP client round trips remain unmeasured. No hardware,
flashing, downloads, remote access or host-network changes were performed.

## Latest tooling cleanup verification

Independent cleanup verification passed ten native ASan/UBSan executables, three CLI tests, and 41 link Python tests.
The external consumer compiled and linked only. It was not executed.
Six wrappers were removed only because they duplicated build/run plumbing.
Core, fake TCP, real lwIP, RAM, platform/heap, and consumer coverage remains.
See the [current tooling guide](../../scripts/ethernet/README.md) for suite paths and authorization boundaries.
Default and `all` are offline. Raw default and link live capture are excluded and require fresh authorization.
Cleanup added no firmware build or hardware evidence. The image accounting below remains earlier extraction evidence.

## Final reusable extraction verification

Fresh OFF builds omitted both lwIP and the entire FTP example.
All four existing firmware targets and the opt-in core/RAM archives built successfully.
Fresh ON builds produced all five firmware targets.
Five ASan/UBSan suites passed: RAM, core, TCP, platform, and real lwIP.
There were six executable runs because platform includes the separate heap test.
The caller-owned external consumer compiled and linked with a different lwIP configuration.
It copied only `src/` and `include/`, used 113-byte buffers, and inherited config/includes transitively.
It was not executed; this is compile/link reuse evidence, not alternate-config runtime or physical acceptance.

Latest FTP ELF SHA256: `8680fe7e01a4aba564c717360d58e7fbfa2738fa5cbcebe835f94fdbf4995384`.
Latest map SHA256: `88c36158ac2817c3f325f2923b29ac157c92d35ad2fd7693852f4b96f6ee48fb`.
These identify the fresh dirty-tree snapshot, not HEAD alone or the older image above.

| Allocation | Bytes |
|---|---:|
| `.text` | 27596 |
| `.ARM.exidx` | 8 |
| GNU size text | 27604 |
| `.data` | 8 |
| Flash load total | 27612 |
| Actual static `.bss` | 39976 |
| Static SRAM | 39984 |
| C heap reserve | 2048 |
| Stack reserve | 8192 |
| Static plus reserves | 50224 |
| Unassigned gap | 15312 |

GNU size BSS is 50216, including the NOLOAD reservations, not actual static BSS.
`39984 + 2048 + 8192 + 15312 = 65536`.
Heap bounds are `0x20009c30..0x2000a430`; stack bounds remain `0x2000e000..0x20010000`.
The vector at `0x3c` is `0x000001f1`, resolving to strong SysTick_Handler at `0x1f0`.
Example `main.c` owns staging at `0x20000008`, committed buffer at `0x20001008`,
and the 40-byte RAM object at `0x20002008`. Each buffer is 4096 bytes.
Core state is 1464 bytes plus a separate 4-byte borrowed storage pointer.
Pools and buffers are already in BSS; do not add them again.
No runtime timing, memory high-water, cable-cycle, physical IP, or FTP acceptance was added.
The dedicated-stack/global-sweep and pinned private-contract restrictions remain unchanged.
