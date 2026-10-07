#include "lm3s6965/ftp_core.h"
#include <limits.h>
#include <string.h>

typedef struct {
    char text[FTP_REPLY_CAPACITY];
    size_t length, offset;
    bool start;
} reply_t;
typedef struct {
    char text[FTP_CONTROL_CAPACITY];
    size_t length;
    bool bad, cr, active;
    uint32_t since;
} parser_t;
static struct {
    ftp_storage_view_t download;
    size_t enqueued, acked;
    bool session, quitting, user, login, binary;
    bool opening, passive, epsv, connected, ready, eof, closing;
    ftp_direction_t direction;
    parser_t parser;
    reply_t replies[FTP_REPLY_SLOTS];
    unsigned head, count, actions;
    uint32_t control_time, output_time, data_output_time;
    uint32_t passive_time, start_time, progress_time, close_time;
} core;
static const ftp_storage_t *storage;

static bool elapsed(uint32_t now, uint32_t then, uint32_t limit)
{
    return (uint32_t)(now - then) >= limit;
}
static void teardown(void)
{
    if (core.opening || core.passive) { core.actions |= FTP_CLOSE_PASSIVE; }
    if (core.connected || core.direction != FTP_IDLE || core.closing) {
        core.actions |= FTP_ABORT_DATA;
    }
    core.actions &= ~(unsigned)(FTP_OPEN_PASSIVE | FTP_CLOSE_DATA);
    for (unsigned i = 0u; i < FTP_REPLY_SLOTS; ++i) { core.replies[i].start = false; }
    core.opening = core.passive = core.connected = core.ready = false;
    core.eof = core.closing = false;
    core.direction = FTP_IDLE;
    core.enqueued = core.acked = 0u;
    memset(&core.download, 0, sizeof core.download);
    if (storage != NULL) { storage->ops->abort(storage->context); }
}
static void lost(void)
{
    if (storage == NULL) { return; }
    teardown();
    core.session = core.quitting = core.user = core.login = core.binary = false;
    memset(&core.parser, 0, sizeof core.parser);
    core.head = core.count = 0u;
    core.actions &= ~(unsigned)FTP_CLOSE_CONTROL;
    core.actions |= FTP_ABORT_CONTROL;
}
static bool reply(const char *text, bool start, uint32_t now)
{
    if (core.count == FTP_REPLY_SLOTS) { lost(); return false; }
    reply_t *r = &core.replies[(core.head + core.count) % FTP_REPLY_SLOTS];
    r->length = strlen(text);
    memcpy(r->text, text, r->length);
    r->offset = 0u;
    r->start = start;
    if (core.count == 0u) { core.output_time = now; }
    ++core.count;
    return true;
}
static void failure(const char *text, uint32_t now)
{
    teardown();
    (void)reply(text, false, now);
}
/* Small decimal formatter: no printf, truncation or variable format strings. */
static size_t decimal(char *out, unsigned value)
{
    char reverse[10];
    size_t n = 0u;
    do { reverse[n++] = (char)('0' + value % 10u); value /= 10u; } while (value != 0u);
    for (size_t i = 0u; i < n; ++i) { out[i] = reverse[n - i - 1u]; }
    return n;
}
static bool name(const char *arg)
{
    return storage->ops->matches_name(storage->context, arg, strlen(arg));
}
static void command(char *line, uint32_t now)
{
    char *arg = strchr(line, ' ');
    if (arg != NULL) { *arg++ = '\0'; } else { arg = line + strlen(line); }
    for (char *p = line; *p != '\0'; ++p) {
        if (*p >= 'a' && *p <= 'z') { *p = (char)(*p - 'a' + 'A'); }
    }
    const char *answer = "502 Command not supported.\r\n";
    if (strcmp(line, "QUIT") == 0 && *arg == '\0') {
        teardown(); core.quitting = true; answer = "221 Goodbye.\r\n";
    } else if (strcmp(line, "ABOR") == 0 && *arg == '\0') {
        bool active = core.direction != FTP_IDLE;
        teardown();
        answer = active ? "426 Transfer aborted.\r\n225 Abort complete.\r\n" : "225 No transfer.\r\n";
    } else if (strcmp(line, "NOOP") == 0 && *arg == '\0') {
        answer = "200 OK.\r\n";
    } else if (strcmp(line, "SYST") == 0 && *arg == '\0') {
        answer = "215 UNIX Type: L8\r\n";
    } else if (strcmp(line, "FEAT") == 0 && *arg == '\0') {
        answer = "211-Features\r\n EPSV\r\n PASV\r\n SIZE\r\n211 End\r\n";
    } else if (core.direction != FTP_IDLE || core.opening) {
        answer = "503 Operation in progress.\r\n";
    } else if (strcmp(line, "USER") == 0) {
        teardown(); core.user = strcmp(arg, "anonymous") == 0;
        core.login = core.binary = false;
        answer = core.user ? "331 Send password.\r\n" : "530 Anonymous only.\r\n";
    } else if (strcmp(line, "PASS") == 0) {
        if (core.user) { core.login = true; answer = "230 Logged in.\r\n"; }
        else { answer = "503 Send USER anonymous first.\r\n"; }
    } else if (!core.login) {
        answer = "530 Log in first.\r\n";
    } else if ((strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) && *arg == '\0') {
        answer = "257 \"/\"\r\n";
    } else if (strcmp(line, "CWD") == 0) {
        answer = strcmp(arg, "/") == 0 ? "250 Root directory.\r\n" : "550 Root only.\r\n";
    } else if (strcmp(line, "CDUP") == 0 && *arg == '\0') {
        answer = "250 Root directory.\r\n";
    } else if (strcmp(line, "TYPE") == 0) {
        if (strcmp(arg, "I") == 0) { core.binary = true; answer = "200 Binary mode.\r\n"; }
        else { core.binary = false; answer = "504 Binary mode only.\r\n"; }
    } else if ((strcmp(line, "EPSV") == 0 || strcmp(line, "PASV") == 0) && *arg == '\0') {
        teardown(); core.opening = true; core.epsv = strcmp(line, "EPSV") == 0;
        core.passive_time = now; core.actions |= FTP_OPEN_PASSIVE;
        return; /* Port reply is deferred until listener creation succeeds. */
    } else if (strcmp(line, "SIZE") == 0) {
        ftp_storage_view_t view;
        storage->ops->inspect(storage->context, &view);
        if (!name(arg) || !view.exists) { answer = "550 File unavailable.\r\n"; }
        else {
            char text[32] = "213 ";
            size_t n = 4u + decimal(text + 4u, (unsigned)view.length);
            memcpy(text + n, "\r\n", 3u); (void)reply(text, false, now); return;
        }
    } else if (strcmp(line, "STOR") == 0 || strcmp(line, "RETR") == 0) {
        bool upload = strcmp(line, "STOR") == 0;
        ftp_storage_view_t view;
        storage->ops->inspect(storage->context, &view);
        if (!name(arg) || (!upload && !view.exists)) { answer = "550 File unavailable.\r\n"; }
        else if (!core.binary) { answer = "504 Select TYPE I.\r\n"; }
        else if (!core.passive) { answer = "425 Use EPSV or PASV first.\r\n"; }
        else {
            if (upload && storage->ops->begin(storage->context) != FTP_STORAGE_OK) {
                failure("451 Storage operation failed.\r\n", now); return;
            }
            core.direction = upload ? FTP_UPLOAD : FTP_DOWNLOAD;
            if (!upload) { core.download = view; }
            core.enqueued = core.acked = 0u;
            core.start_time = core.progress_time = now;
            (void)reply("150 Opening binary data connection.\r\n", true, now); return;
        }
    }
    (void)reply(answer, false, now);
}

