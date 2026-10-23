#include "lm3s6965/ftp_diagnostic.h"

#if LM3S6965_FTP_DIAGNOSTIC
#include <stdatomic.h>
#include <stdint.h>

volatile ftp_diagnostic_record_t ftp_diagnostic_record;
static uint32_t (*clock_ms)(void *);
static void *clock_context;

void ftp_diagnostic_clock(uint32_t (*clock)(void *), void *context)
{
    clock_ms = clock;
    clock_context = context;
}
void ftp_diagnostic_arm(void)
{
    ftp_diagnostic_record = (ftp_diagnostic_record_t){0};
    ftp_diagnostic_record.version = 1u;
    atomic_signal_fence(memory_order_release);
    ftp_diagnostic_record.armed = 1u;
}
void ftp_diagnostic_end(void)
{
    atomic_signal_fence(memory_order_release);
    ftp_diagnostic_record.armed = 0u;
}
void ftp_diagnostic_auth_enter(uint32_t phase)
{
    if (!ftp_diagnostic_record.armed || !clock_ms ||
        ftp_diagnostic_record.auth_entry_valid) return;
    ftp_diagnostic_record.auth_entry_ms = clock_ms(clock_context);
    ftp_diagnostic_record.auth_entry_phase = phase;
    atomic_signal_fence(memory_order_release);
    ftp_diagnostic_record.auth_entry_valid = 1u;
    /* Keep the measured operation after the published entry. */
    atomic_signal_fence(memory_order_seq_cst);
}
void ftp_diagnostic_auth_exit(uint32_t phase, uint32_t result)
{
    atomic_signal_fence(memory_order_seq_cst);
    if (!ftp_diagnostic_record.armed || !clock_ms ||
        !ftp_diagnostic_record.auth_entry_valid || ftp_diagnostic_record.auth_exit_valid) return;
    uint32_t end = clock_ms(clock_context);
    ftp_diagnostic_record.auth_exit_ms = end;
    ftp_diagnostic_record.auth_duration_ms = end - ftp_diagnostic_record.auth_entry_ms;
    ftp_diagnostic_record.auth_exit_phase = phase;
    ftp_diagnostic_record.auth_result = result;
    atomic_signal_fence(memory_order_release);
    ftp_diagnostic_record.auth_exit_valid = 1u;
}
void ftp_diagnostic_cause(ftp_diagnostic_cause_t cause, uint32_t software_ms)
{
    if (!ftp_diagnostic_record.armed || ftp_diagnostic_record.cause_valid) return;
    ftp_diagnostic_record.first_cause = (uint32_t)cause;
    ftp_diagnostic_record.cause_ms = software_ms;
    atomic_signal_fence(memory_order_release);
    ftp_diagnostic_record.cause_valid = 1u;
    /* Keep caller teardown after the published branch evidence. */
    atomic_signal_fence(memory_order_seq_cst);
}
#endif
