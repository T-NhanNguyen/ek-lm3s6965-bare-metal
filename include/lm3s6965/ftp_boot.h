/* Concrete foreground boot-service binding; opt-in, NOT startup/MMIO glue. */
#ifndef LM3S6965_FTP_BOOT_H
#define LM3S6965_FTP_BOOT_H
#include <stdbool.h>
#include <stddef.h>

#include "lm3s6965/boot_diag_publication.h"
#include "lm3s6965/boot_receive.h"
#include "lm3s6965/boot_service.h"
#include "lm3s6965/boot_snapshot.h"
#include "lm3s6965/ftp_operation.h"
#define FTP_BOOT_REPORT_ID_MAX 32u
#define FTP_BOOT_REPORT_DESCRIPTION_MAX 128u
/* Header literals occupy 54 bytes plus NUL; two IDs and the description
 * fit here, followed by the complete legacy status body. */
#define FTP_BOOT_REPORT_PREFIX_CAPACITY (55u + 2u * FTP_BOOT_REPORT_ID_MAX + FTP_BOOT_REPORT_DESCRIPTION_MAX)
#define FTP_BOOT_REPORT_CAPACITY (FTP_BOOT_REPORT_PREFIX_CAPACITY + BOOT_STATUS_CAPACITY)
typedef enum { FTP_BOOT_NONE, FTP_BOOT_STATUS, FTP_BOOT_FIRMWARE, FTP_BOOT_CONFIRM, FTP_BOOT_REPORT } ftp_boot_kind_t;
typedef struct {
    const char *id;
    const char *description;
} ftp_boot_report_descriptor_t;
typedef struct {
    boot_receive_t *receiver; /* fresh BOOT_RECEIVE_INIT, never reinitialized */
    boot_receive_config_t receive;
    boot_diag_publication_t *publication;
    boot_snapshot_sources_t sources;
    /* Optional, immutable static-lifetime descriptor and strings. NULL keeps
     * legacy behavior (boot-report unavailable). Printable ASCII only, 1..MAX
     * bytes excluding NUL. ID must name the captured running slot:
     * slot-a-breathing or slot-b-blink. Invalid descriptors reject init. */
    const ftp_boot_report_descriptor_t *report;
} ftp_boot_config_t;
typedef struct {
    ftp_operation_t operation;
    ftp_boot_config_t config;
    ftp_boot_kind_t active;
    bool entered, quarantine, firmware_used, accepted, eof;
    /* Sticky acceptance survives end/linkdown/lost reply. No reset actuator. */
    bool reset_requested;
    boot_receive_acceptance_t acceptance;
    boot_confirm_service_t confirm;
    boot_confirm_parser_t parser;
    char status[FTP_BOOT_REPORT_CAPACITY];
    size_t status_length;
} ftp_boot_t;
/* Zero-initialize ONCE before publication. All descriptor/context objects live
 * until unbound and transport quiescent. Caller supplies truthful running
 * evidence, real flash/controller and coherent sources for the SAME authority.
 * All services/flash/images/publication must share this foreground owner: no
 * direct service, snapshot, receive or controller calls while active. No ISR,
 * callback reentry, debugger/DMA mutation or alternate binding may bypass it.
 * This API never touches/copies/feeds a watchdog; caller preserves the SAME
 * APPLICATION epoch/history and services genuine Ethernet/timer/FTP evidence.
 * No implicit receiver replacement/retry/reconciliation or accounting replay. */
bool ftp_boot_init(ftp_boot_t *, const ftp_boot_config_t *);
const ftp_operation_t *ftp_boot_operation(ftp_boot_t *);
#endif
