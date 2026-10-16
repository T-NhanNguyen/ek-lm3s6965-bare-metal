# FTP boot binding (opt-in; offline validation pending)

## Boundary and build

`include/lm3s6965/ftp_boot.h` / `src/ftp_boot.c` bind **real** status capture/render,
confirmation parser/lifecycle/store and firmware verifier/flash/store/receiver to
production `ftp_core` and pinned raw lwIP 2.2.1. `ftp_storage_ops_t` is unchanged:
RAM `hello` remains synchronous, whole-chunk append and commit-after-FIN.
`ftp_operation.h` is a separate optional async interface, not flash pretending
to be RAM. Link opt-in `lm3s6965_ftp_boot` plus the existing TCP factory target
and verifier/crypto closure. Neither archive is an enabled updater/boot image.
Standalone HOST archive entry: `cmake -S tests/boot -B /tmp/ftp-binding-build-debug
-DCMAKE_BUILD_TYPE=Debug`, then `cmake --build /tmp/ftp-binding-build-debug`.

## Startup, ownership and foreground schedule

1. The actual application owner supplies an initialized real flash controller,
   a fresh `BOOT_RECEIVE_INIT` receiver, initialized/preserved diagnostic
   publication and coherent metadata/running sources. Running evidence must be
   the authenticated **actually executing** image, never a selected-role guess.
2. Zero-initialize `ftp_boot_t` once and call `ftp_boot_init`; initialize RAM
   core storage as before, then bind `ftp_core_bind_operation(ftp_boot_operation(&binding))`
   while the transport/core are quiescent. Immutable descriptors and all source
   objects outlive the binding; no stack-local borrowed descriptors.
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
reentry guard. Status/confirm cannot bypass a receiving transaction; the core
rejects concurrent commands and the binding rejects concurrent begins. External
users must also obey that owner: direct `boot_confirm_apply`, snapshot/receive,
flash or diagnostic calls, alternate bindings, ISR/DMA/debugger mutation cannot
bypass it. C structs are not a provenance/authentication mechanism.

`examples/ethernet-ftp/main.c` shows the truthful foreground schedule but still
binds **RAM hello only**. Production boot-preserved object placement/startup,
MMIO flash/watchdog ports, clock continuity, candidate deadline and reset
actuator are **not wired** by this change. No fake enabled updater, reset,
hardware startup, physical timing/durability, 32 KiB fit or full-MVP proof.

## Wire and cancellation decisions

All new names require TYPE I: `RETR boot-status`, `STOR firmware`, `STOR confirm`
(with optional single leading slash). Status is rendered into owned 1024-byte
binding storage at begin and remains stable until ACK/FIN/teardown. No SIZE
extension is invented. Confirmation remains restricted-bench unsigned.

Firmware feed reports the exact `boot_receive_feed` accepted prefix end-to-end.
Only those bytes receive TCP credit; the adapter retains the suffix, including
zero-accept WAIT, without failure, timer refresh or synthetic progress. It holds
one pbuf chain and lwIP may refuse the next. Retained and refused chains drain
before exactly-once EOF; FIN-bearing refused chains are reoffered explicitly.
There is no erase/program/crypto loop in a raw callback. One foreground step
may still be an **opaque synchronous signature/store/full stored-verification
gap**; bounded bytes/actions do not prove elapsed service or watchdog margins.

WAIT keeps existing **15 s accepted-byte idle / 60 s total transfer** limits;
foreground erase/crypto activity is not accepted-byte progress. Existing 5 s
output/close, 15 s passive and finite reply/buffer limits remain. This may reject
a slow physical device rather than silently extending a safety deadline.

Async success is durable **before FIN**: firmware must publish READY with stored
verification and committed readback; confirmation must durably apply/readback
or match an existing confirmed identity with no write. Only then is FIN
requested; **226 requires accepted graceful FIN plus durable success**. This
intentionally differs from RAM's commit-after-FIN and avoids pretending flash
can roll back after terminal acceptance. No actual reset occurs here.
`reset_requested` and owned firmware acceptance are sticky from durable acceptance,
independent of final reply delivery. The application reset actuator must eventually
honor that intent even if FIN/reply is lost; it must not wait forever for 226.
The binding does not choose a physical reset time or claim trial execution.

