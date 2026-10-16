#include "lm3s6965/ftp_boot.h"
#include <string.h>

static ftp_boot_kind_t kind(const char *p, size_t n, bool upload)
{
    if (n && *p == '/') { ++p; --n; }
    if (!upload && n == 11u && memcmp(p, "boot-status", n) == 0) return FTP_BOOT_STATUS;
    if (upload && n == 8u && memcmp(p, "firmware", n) == 0) return FTP_BOOT_FIRMWARE;
    if (upload && n == 7u && memcmp(p, "confirm", n) == 0) return FTP_BOOT_CONFIRM;
    return FTP_BOOT_NONE;
}
static bool matches(void *ctx, const char *p, size_t n, bool upload)
{
    (void)ctx;
    return p != NULL && kind(p, n, upload) != FTP_BOOT_NONE;
}
static bool begin(void *ctx, const char *p, size_t n, bool upload, ftp_storage_view_t *view)
{
    ftp_boot_t *b = ctx;
    if (b->entered || b->active != FTP_BOOT_NONE || b->quarantine) return false;
    ftp_boot_kind_t k = kind(p, n, upload);
    if (k == FTP_BOOT_NONE || (k == FTP_BOOT_FIRMWARE && b->firmware_used)) return false;
    b->entered = true;
    /* Claim ownership BEFORE callbacks. Every service path shares this lock. */
    b->active = k;
    b->accepted = b->eof = false;
    bool ok = false;
    if (k == FTP_BOOT_STATUS) {
        boot_snapshot_t snapshot;
        ok = boot_snapshot_capture(b->config.publication, &b->config.sources, &snapshot) == BOOT_SNAPSHOT_OK &&
            boot_status_render(&snapshot, b->status, sizeof b->status, &b->status_length);
        if (ok) *view = (ftp_storage_view_t){ (const uint8_t *)b->status, b->status_length, true };
    } else if (k == FTP_BOOT_FIRMWARE) {
        b->firmware_used = true;
        boot_receive_result_t r = boot_receive_begin(b->config.receiver, &b->config.receive);
        ok = r == BOOT_RECEIVE_OK;
        if (r == BOOT_RECEIVE_RECONCILE) b->quarantine = true;
    } else {
        memset(&b->parser, 0, sizeof b->parser);
        ok = true;
    }
    if (b->config.receive.flash->blocked) b->quarantine = true;
    if (!ok) b->active = FTP_BOOT_NONE;
    b->entered = false;
    return ok;
}
static ftp_operation_result_t receive_result(ftp_boot_t *b, boot_receive_result_t r)
{
    if (r == BOOT_RECEIVE_RECONCILE) b->quarantine = true;
    if (r == BOOT_RECEIVE_OK) return FTP_OPERATION_OK;
    if (r == BOOT_RECEIVE_WAIT) return FTP_OPERATION_WAIT;
    return FTP_OPERATION_ERROR;
}
static ftp_operation_result_t feed(void *ctx, const uint8_t *p, size_t n, size_t *accepted)
{
    ftp_boot_t *b = ctx;
    *accepted = 0u;
    if (b->entered || b->eof || b->accepted) return FTP_OPERATION_ERROR;
    b->entered = true;
    ftp_operation_result_t r = FTP_OPERATION_ERROR;
    if (b->active == FTP_BOOT_FIRMWARE) {
        r = receive_result(b, boot_receive_feed(b->config.receiver, p, n, accepted));
    } else if (b->active == FTP_BOOT_CONFIRM && boot_confirm_feed(&b->parser, p, n)) {
        *accepted = n;
        r = FTP_OPERATION_OK;
    }
    b->entered = false;
    return r;
}
static bool eof(void *ctx)
{
    ftp_boot_t *b = ctx;
    if (b->entered || b->eof || b->active == FTP_BOOT_NONE) return false;
    b->entered = true;
    b->eof = true;
    bool ok = b->active == FTP_BOOT_CONFIRM;
    if (b->active == FTP_BOOT_FIRMWARE)
        ok = receive_result(b, boot_receive_eof(b->config.receiver)) == FTP_OPERATION_OK;
    b->entered = false;
    return ok;
}
static ftp_operation_result_t step(void *ctx)
{
    ftp_boot_t *b = ctx;
    if (b->entered || b->quarantine || b->active == FTP_BOOT_NONE) return FTP_OPERATION_ERROR;
    if (b->accepted) return FTP_OPERATION_ACCEPTED;
    b->entered = true;
    ftp_operation_result_t r = FTP_OPERATION_WAIT;
    if (b->active == FTP_BOOT_FIRMWARE) {
        boot_receive_result_t rr = boot_receive_step(b->config.receiver, &b->acceptance);
        if (rr == BOOT_RECEIVE_ACCEPTED) {
            b->accepted = true;
            b->reset_requested = b->acceptance.reset_requested != 0u;
            r = FTP_OPERATION_ACCEPTED;
        } else r = receive_result(b, rr);
    } else if (b->active == FTP_BOOT_CONFIRM && b->eof) {
        boot_confirm_request_t request;
        boot_store_snapshot_t committed;
        r = FTP_OPERATION_ERROR;
        if (boot_confirm_finish(&b->parser, &request)) {
            boot_confirm_result_t cr = boot_confirm_apply(&b->confirm,
                b->config.publication, &b->config.sources, &request, &committed);
            if (cr == BOOT_CONFIRM_RECONCILE || b->config.receive.flash->blocked)
                b->quarantine = true;
            if (cr == BOOT_CONFIRM_OK) { b->accepted = true; r = FTP_OPERATION_ACCEPTED; }
        }
    }
    b->entered = false;
    return r;
}
static void end(void *ctx)
{
    ftp_boot_t *b = ctx;
    if (b->entered || b->active == FTP_BOOT_NONE) return;
    b->entered = true;
    if (b->active == FTP_BOOT_FIRMWARE && !b->accepted)
        (void)boot_receive_abort(b->config.receiver);
    b->active = FTP_BOOT_NONE;
    b->entered = false;
    /* acceptance/reset_requested/quarantine/one-shot history are NOT cleared. */
}
static const ftp_operation_ops_t ops = { matches, begin, feed, eof, step, end };
bool ftp_boot_init(ftp_boot_t *b, const ftp_boot_config_t *c)
{
    if (!b || !c || b->operation.ops || !c->receiver || !c->receive.flash ||
        !c->receive.running || !c->publication || !c->sources.running ||
        !c->sources.metadata.read || !c->sources.metadata.erase_page ||
        !c->sources.metadata.program_word || c->receiver->phase != BOOT_RECEIVE_NEW)
        return false;
    b->config = *c;
    b->operation = (ftp_operation_t){ &ops, b };
    return true;
}
const ftp_operation_t *ftp_boot_operation(ftp_boot_t *b)
{
    return b && b->operation.ops == &ops ? &b->operation : NULL;
}
