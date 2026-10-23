/* Opt-in foreground experiment only; no reset persistence or wire exposure. */
#ifndef LM3S6965_FTP_DIAGNOSTIC_H
#define LM3S6965_FTP_DIAGNOSTIC_H
#include <stdint.h>

#ifndef LM3S6965_FTP_DIAGNOSTIC
#define LM3S6965_FTP_DIAGNOSTIC 0
#endif

typedef enum {
    FTP_DIAG_NONE, FTP_DIAG_TCP_CONTROL_ERROR, FTP_DIAG_TCP_CONTROL_FIN,
    FTP_DIAG_TCP_CONTROL_OUTPUT, FTP_DIAG_TCP_DATA_ERROR, FTP_DIAG_TCP_DATA_OUTPUT,
    FTP_DIAG_CORE_CONTROL_IDLE, FTP_DIAG_CORE_PARSER, FTP_DIAG_CORE_REPLY_OUTPUT,
    FTP_DIAG_CORE_CONTROL_CLOSE, FTP_DIAG_CORE_DATA_CLOSE,
    FTP_DIAG_CORE_DATA_OUTPUT, FTP_DIAG_CORE_TRANSFER_TOTAL,
    FTP_DIAG_CORE_TRANSFER_PROGRESS, FTP_DIAG_CORE_PASSIVE,
    FTP_DIAG_TCP_LINK_DOWN, FTP_DIAG_CORE_ABOR, FTP_DIAG_CORE_QUIT,
    FTP_DIAG_CORE_OPERATION_FEED, FTP_DIAG_CORE_OPERATION_EOF,
    FTP_DIAG_CORE_OPERATION_STEP, FTP_DIAG_CORE_PREMATURE_ACCEPTANCE,
    FTP_DIAG_CORE_DATA_ERROR
} ftp_diagnostic_cause_t;

/* All fields are fixed-width debugger-readable RAM. valid fields publish LAST
 * with compiler release ordering; halt the serialized foreground owner before
 * inspecting. Not an atomic concurrent-reader protocol or a reset capsule.
 * Times are unsigned software tick milliseconds, NOT elapsed wallclock: a
 * synchronous step can delay the software clock producer. Subtract unsigned
 * exit-entry modulo 2^32; no new timer/register is used. phase/result values
 * are boot_receive enums, without coupling this module to boot internals. */
typedef struct {
    uint32_t version, armed;
    uint32_t auth_entry_valid, auth_exit_valid, cause_valid;
    uint32_t auth_entry_ms, auth_exit_ms, auth_duration_ms;
    uint32_t auth_entry_phase, auth_exit_phase, auth_result;
    uint32_t first_cause, cause_ms;
} ftp_diagnostic_record_t;

#if LM3S6965_FTP_DIAGNOSTIC
extern volatile ftp_diagnostic_record_t ftp_diagnostic_record;
/* Configure before admission; callback/context outlive the transaction, return
 * software tick ms, and must not mutate/reenter FTP. NULL leaves AUTH invalid. */
void ftp_diagnostic_clock(uint32_t (*clock_ms)(void *), void *context);
/* Only the admitted firmware binding arms. Status/confirm/session init never
 * reset the record. End disarms but retains all evidence until next admission. */
void ftp_diagnostic_arm(void);
void ftp_diagnostic_end(void);
void ftp_diagnostic_auth_enter(uint32_t phase);
void ftp_diagnostic_auth_exit(uint32_t phase, uint32_t result);
void ftp_diagnostic_cause(ftp_diagnostic_cause_t cause, uint32_t software_ms);
#else
static inline void ftp_diagnostic_clock(uint32_t (*clock_ms)(void *), void *context)
{ (void)clock_ms; (void)context; }
static inline void ftp_diagnostic_arm(void) { }
static inline void ftp_diagnostic_end(void) { }
static inline void ftp_diagnostic_auth_enter(uint32_t phase) { (void)phase; }
static inline void ftp_diagnostic_auth_exit(uint32_t phase, uint32_t result)
{ (void)phase; (void)result; }
static inline void ftp_diagnostic_cause(ftp_diagnostic_cause_t cause, uint32_t software_ms)
{ (void)cause; (void)software_ms; }
#endif
#endif