ABOR/QUIT/control EOF/RST, data error, linkdown, timeout or failed close end a
successful begin exactly once. Before firmware acceptance this invokes receiver
abort once, without cleanup IO; terminal receiver duplicates cannot replay IO.
After acceptance end never calls abort, clears intent or claims rollback. ABOR
while accepted says 'Operation already accepted'; other transport errors describe
transport failure only. A lost reply is an uncertain client outcome to resolve
by fresh status, never grounds for destructive blind retry.

One receiver lifetime permits **one admitted firmware attempt**, successful or
not. It is never silently reinitialized. A new independently authorized lifetime
is a caller action, not an automatic retry. RECONCILE or blocked-controller
failure quarantines **all three** binding services even after disconnect. There
is no clear-quarantine/revive/replay API. External explicit authority/controller
reconciliation and new lifetime policy are required; reconciliation cannot turn
an old failed transaction into accepted publication.

## Offline validation entry points

`scripts/ethernet/test.sh core tcp lwip platform` includes unchanged RAM core,
fake-TCP ownership and real-lwIP regressions; **lwip runs two binaries**, original
RAM peer then the boot-enabled peer using actual signed verifier/store/controller
fixtures. `FTP_TEST_BUILD_ONLY=1` compiles without executing compiled suites
(platform Python is omitted too; raw/link reject compile-only). Optional
`FTP_TEST_BUILD_DIR`, `FTP_BOOT_BUILD_DIR`, `FTP_TEST_CONFIG=Debug|Release` make
build provenance explicit. Existing native Ethernet flags remain `-g -O1` with
ASan/UBSan; config selects Debug/Release for real boot libraries.

### Receive-window oracle and refused FIN boundary

The initial frozen peer failed its first partial-WAIT window assertion in both
configurations. That oracle omitted FIN sequence space, not missing payload
credit in the adapter. In pinned lwIP `77dcd25a` (2.2.1):

- `TCP_TCPLEN` (`src/include/lwip/priv/tcp_priv.h:152`) includes FIN's one byte;
  `src/core/tcp_in.c:1546-1550` advances `rcv_nxt` and debits that entire length.
- `tcp_in.c:531-540` marks refused data `PBUF_FLAG_TCP_FIN` and defers the FIN
  correction; it does not restore the byte while that data remains refused.
- `src/core/tcp.c:1568-1581` restores the FIN byte and delivers the NULL callback
  only when reoffering succeeds. This is ownership acceptance, not consumption
  of the second chain. The adapter still delays backend EOF until its retained
  payload is consumed; it must **not** call `tcp_recved` for FIN itself.

For the fixture's 396-byte package (160-byte manifest + 236-byte payload),
`TCP_WND=2144`: after both packets the window is **1747** (396 + FIN owed),
then **1907** after accepting only the manifest, unchanged on zero-progress
WAIT. The old assertion expected 1908. These are statically derived expectations,
not observed runtime trace values. Consuming the first chain's four payload bytes
makes 1911; consuming its last byte and accepting the refused chain restores one
payload byte plus FIN, making **1913**, with backend EOF still false. The second
chain's 231 payload bytes are credited only as consumed, ending at **2144** before
READY/FIN/226. The peer now checks every turn through these boundaries, including
no early EOF/success/reset and full window recovery. This strengthens the same
partial-WAIT class; it adds no modeled lifetime or test registration. Corrected
behavioral execution remains pending with test-merit-gate.

Full HOST gates are still the unfiltered Debug/Release `tests/boot` CTest suites,
**not a substitute for the Ethernet script suites**. No worker behavioral tests
were executed; test-merit-gate owns execution against the final frozen target.
