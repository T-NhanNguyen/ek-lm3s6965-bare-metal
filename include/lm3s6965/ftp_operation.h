/* Optional foreground operations, separate from synchronous RAM storage. */
#ifndef LM3S6965_FTP_OPERATION_H
#define LM3S6965_FTP_OPERATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/ftp_storage.h"
typedef enum { FTP_OPERATION_OK, FTP_OPERATION_WAIT, FTP_OPERATION_ACCEPTED,
               FTP_OPERATION_ERROR } ftp_operation_result_t;
typedef struct {
    bool (*matches)(void *, const char *, size_t, bool upload);
    /* Begin owns the operation until end. A failed begin owns nothing.
     * Downloads publish a stable owned view through end. No pending begin. */
    bool (*begin)(void *, const char *, size_t, bool, ftp_storage_view_t *);
    /* Copy only the reported prefix, including on WAIT. Never retain input.
     * ERROR may report a copied prefix; caller credits it before teardown. */
    ftp_operation_result_t (*feed)(void *, const uint8_t *, size_t, size_t *);
    bool (*eof)(void *); /* exactly once, after all retained/refused data */
    /* One bounded foreground action; WAIT is not progress. Never in callbacks.
     * ACCEPTED means durable success, not a transport/FIN result. */
    ftp_operation_result_t (*step)(void *);
    /* Exactly once for every successful begin. May cancel preacceptance only;
     * never revoke durable acceptance/reset intent, even on lost reply. */
    void (*end)(void *);
} ftp_operation_ops_t;
typedef struct { const ftp_operation_ops_t *ops; void *context; } ftp_operation_t;
#endif
