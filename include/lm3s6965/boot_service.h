/* HOST application service boundary. No FTP transport or flash binding. */
#ifndef LM3S6965_BOOT_SERVICE_H
#define LM3S6965_BOOT_SERVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_diag_publication.h"
#include "lm3s6965/boot_lifecycle.h"
#include "lm3s6965/boot_snapshot.h"
#include "lm3s6965/boot_store.h"

#define BOOT_STATUS_CAPACITY 1024u
#define BOOT_CONFIRM_CAPACITY 128u

typedef struct {
    uint8_t bytes[BOOT_CONFIRM_CAPACITY];
    size_t used;
    bool failed, finished;
} boot_confirm_parser_t;
/* Zero-initialize once per body. Feed never mutates metadata. EOF is explicit.
 * A failed or finished parser cannot accept more bytes. Output is disjoint. */
bool boot_confirm_feed(boot_confirm_parser_t *, const uint8_t *, size_t);
bool boot_confirm_finish(boot_confirm_parser_t *, boot_confirm_request_t *);

/* Render only a successful owned boot_snapshot_capture output. Provenance is
 * a caller contract. Output and size stay unchanged on failure. All spans are
 * disjoint. No source callback runs during rendering. */
bool boot_status_render(const boot_snapshot_t *, char *, size_t, size_t *);

typedef struct { bool busy, reconcile; } boot_confirm_service_t;
typedef enum {
    BOOT_CONFIRM_OK, BOOT_CONFIRM_ARGUMENT, BOOT_CONFIRM_BUSY,
    BOOT_CONFIRM_SNAPSHOT, BOOT_CONFIRM_REJECTED, BOOT_CONFIRM_STORE,
    BOOT_CONFIRM_RECONCILE
} boot_confirm_result_t;
/* Caller owns sources, physical images, owner, service and metadata exclusively
 * across the full call. Callbacks cannot mutate these objects or bypass this
 * service. Running evidence must describe the actual authenticated execution.
 * New success requires store commit/readback. Duplicate success uses fresh
 * captured authority and makes no write. UNCERTAIN blocks this service; caller
 * must reconcile authority explicitly. Never replay reset accounting. Output
 * must be disjoint from all inputs and callback storage; publish on OK only. */
boot_confirm_result_t boot_confirm_apply(boot_confirm_service_t *,
    boot_diag_publication_t *, const boot_snapshot_sources_t *,
    const boot_confirm_request_t *, boot_store_snapshot_t *);
#endif
