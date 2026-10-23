#include "lm3s6965/ftp_diagnostic.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t clock_ms(void *context)
{
    uint32_t *ticks = context;
    return (*ticks)++;
}
int main(void)
{
    uint32_t ticks = UINT32_MAX - 2u;
    ftp_diagnostic_clock(clock_ms, &ticks);
    ftp_diagnostic_cause(FTP_DIAG_TCP_CONTROL_FIN, 1u);
    ftp_diagnostic_auth_enter(2u);
    assert(ticks == UINT32_MAX - 2u); /* Unarmed/OFF never calls clock. */
    ftp_diagnostic_arm();
    ftp_diagnostic_auth_enter(2u);
#if LM3S6965_FTP_DIAGNOSTIC
    assert(ftp_diagnostic_record.version == 1u);
    assert(ftp_diagnostic_record.auth_entry_valid && !ftp_diagnostic_record.auth_exit_valid);
    assert(ftp_diagnostic_record.auth_entry_ms == UINT32_MAX - 2u);
    assert(ftp_diagnostic_record.auth_entry_phase == 2u);
    ticks = 4u;
#endif
    ftp_diagnostic_auth_exit(3u, 0u);
    ftp_diagnostic_cause(FTP_DIAG_TCP_CONTROL_FIN, 20u);
    ftp_diagnostic_cause(FTP_DIAG_CORE_TRANSFER_PROGRESS, 21u);
    ftp_diagnostic_end();
    ftp_diagnostic_cause(FTP_DIAG_TCP_LINK_DOWN, 22u);
#if LM3S6965_FTP_DIAGNOSTIC
    assert(ftp_diagnostic_record.auth_exit_valid && !ftp_diagnostic_record.armed);
    assert(ftp_diagnostic_record.auth_exit_phase == 3u && ftp_diagnostic_record.auth_result == 0u);
    assert(ftp_diagnostic_record.auth_duration_ms == 7u);
    assert(ftp_diagnostic_record.first_cause == FTP_DIAG_TCP_CONTROL_FIN);
    assert(ftp_diagnostic_record.cause_ms == 20u);
    ftp_diagnostic_arm();
    assert(!ftp_diagnostic_record.cause_valid && !ftp_diagnostic_record.auth_entry_valid);
    ftp_diagnostic_auth_enter(2u);
    ftp_diagnostic_auth_exit(9u, 10u); /* Failure and terminal phase retained. */
    assert(ftp_diagnostic_record.auth_result == 10u && ftp_diagnostic_record.auth_exit_phase == 9u);
    ftp_diagnostic_clock(NULL, NULL);
    ftp_diagnostic_arm();
    ftp_diagnostic_auth_enter(2u);
    ftp_diagnostic_auth_exit(3u, 0u);
    assert(!ftp_diagnostic_record.auth_entry_valid && !ftp_diagnostic_record.auth_exit_valid);
#else
    assert(ticks == UINT32_MAX - 2u);
#endif
    return 0;
}
