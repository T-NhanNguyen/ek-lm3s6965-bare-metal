# Offline FTP / real lwIP validation

From the repository root, run `./scripts/ethernet/test.sh lwip`. It compiles the populated lwIP 2.2.1 core,
IPv4 and Ethernet sources together with production `ftp_core.c` and
`ftp_tcp.c`, using ASan/UBSan. No sockets, BPF, device, wall clock or external
network are used. The existing native options shim changes only alignment to
8 bytes; the build disables lwIP byteorder function aliases for Darwin.
Firmware TCP/heap/pool capacities remain unchanged.

## Asserted scenarios

- Valid Ethernet ARP request/reply and IPv4/TCP checksummed SYN/SYN-ACK/ACK;
  outgoing IPv4 and TCP checksums independently verified by the test peer.
- Anonymous login split across TCP packets, including a split CRLF; PASS and
  TYPE I coalesced with the preceding command's terminating LF.
- EPSV and PASV reply parsing, including exact PASV address, and actual passive
  TCP handshakes.
- STOR/RETR `hello`, exact byte comparison and `226 Transfer complete` for
  0, 17 and 4096 bytes (nontext patterns include NUL). Both passive modes used
  for uploads and downloads; upload EOF and server download FIN observed.
- Withheld download ACKs cause real timer-driven retransmission; duplicate
  sequence ranges do not duplicate reconstructed bytes. No FIN/226 while
  ACKs lag, including after all bytes have arrived but the final ACK is absent.
- Data RST after partial upload and a 4097-byte overflow produce 426/552;
  subsequent RETR still returns the previous complete 4096-byte file.
- 100 coalesced NOOP commands with linkoutput returning ERR_MEM exercise
  output retry, reply-ring pressure and retained receive cursor; all 100 exact
  replies arrive once and in order after output resumes.
- Exhaust the real fixed TCP_SEG pool after upload EOF. FIN cannot enqueue:
  no server FIN, no 226, no replacement of prior bytes, and callbacks/PCB remain
  owned with CLOSEPEND cleared. Inject RST before resource recovery, test the
  exact 4999/5000-ms close boundary with real timers, then test pool recovery
  producing FIN/226/commit. No test-only allocator hook or stack patch is used.
- QUIT with a held input chain and lwIP `refused_data`, then newly arriving
  trailing input: all are credited/discarded. Actual TCP segment exhaustion
  and linkoutput pressure delay closure; on recovery exactly 221 and FIN arrive
  without RST. Indefinite output failure aborts at 5 s even after the core reply
  ring drains. Fake-API tests additionally cover download output expiry after
  every file byte is enqueued, and control/link loss before close acceptance.
- Core whole-span EPSV/NOOP success and failure return the EPSV accepted prefix,
  block the suffix while opening, then queue 229/425 before 200.
- Control RST followed by a fresh handshake/login/download.
- Link-down during a partial upload, offline adapter teardown, fake-time
  advancement, link-up, fresh session and download of the retained file.
- Teardown leaves no active, TIME_WAIT, bound or listening TCP PCBs, zero lwIP
  heap use, and zero PBUF_POOL/PBUF/TCP_PCB/TCP_PCB_LISTEN/TCP_SEG pool use.

## Results and boundaries

Earlier native checks and the Arm Release opt-in crossbuild passed before tooling cleanup.
Current native equivalents are `./scripts/ethernet/test.sh lwip core tcp platform`.
Latest independent cleanup checks passed ten native ASan/UBSan executables, three CLI tests, and 41 link Python tests.
The consumer is compile/link only. Cleanup adds no firmware build or physical evidence.
See the [tooling guide](../README.md) for all suites and safe defaults.
Production fixes retain close ownership through actual FIN allocation,
continuously drain QUIT input, report deferred-action input prefixes, and bound
pending output independently of the core reply ring. The shutdown fake models
real ERR_OK + TF_CLOSEPEND, not tcp_close returning ERR_MEM.
The first run requested ASan leak detection, unsupported by this Darwin ASan
runtime; the runner now disables that feature. ASan bounds/use-after-free and
UBSan remain enabled, and explicit lwIP heap/pool assertions check ownership.

The peer is deliberately small, not a general TCP implementation: fixed
receive window, in-order injected traffic, no IP fragmentation, TCP options,
randomized loss, congestion benchmark or hardware timing. Link-down invokes
netif state changes and adapter/ARP cleanup directly, not the hardware driver.
Actual TCP_SEG exhaustion is covered for FIN and QUIT write allocation; this is
not exhaustive allocator-failure coverage for every stack path. Pool accounting
checks quiescent teardown rather than every allocation. FIN enqueue/accepted
close is the success boundary, not remote FIN acknowledgment. The adapter's
private CLOSEPEND/refused-data contracts are guarded to lwIP 2.2.1; upgrades
require re-review. Firmware resource capacities are unchanged; the fixes add
8 static SRAM bytes and 292 flash bytes (see [historical transport accounting](../../../examples/ethernet-ftp/TRANSPORT_VALIDATION.md)).
