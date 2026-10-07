/* Singleton, serialized FTP state machine with borrowed synchronous storage.
 * See examples/ethernet-ftp/FTP_CORE_API.md before use. No reentrancy/ISR use. */
#ifndef LM3S6965_FTP_CORE_H
#define LM3S6965_FTP_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/ftp_storage.h"
#define FTP_CONTROL_CAPACITY 256u
#define FTP_REPLY_SLOTS 8u
#define FTP_REPLY_CAPACITY 128u

typedef enum { FTP_IDLE, FTP_UPLOAD, FTP_DOWNLOAD } ftp_direction_t;
typedef enum { FTP_INPUT_OK, FTP_INPUT_PARTIAL, FTP_INPUT_BACKPRESSURE, FTP_INPUT_REJECTED } ftp_input_t;
enum {
    FTP_OPEN_PASSIVE = 1u << 0,
    FTP_CLOSE_PASSIVE = 1u << 1,
    FTP_ABORT_DATA = 1u << 2,
    FTP_CLOSE_DATA = 1u << 3,
    FTP_CLOSE_CONTROL = 1u << 4,
    FTP_ABORT_CONTROL = 1u << 5
};

/* Times are monotonic milliseconds modulo 2^32. No heap or retained input. */
/* Validate and borrow immutable descriptor/ops/context; no backend callbacks
 * or mutation. False preserves previous binding and protocol/backend state.
 * Requires quiescent core AND no live transport, backend staging, or retained
 * views (the latter are caller preconditions). All callbacks/context required;
 * capacity is 1..min(UINT_MAX, UINT32_MAX). Objects outlive the binding. */
bool ftp_core_init(const ftp_storage_t *storage);
/* Read-only binding query, including before init; does not inspect storage. */
bool ftp_core_is_initialized(void);
/* Explicit boot/reset only: clear backend and protocol, retain binding.
 * Release PCBs/retries and all views first. Unbound: protocol reset only. */
void ftp_core_reset(void);
bool ftp_core_session_open(uint32_t now);
void ftp_core_session_lost(void); /* Control EOF/RST or link loss. */
void ftp_core_control_closed(void); /* Completion of CLOSE_CONTROL. */
void ftp_core_tick(uint32_t now);
/* Split spans into <=256 bytes. PARTIAL stops after a passive command;
 * resume only after passive_result. accepted reports the consumed prefix. */
ftp_input_t ftp_core_control_span(const uint8_t *bytes, size_t length,
                                  uint32_t now, size_t *accepted);
bool ftp_core_quitting(void);
const uint8_t *ftp_core_reply_peek(size_t *length);
bool ftp_core_reply_consume(size_t length, uint32_t now);
unsigned ftp_core_take_actions(void);
/* Supply the local IPv4 address as four network-order octets, not host IP. */
bool ftp_core_passive_result(bool success, const uint8_t address[4],
                             uint16_t port, uint32_t now);
bool ftp_core_data_connected(uint32_t now);
bool ftp_core_data_is_connected(void);
ftp_direction_t ftp_core_direction(void);
bool ftp_core_transfer_ready(void); /* 150 consumed AND data connected. */
/* Oversize upload chunks fail the whole transfer before copying any bytes. */
bool ftp_core_upload_chunk(const uint8_t *bytes, size_t length, uint32_t now);
bool ftp_core_upload_eof(uint32_t now);
bool ftp_core_eof_pending(void);
/* Download pointer remains valid until teardown. Enqueued != acknowledged. */
const uint8_t *ftp_core_download_span(size_t *length);
bool ftp_core_download_enqueued(size_t length, uint32_t now);
bool ftp_core_download_acked(size_t length, uint32_t now);
/* ONLY after FIN enqueue succeeds and ownership is safely released.
 * tcp_close ERR_OK alone is insufficient in lwIP 2.2.1. Never on RST/error. */
bool ftp_core_data_close_result(bool accepted, uint32_t now);
void ftp_core_data_error(uint32_t now); /* RST, unexpected EOF, PCB failure. */
/* Borrowed committed view; NULL/0/false when unbound or absent.
 * Existing empty file has non-NULL data. Invalidated by successful commit,
 * reset/clear or permitted reinit; release all references before those calls. */
const uint8_t *ftp_core_file(size_t *length, bool *exists);
#endif
