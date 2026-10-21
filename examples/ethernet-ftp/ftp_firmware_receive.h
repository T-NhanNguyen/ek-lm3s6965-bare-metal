/* Opt-in Ethernet receive to stored-firmware mapping.
 *
 * Design authority: checked-application-handoff trace f73 (Ethernet receive to
 * stored-firmware mapping design). This module wires the designed ftp_boot
 * operation (STOR firmware/confirm, RETR boot-status) onto the one shared
 * ftp_core singleton that already serves RAM hello. It performs no transport,
 * flash, watchdog, reset, or LED work itself.
 *
 * Mapping (delegated, not reimplemented):
 * - Receive bytes flow through the existing FTP path: ftp_tcp_service drains
 *   a bounded input prefix and calls ftp_core_foreground once per turn. This
 *   module adds no second foreground call in the same turn.
 * - Firmware bytes map to boot_receive feed/eof/step via ftp_boot:
 *   begin->receive_begin, feed/eof passthrough, one foreground step per turn
 *   through INVALIDATE(store_replace)->ERASE->PAYLOAD->WRITE_MANIFEST->
 *   VERIFY(readback+verify_stored)->READY(store_replace READY)->ACCEPTED->FIN.
 * - Durable READY before FIN: ftp_core only closes (FIN/226) after ACCEPTED,
 *   and boot_receive only accepts after READY is durably stored. Unchanged.
 * - Readback verification: VERIFY compares exact flash readback to the wire
 *   manifest, then runs independent stored authentication/hash/vectors.
 * - Quarantine on uncertain: store UNCERTAIN or a blocked controller yields
 *   RECONCILE, which quarantines all three binding services with no retry,
 *   revive, or rollback. External reconciliation is a caller action.
 * - Fallback: pre-acceptance failures discard staging only; the committed
 *   file and old metadata page survive (erase targets the alternate page
 *   only, confirmed by readbacks). Selection fallback across
 *   candidate/current/previous lives in boot_select, untouched here.
 * - Watchdog: this API never exposes, copies, feeds, or transfers any
 *   watchdog. The caller keeps the SAME APPLICATION epoch, stream history,
 *   and genuine ETHERNET/TIMERS/FTP evidence between calls.
 *
 * Ownership/lifetime: the caller supplies a fully populated ftp_boot_config_t
 * whose receiver (fresh BOOT_RECEIVE_INIT, never reinitialized), flash
 * controller, running/publication sources, and all contexts outlive the
 * binding. One admitted receiver lifetime only; no silent reinitialization.
 *
 * Failure mode: false returns the FTP path to RAM-only hello with the
 * previous binding preserved (ftp_core_bind_operation changes nothing on
 * false). No quarantine, retry, or fail_closed on bind failure. */

#ifndef FTP_FIRMWARE_RECEIVE_H
#define FTP_FIRMWARE_RECEIVE_H

#include <stdbool.h>

#include "lm3s6965/ftp_boot.h"

/* Bind the firmware receive operation onto the shared ftp_core singleton.
 * Requires a quiescent core and no live transport (call once after
 * ftp_core_init/reset, before transport start), matching
 * ftp_core_bind_operation's contract. True once bound; later calls are
 * no-ops returning true. False (NULL/invalid config, init or bind refusal)
 * leaves RAM-only hello in place. Never touches any watchdog. */
bool ftp_firmware_receive_bind(const ftp_boot_config_t *config);

/* Production hook: returns the real flash/running/publication/source bundle,
 * or NULL when this example has none. Weak default returns NULL (RAM-only
 * fallback). A production consumer overrides this with a strong definition
 * naming its initialized controller, fresh receiver, coherent metadata and
 * running sources, and permitted slot; it must NOT invent fake evidence. */
const ftp_boot_config_t *ftp_firmware_receive_production_config(void);

#endif