bool ftp_core_init(const ftp_storage_t *binding)
{
    if (binding == NULL || binding->context == NULL || binding->ops == NULL ||
        binding->capacity == 0u) { return false; }
#if SIZE_MAX > UINT_MAX
    if (binding->capacity > UINT_MAX) { return false; }
#endif
#if SIZE_MAX > UINT32_MAX
    if (binding->capacity > UINT32_MAX) { return false; }
#endif
    const ftp_storage_ops_t *ops = binding->ops;
    if (ops->matches_name == NULL || ops->clear == NULL || ops->inspect == NULL ||
        ops->begin == NULL || ops->append == NULL || ops->abort == NULL ||
        ops->commit == NULL || core.session || core.quitting || core.count != 0u ||
        core.actions != 0u || core.opening || core.passive || core.connected ||
        core.closing || core.direction != FTP_IDLE) { return false; }
    storage = binding;
    memset(&core, 0, sizeof core);
    return true;
}
bool ftp_core_is_initialized(void) { return storage != NULL; }
void ftp_core_reset(void)
{
    if (storage != NULL) { storage->ops->clear(storage->context); }
    memset(&core, 0, sizeof core);
}
bool ftp_core_session_open(uint32_t now)
{
    if (storage == NULL || core.session || core.quitting || core.actions != 0u) { return false; }
    core.session = true; core.control_time = now;
    return reply("220 RAM FTP ready.\r\n", false, now);
}
void ftp_core_session_lost(void) { lost(); }
void ftp_core_control_closed(void)
{
    /* Glue has already closed the control PCB, so do not request an abort. */
    lost(); core.actions &= ~(unsigned)FTP_ABORT_CONTROL;
}
unsigned ftp_core_take_actions(void)
{
    unsigned actions = core.actions; core.actions = 0u; return actions;
}
const uint8_t *ftp_core_reply_peek(size_t *length)
{
    if (length == NULL) { return NULL; }
    *length = 0u;
    if (core.count == 0u) { return NULL; }
    const reply_t *r = &core.replies[core.head];
    *length = r->length - r->offset;
    return (const uint8_t *)r->text + r->offset;
}
bool ftp_core_reply_consume(size_t length, uint32_t now)
{
    if (core.count == 0u) { return false; }
    reply_t *r = &core.replies[core.head];
    if (length == 0u || length > r->length - r->offset) { return false; }
    r->offset += length; core.output_time = now;
    if (r->offset == r->length) {
        if (r->start && core.direction != FTP_IDLE) {
            core.ready = true; core.data_output_time = now;
        }
        core.head = (core.head + 1u) % FTP_REPLY_SLOTS; --core.count;
    }
    if (core.quitting && core.count == 0u) {
        core.actions |= FTP_CLOSE_CONTROL; core.close_time = now;
    }
    return true;
}
/* Returns one complete line, including a malformed line, at CRLF. */
static bool parse_byte(parser_t *p, uint8_t byte, uint32_t now)
{
    if (!p->active) { p->active = true; p->since = now; }
    if (p->cr && byte == '\n') {
        if (p->length > FTP_CONTROL_CAPACITY - 2u) { p->bad = true; }
        p->text[p->length] = '\0';
        return true;
    }
    if (p->cr) { p->bad = true; }
    p->cr = byte == '\r';
    if (byte == '\r') { return false; }
    if (byte < 32u || byte > 126u) { p->bad = true; }
    if (p->length < FTP_CONTROL_CAPACITY - 1u) { p->text[p->length++] = (char)byte; }
    else { p->bad = true; }
    return false;
}
bool ftp_core_quitting(void) { return core.quitting; }
ftp_input_t ftp_core_control_span(const uint8_t *bytes, size_t length,
                                 uint32_t now, size_t *accepted)
{
    if (accepted == NULL) { return FTP_INPUT_REJECTED; }
    *accepted = 0u;
    if (!core.session || core.quitting || length > FTP_CONTROL_CAPACITY ||
        (length != 0u && bytes == NULL)) { return FTP_INPUT_REJECTED; }
    if (core.opening) { return FTP_INPUT_BACKPRESSURE; }
    parser_t probe = core.parser;
    unsigned lines = 0u;
    for (size_t i = 0u; i < length; ++i) {
        if (parse_byte(&probe, bytes[i], now)) { ++lines; memset(&probe, 0, sizeof probe); }
    }
    /* Reserve one slot for an asynchronous passive/transfer result. */
    if (lines != 0u && (core.count >= FTP_REPLY_SLOTS - 1u ||
        lines > FTP_REPLY_SLOTS - 1u - core.count)) { return FTP_INPUT_BACKPRESSURE; }
    for (size_t i = 0u; i < length; ++i) {
        if (parse_byte(&core.parser, bytes[i], now)) {
            if (core.parser.bad) { (void)reply("500 Malformed command line.\r\n", false, now); }
            else { command(core.parser.text, now); }
            memset(&core.parser, 0, sizeof core.parser);
            /* QUIT's tail belongs to the closing session; never execute it. */
            if (core.quitting) { *accepted = length; break; }
            if (core.opening) { *accepted = i + 1u; break; }
        }
        *accepted = i + 1u;
    }
    if (*accepted != 0u) { core.control_time = now; }
    return *accepted == length ? FTP_INPUT_OK : FTP_INPUT_PARTIAL;
}
bool ftp_core_passive_result(bool success, const uint8_t address[4], uint16_t port, uint32_t now)
{
    if (!core.session || !core.opening) { return false; }
    core.actions &= ~(unsigned)FTP_OPEN_PASSIVE;
    if (!success || port == 0u || (!core.epsv && address == NULL)) {
        failure("425 Cannot open passive listener.\r\n", now); return true;
    }
    char text[FTP_REPLY_CAPACITY];
    const char *prefix = core.epsv ? "229 Entering Extended Passive Mode (|||" : "227 Entering Passive Mode (";
    size_t n = strlen(prefix); memcpy(text, prefix, n);
    if (!core.epsv) {
        for (unsigned i = 0u; i < 4u; ++i) {
            n += decimal(text + n, address[i]); text[n++] = ',';
        }
        n += decimal(text + n, port / 256u); text[n++] = ',';
        n += decimal(text + n, port % 256u);
    } else { n += decimal(text + n, port); text[n++] = '|'; }
    memcpy(text + n, ").\r\n", 5u);
    core.opening = false; core.passive = true;
    return reply(text, false, now);
}
bool ftp_core_data_connected(uint32_t now)
{
    if (!core.session || !core.passive || core.connected || core.closing) { return false; }
    core.connected = true; core.progress_time = core.data_output_time = now;
    core.actions |= FTP_CLOSE_PASSIVE;
    return true;
}
bool ftp_core_data_is_connected(void) { return core.connected; }
ftp_direction_t ftp_core_direction(void) { return core.direction; }
bool ftp_core_transfer_ready(void)
{
    return core.direction != FTP_IDLE && core.connected && core.ready && !core.closing;
}
static void close_data(uint32_t now)
{
    core.closing = true; core.close_time = now; core.actions |= FTP_CLOSE_DATA;
}
bool ftp_core_upload_chunk(const uint8_t *bytes, size_t length, uint32_t now)
{
    if (!ftp_core_transfer_ready() || core.direction != FTP_UPLOAD || core.eof ||
        (length != 0u && bytes == NULL)) { return false; }
    ftp_storage_result_t result = storage->ops->append(storage->context, bytes, length);
    if (result != FTP_STORAGE_OK) {
        failure(result == FTP_STORAGE_CAPACITY ? "552 File exceeds storage capacity.\r\n" :
                "451 Storage operation failed.\r\n", now);
        return false;
    }
    if (length != 0u) { core.progress_time = now; }
    return true;
}
bool ftp_core_upload_eof(uint32_t now)
{
    if (!ftp_core_transfer_ready() || core.direction != FTP_UPLOAD || core.eof) { return false; }
    core.eof = true; close_data(now); return true;
}
bool ftp_core_eof_pending(void) { return core.eof; }
const uint8_t *ftp_core_download_span(size_t *length)
{
    if (length == NULL) { return NULL; }
    *length = 0u;
    if (!ftp_core_transfer_ready() || core.direction != FTP_DOWNLOAD) { return NULL; }
    *length = core.download.length - core.enqueued;
    return core.download.data + core.enqueued;
}
bool ftp_core_download_enqueued(size_t length, uint32_t now)
{
    if (!ftp_core_transfer_ready() || core.direction != FTP_DOWNLOAD ||
        length > core.download.length - core.enqueued) { return false; }
    core.enqueued += length;
    if (length != 0u) { core.progress_time = core.data_output_time = now; }
    if (core.enqueued == core.download.length && core.acked == core.download.length) { close_data(now); }
    return true;
}
bool ftp_core_download_acked(size_t length, uint32_t now)
{
    if (!ftp_core_transfer_ready() || core.direction != FTP_DOWNLOAD ||
        length > core.enqueued - core.acked) { return false; }
    core.acked += length;
    if (length != 0u) { core.progress_time = now; }
    if (core.enqueued == core.download.length && core.acked == core.download.length) { close_data(now); }
    return true;
}
bool ftp_core_data_close_result(bool accepted, uint32_t now)
{
    if (!core.session || !core.closing) { return false; }
    if (!accepted) { return true; } /* Retry CLOSE_DATA in glue until 5 s deadline. */
    if (core.count == FTP_REPLY_SLOTS) { lost(); return false; }
    bool committed = true;
    if (core.direction == FTP_UPLOAD) {
        committed = core.eof && storage->ops->commit(storage->context) == FTP_STORAGE_OK;
    }
    /* Successful graceful close does not request an abort of that PCB. */
    core.connected = core.closing = false;
    teardown();
    core.actions &= ~(unsigned)(FTP_ABORT_DATA | FTP_CLOSE_DATA);
    bool queued = reply(committed ? "226 Transfer complete.\r\n" :
                        "451 Storage operation failed.\r\n", false, now);
    return committed && queued;
}
void ftp_core_data_error(uint32_t now)
{
    if (core.direction != FTP_IDLE) { failure("426 Data connection failed.\r\n", now); }
    else if (core.passive || core.opening) { failure("425 Data connection failed.\r\n", now); }
}
const uint8_t *ftp_core_file(size_t *length, bool *exists)
{
    ftp_storage_view_t view = { NULL, 0u, false };
    if (storage != NULL) { storage->ops->inspect(storage->context, &view); }
    if (length != NULL) { *length = view.length; }
    if (exists != NULL) { *exists = view.exists; }
    return view.data;
}
void ftp_core_tick(uint32_t now)
{
    if (!core.session) { return; }
    if (elapsed(now, core.control_time, 60000u) ||
        (core.parser.active && elapsed(now, core.parser.since, 10000u)) ||
        (core.count != 0u && elapsed(now, core.output_time, 5000u)) ||
        (core.quitting && core.count == 0u && elapsed(now, core.close_time, 5000u))) {
        lost(); return;
    }
    if (core.closing && elapsed(now, core.close_time, 5000u)) {
        failure("426 Data close timed out.\r\n", now);
    } else if (core.direction == FTP_DOWNLOAD && core.connected && core.ready &&
               !core.closing && core.enqueued < core.download.length &&
               elapsed(now, core.data_output_time, 5000u)) {
        failure("426 Data output timed out.\r\n", now);
    } else if (core.direction != FTP_IDLE &&
               (elapsed(now, core.start_time, 60000u) || elapsed(now, core.progress_time, 15000u))) {
        failure("426 Transfer timed out.\r\n", now);
    } else if ((core.opening || (core.passive && core.direction == FTP_IDLE) ||
                (core.passive && !core.connected)) && elapsed(now, core.passive_time, 15000u)) {
        failure("425 Passive connection timed out.\r\n", now);
    }
}
