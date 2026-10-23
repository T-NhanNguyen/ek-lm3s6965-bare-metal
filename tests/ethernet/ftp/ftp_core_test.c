/* Offline native assertions. This is not evidence of lwIP or board operation. */
#include "lm3s6965/ftp_core.h"
#include "lm3s6965/ram_file.h"
#include <limits.h>
#define FIXTURE_CAPACITY 4096u
static ram_file_t fixture;
static uint8_t fixture_a[FIXTURE_CAPACITY], fixture_b[FIXTURE_CAPACITY];
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t clock_ms;
static uint8_t binary[FIXTURE_CAPACITY + 1u];
static const uint8_t ip[4] = {192u, 168u, 7u, 2u};
static unsigned groups;

/* Most tests intentionally feed complete non-deferred spans. */
static ftp_input_t ftp_core_control_input(const uint8_t *b, size_t n, uint32_t t)
{
    size_t accepted;
    ftp_input_t result = ftp_core_control_span(b, n, t, &accepted);
    assert(result != FTP_INPUT_PARTIAL);
    assert(accepted == (result == FTP_INPUT_OK ? n : 0));
    return result;
}
static void input(const char *text)
{
    assert(ftp_core_control_input((const uint8_t *)text, strlen(text), clock_ms) == FTP_INPUT_OK);
}
static void expect(const char *text)
{
    size_t length = 99u;
    const uint8_t *p = ftp_core_reply_peek(&length);
    assert(p != NULL && length == strlen(text));
    assert(memcmp(p, text, length) == 0);
    assert(!ftp_core_reply_consume(length + 1u, clock_ms));
    assert(!ftp_core_reply_consume(0u, clock_ms));
    assert(ftp_core_reply_consume(length, clock_ms));
}
static void empty(void)
{
    size_t length = 99u;
    assert(ftp_core_reply_peek(&length) == NULL && length == 0u);
}
static void file_is(const uint8_t *bytes, size_t length, bool exists)
{
    size_t actual = 99u; bool present = !exists;
    const uint8_t *p = ftp_core_file(&actual, &present);
    assert(actual == length && present == exists);
    if (length != 0u) { assert(memcmp(p, bytes, length) == 0); }
}
static void boot(uint32_t now)
{
    ftp_core_reset(); clock_ms = now;
    file_is(NULL, 0u, false);
    assert(ftp_core_session_open(clock_ms));
    assert(!ftp_core_session_open(clock_ms));
    expect("220 RAM FTP ready.\r\n");
}
static void login(void)
{
    input("USER anonymous\r\nPASS a@b\r\nTYPE I\r\n");
    expect("331 Send password.\r\n"); expect("230 Logged in.\r\n"); expect("200 Binary mode.\r\n");
}
static void passive(bool epsv)
{
    input(epsv ? "EPSV\r\n" : "PASV\r\n"); empty();
    assert((ftp_core_take_actions() & FTP_OPEN_PASSIVE) != 0u);
    assert(ftp_core_passive_result(true, ip, 49153u, clock_ms));
    expect(epsv ? "229 Entering Extended Passive Mode (|||49153|).\r\n" :
                 "227 Entering Passive Mode (192,168,7,2,192,1).\r\n");
}
static void start(bool upload, bool connect_first)
{
    passive(upload);
    if (connect_first) { assert(ftp_core_data_connected(clock_ms)); }
    input(upload ? "STOR /hello\r\n" : "RETR hello\r\n");
    assert(!ftp_core_transfer_ready());
    assert(!ftp_core_upload_chunk(binary, 1u, clock_ms));
    assert(!ftp_core_upload_eof(clock_ms));
    expect("150 Opening binary data connection.\r\n");
    if (!connect_first) { assert(ftp_core_data_connected(clock_ms)); }
    assert(!ftp_core_data_connected(clock_ms));
    assert(ftp_core_data_is_connected() && ftp_core_transfer_ready());
    assert(ftp_core_direction() == (upload ? FTP_UPLOAD : FTP_DOWNLOAD));
    assert((ftp_core_take_actions() & FTP_CLOSE_PASSIVE) != 0u);
}
static void finish(void)
{
    assert((ftp_core_take_actions() & FTP_CLOSE_DATA) != 0u);
    empty();
    assert(ftp_core_data_close_result(false, clock_ms)); empty();
    assert(ftp_core_data_close_result(true, clock_ms));
    assert(!ftp_core_data_close_result(true, clock_ms));
    expect("226 Transfer complete.\r\n");
    assert(ftp_core_direction() == FTP_IDLE && !ftp_core_data_is_connected());
    assert((ftp_core_take_actions() & FTP_ABORT_DATA) == 0u);
}
static void store(const uint8_t *bytes, size_t length)
{
    start(true, true);
    assert(ftp_core_upload_chunk(bytes, length, clock_ms));
    assert(ftp_core_upload_eof(clock_ms)); assert(ftp_core_eof_pending());
    assert(!ftp_core_upload_chunk(bytes, length, clock_ms));
    finish(); file_is(bytes, length, true);
}
static void protocol(void)
{
    boot(0u);
    input("PASS x\r\nSIZE hello\r\nUSER root\r\nPASS x\r\n");
    expect("503 Send USER anonymous first.\r\n"); expect("530 Log in first.\r\n");
    expect("530 Anonymous only.\r\n"); expect("503 Send USER anonymous first.\r\n");
    input("syst\r\nFEAT\r\nNOOP\r\n");
    expect("215 UNIX Type: L8\r\n");
    expect("211-Features\r\n EPSV\r\n PASV\r\n SIZE\r\n211 End\r\n"); expect("200 OK.\r\n");
    input("USER anonymous\r\nPASS\r\n");
    expect("331 Send password.\r\n"); expect("230 Logged in.\r\n");
    passive(true);
    input("STOR hello\r\n"); expect("504 Select TYPE I.\r\n");
    input("TYPE A\r\nTYPE I\r\nPWD\r\nXPWD\r\nCWD /\r\nCDUP\r\n");
    expect("504 Binary mode only.\r\n"); expect("200 Binary mode.\r\n");
    expect("257 \"/\"\r\n"); expect("257 \"/\"\r\n");
    expect("250 Root directory.\r\n"); expect("250 Root directory.\r\n");
    input("CWD ..\r\nSTOR other\r\nRETR hello\r\nSIZE /hello\r\nPORT x\r\nEPRT x\r\nLIST\r\n");
    expect("550 Root only.\r\n"); expect("550 File unavailable.\r\n");
    expect("550 File unavailable.\r\n"); expect("550 File unavailable.\r\n");
    for (unsigned i = 0u; i < 3u; ++i) { expect("502 Command not supported.\r\n"); }
    input("ABOR\r\nSTOR hello\r\n");
    expect("225 No transfer.\r\n"); expect("425 Use EPSV or PASV first.\r\n");
    (void)ftp_core_take_actions();
    const uint8_t span[] = "EPSV\r\nNOOP\r\n";
    size_t accepted;
    assert(ftp_core_control_span(span, sizeof span - 1u, clock_ms, &accepted) == FTP_INPUT_PARTIAL);
    assert(accepted == 6u);
    assert(ftp_core_passive_result(false, NULL, 0u, clock_ms));
    assert(ftp_core_control_span(span + accepted, 6u, clock_ms, &accepted) == FTP_INPUT_OK);
    assert(accepted == 6u);
    expect("425 Cannot open passive listener.\r\n"); expect("200 OK.\r\n");
    assert(!ftp_core_passive_result(true, ip, 1u, clock_ms));
    assert(!ftp_core_data_connected(clock_ms));
    (void)ftp_core_take_actions();
    ++groups;
}
static void round_trips(void)
{
    boot(1u); login();
    store(NULL, 0u);
    input("SIZE /hello\r\n"); expect("213 0\r\n");
    start(false, false);
    size_t n;
    assert(ftp_core_download_span(&n) != NULL && n == 0u);
    assert(ftp_core_download_enqueued(0u, clock_ms)); finish();
    for (unsigned repeat = 0u; repeat < 4u; ++repeat) {
        for (size_t i = 0u; i < sizeof binary; ++i) { binary[i] = (uint8_t)(i + repeat); }
        start(true, false);
        assert(ftp_core_upload_chunk(binary, 2048u, clock_ms));
        assert(ftp_core_upload_chunk(binary + 2048u, 2048u, clock_ms));
        assert(ftp_core_upload_eof(clock_ms));
        /* Previous generation differs from the newly staged one. */
        if (repeat == 0u) { file_is(NULL, 0u, true); }
        else {
            size_t old_length; bool old_exists;
            const uint8_t *old = ftp_core_file(&old_length, &old_exists);
            assert(old_exists && old_length == FIXTURE_CAPACITY);
            for (size_t i = 0u; i < old_length; ++i) {
                assert(old[i] == (uint8_t)(i + repeat - 1u));
            }
        }
        finish(); file_is(binary, FIXTURE_CAPACITY, true);
        input("SIZE hello\r\n"); expect("213 4096\r\n");
        start(false, (repeat & 1u) == 0u);
        const uint8_t *p = ftp_core_download_span(&n);
        assert(n == FIXTURE_CAPACITY && memcmp(p, binary, n) == 0);
        assert(!ftp_core_download_acked(1u, clock_ms));
        assert(!ftp_core_download_enqueued(4097u, clock_ms));
        assert(ftp_core_download_enqueued(1024u, clock_ms));
        assert(!ftp_core_download_acked(1025u, clock_ms));
        assert(ftp_core_download_acked(512u, clock_ms));
        assert(ftp_core_download_span(&n) == p + 1024u && n == 3072u);
        assert(ftp_core_download_enqueued(n, clock_ms)); empty();
        assert(ftp_core_download_acked(3584u, clock_ms)); finish();
    }
    store(NULL, 0u); ftp_core_reset(); file_is(NULL, 0u, false);
    ++groups;
}
static void preservation(void)
{
    boot(0u); login(); store(binary, 7u);
    start(true, true);
    assert(!ftp_core_upload_chunk(binary, 4097u, clock_ms));
    expect("552 File exceeds storage capacity.\r\n");
    file_is(binary, 7u, true);
    assert((ftp_core_take_actions() & FTP_ABORT_DATA) != 0u);
    assert(!ftp_core_upload_eof(clock_ms));
    start(true, false);
    assert(ftp_core_upload_chunk(binary, 4096u, clock_ms));
    assert(!ftp_core_upload_chunk(binary, 1u, clock_ms));
    expect("552 File exceeds storage capacity.\r\n"); file_is(binary, 7u, true);
    (void)ftp_core_take_actions();
    start(true, true); assert(ftp_core_upload_chunk(binary, 3u, clock_ms));
    input("SIZE hello\r\n"); expect("503 Operation in progress.\r\n");
    input("ABOR\r\n"); expect("426 Transfer aborted.\r\n225 Abort complete.\r\n");
    file_is(binary, 7u, true); (void)ftp_core_take_actions();
    start(true, true); assert(ftp_core_upload_chunk(binary, 10u, clock_ms));
    assert(ftp_core_upload_eof(clock_ms)); ftp_core_data_error(clock_ms);
    expect("426 Data connection failed.\r\n"); file_is(binary, 7u, true);
    assert(!ftp_core_data_close_result(true, clock_ms)); (void)ftp_core_take_actions();
    start(true, true); assert(ftp_core_upload_chunk(binary, 10u, clock_ms));
    ftp_core_session_lost(); empty(); file_is(binary, 7u, true);
    assert(ftp_core_direction() == FTP_IDLE);
    assert((ftp_core_take_actions() & (FTP_ABORT_DATA | FTP_ABORT_CONTROL)) ==
           (FTP_ABORT_DATA | FTP_ABORT_CONTROL));
    assert(ftp_core_session_open(clock_ms)); expect("220 RAM FTP ready.\r\n"); login();
    start(false, true); ftp_core_data_error(clock_ms);
    expect("426 Data connection failed.\r\n"); file_is(binary, 7u, true);
    (void)ftp_core_take_actions();
    start(true, true); assert(ftp_core_upload_eof(clock_ms));
    input("QUIT\r\nSTOR hello\r\n"); expect("221 Goodbye.\r\n");
    unsigned a = ftp_core_take_actions();
    assert((a & FTP_CLOSE_CONTROL) != 0u && (a & FTP_ABORT_DATA) != 0u);
    assert(!ftp_core_data_close_result(true, clock_ms)); file_is(binary, 7u, true);
    ftp_core_control_closed(); assert(ftp_core_take_actions() == 0u);
    assert(ftp_core_session_open(clock_ms)); expect("220 RAM FTP ready.\r\n");
    ++groups;
}
static void parser_tests(void)
{
    boot(0u);
    const char *login_text = "USER anonymous\r\nPASS x\r\nTYPE I\r\n";
    for (size_t i = 0u; i < strlen(login_text); ++i) {
        assert(ftp_core_control_input((const uint8_t *)login_text + i, 1u, clock_ms) == FTP_INPUT_OK);
    }
    expect("331 Send password.\r\n"); expect("230 Logged in.\r\n"); expect("200 Binary mode.\r\n");
    uint8_t nul[] = {'N','O',0,'O','P','\r','\n'};
    assert(ftp_core_control_input(nul, sizeof nul, clock_ms) == FTP_INPUT_OK);
    expect("500 Malformed command line.\r\n");
    input("NOOP\nNOOP\r\nNOOP\rX\r\nNOOP\r\r\n");
    for (unsigned i = 0u; i < 3u; ++i) { expect("500 Malformed command line.\r\n"); }
    uint8_t line[FTP_CONTROL_CAPACITY];
    memset(line, 'A', sizeof line); line[254] = '\r'; line[255] = '\n';
    assert(ftp_core_control_input(line, sizeof line, clock_ms) == FTP_INPUT_OK);
    expect("502 Command not supported.\r\n");
    memset(line, 'A', sizeof line);
    assert(ftp_core_control_input(line, 255u, clock_ms) == FTP_INPUT_OK);
    input("\r\n"); expect("500 Malformed command line.\r\n");
    for (unsigned i = 0u; i < 8u; ++i) {
        assert(ftp_core_control_input(line, sizeof line, clock_ms) == FTP_INPUT_OK);
    }
    input("\r\nNOOP\r\n"); expect("500 Malformed command line.\r\n"); expect("200 OK.\r\n");
    assert(ftp_core_control_input(binary, 257u, clock_ms) == FTP_INPUT_REJECTED);
    assert(ftp_core_control_input(NULL, 1u, clock_ms) == FTP_INPUT_REJECTED);
    assert(ftp_core_control_input(NULL, 0u, clock_ms) == FTP_INPUT_OK);
    input("NOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\n");
    const char *batch = "USER root\r\nNOOP\r\n";
    assert(ftp_core_control_input((const uint8_t *)batch, strlen(batch), clock_ms) == FTP_INPUT_BACKPRESSURE);
    for (unsigned i = 0u; i < 6u; ++i) { expect("200 OK.\r\n"); }
    /* Rejected batch did not revoke login or leave a partial parser suffix. */
    input("PWD\r\n"); expect("257 \"/\"\r\n");
    input("NOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\n");
    input("NO");
    assert(ftp_core_control_input((const uint8_t *)"OP\r\n", 4u, clock_ms) == FTP_INPUT_BACKPRESSURE);
    for (unsigned i = 0u; i < 7u; ++i) { expect("200 OK.\r\n"); }
    input("OP\r\n"); expect("200 OK.\r\n");
    input("NOOP\r\n"); size_t length;
    const uint8_t *p = ftp_core_reply_peek(&length);
    assert(length == 9u && p[0] == '2');
    assert(ftp_core_reply_consume(2u, clock_ms));
    expect("0 OK.\r\n");
    ++groups;
}
static void asynchronous_queue(void)
{
    boot(0u); login();
    size_t accepted;
    const uint8_t span[] = "EPSV\r\nNOOP\r\n";
    assert(ftp_core_control_span(span, sizeof span - 1u, clock_ms, &accepted) == FTP_INPUT_PARTIAL);
    assert(accepted == 6u);
    assert((ftp_core_take_actions() & FTP_OPEN_PASSIVE) != 0u);
    assert(ftp_core_control_span(span + 6u, 6u, clock_ms, &accepted) == FTP_INPUT_BACKPRESSURE);
    assert(accepted == 0u);
    assert(ftp_core_passive_result(true, NULL, UINT16_MAX, clock_ms));
    input("NOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\n");
    assert(ftp_core_control_input((const uint8_t *)"ABOR\r\n", 6u, clock_ms) == FTP_INPUT_BACKPRESSURE);
    expect("229 Entering Extended Passive Mode (|||65535|).\r\n");
    for (unsigned i = 0u; i < 6u; ++i) { expect("200 OK.\r\n"); }
    assert(ftp_core_data_connected(clock_ms)); /* Rejected ABOR had no effect. */
    input("ABOR\r\n"); expect("225 No transfer.\r\n"); (void)ftp_core_take_actions();
    input("PASV\r\n"); (void)ftp_core_take_actions();
    assert(ftp_core_passive_result(true, NULL, 123u, clock_ms));
    expect("425 Cannot open passive listener.\r\n"); (void)ftp_core_take_actions();
    input("EPSV\r\n"); (void)ftp_core_take_actions();
    assert(ftp_core_passive_result(true, NULL, 0u, clock_ms));
    expect("425 Cannot open passive listener.\r\n"); (void)ftp_core_take_actions();
    const char *many = "NOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\nNOOP\r\n";
    assert(ftp_core_control_input((const uint8_t *)many, strlen(many), clock_ms) == FTP_INPUT_BACKPRESSURE);
    empty(); input("NOOP\r\n"); expect("200 OK.\r\n");
    ++groups;
}
static void stale_start(void)
{
    boot(0u); login(); passive(true);
    input("STOR hello\r\nABOR\r\nEPSV\r\n");
    assert((ftp_core_take_actions() & FTP_OPEN_PASSIVE) != 0u);
    assert(ftp_core_passive_result(true, ip, 21u, clock_ms));
    assert(ftp_core_data_connected(clock_ms));
    input("STOR hello\r\n");
    expect("150 Opening binary data connection.\r\n");
    assert(!ftp_core_transfer_ready()); /* Old 150 cannot enable the new upload. */
    expect("426 Transfer aborted.\r\n225 Abort complete.\r\n");
    expect("229 Entering Extended Passive Mode (|||21|).\r\n");
    expect("150 Opening binary data connection.\r\n");
    assert(ftp_core_transfer_ready());
    ++groups;
}
static void timers(uint32_t origin)
{
    boot(origin); login(); store(binary, 7u);
    start(true, true); assert(ftp_core_upload_chunk(binary, 1u, clock_ms));
    clock_ms = origin + 14999u; ftp_core_tick(clock_ms); empty();
    clock_ms = origin + 15000u; ftp_core_tick(clock_ms);
    expect("426 Transfer timed out.\r\n"); file_is(binary, 7u, true);
    (void)ftp_core_take_actions();
    passive(true);
    clock_ms += 14999u; ftp_core_tick(clock_ms); empty();
    ++clock_ms; ftp_core_tick(clock_ms); expect("425 Passive connection timed out.\r\n");
    (void)ftp_core_take_actions();
    /* Close is allowed to retry, but it must not commit before success. */
    start(true, true); assert(ftp_core_upload_chunk(binary, 1u, clock_ms));
    assert(ftp_core_upload_eof(clock_ms));
    clock_ms += 4999u; ftp_core_tick(clock_ms); assert(ftp_core_eof_pending());
    ++clock_ms; ftp_core_tick(clock_ms); expect("426 Data close timed out.\r\n");
    file_is(binary, 7u, true); (void)ftp_core_take_actions();
    boot(origin); login(); store(binary, 7u); start(true, true);
    for (unsigned i = 1u; i <= 5u; ++i) {
        clock_ms = origin + i * 10000u;
        input("NOOP\r\n"); expect("200 OK.\r\n");
        assert(ftp_core_upload_chunk(binary, 1u, clock_ms)); ftp_core_tick(clock_ms); empty();
    }
    clock_ms = origin + 59999u; ftp_core_tick(clock_ms); empty();
    ++clock_ms; ftp_core_tick(clock_ms); expect("426 Transfer timed out.\r\n");
    file_is(binary, 7u, true);
    boot(origin); login(); store(binary, 7u); start(false, true);
    clock_ms = origin + 4999u; ftp_core_tick(clock_ms); empty();
    ++clock_ms; ftp_core_tick(clock_ms); expect("426 Data output timed out.\r\n");
    file_is(binary, 7u, true);
    boot(origin); login(); store(binary, 7u); start(false, true);
    assert(ftp_core_download_enqueued(7u, clock_ms));
    clock_ms = origin + 14999u; ftp_core_tick(clock_ms); empty();
    ++clock_ms; ftp_core_tick(clock_ms); expect("426 Transfer timed out.\r\n");
    file_is(binary, 7u, true);
    boot(origin); input("N");
    clock_ms = origin + 9999u; ftp_core_tick(clock_ms);
    input("O"); ++clock_ms; ftp_core_tick(clock_ms);
    assert((ftp_core_take_actions() & FTP_ABORT_CONTROL) != 0u);
    assert(ftp_core_control_input((const uint8_t *)"OP\r\n", 4u, clock_ms) == FTP_INPUT_REJECTED);
    boot(origin);
    clock_ms = origin + 59999u; ftp_core_tick(clock_ms);
    assert(ftp_core_take_actions() == 0u);
    ++clock_ms; ftp_core_tick(clock_ms);
    assert((ftp_core_take_actions() & FTP_ABORT_CONTROL) != 0u);
    boot(origin); input("NOOP\r\n");
    clock_ms = origin + 4999u; ftp_core_tick(clock_ms);
    assert(ftp_core_take_actions() == 0u);
    ++clock_ms; ftp_core_tick(clock_ms); empty();
    assert((ftp_core_take_actions() & FTP_ABORT_CONTROL) != 0u);
    boot(origin); input("QUIT\r\n"); expect("221 Goodbye.\r\n");
    assert((ftp_core_take_actions() & FTP_CLOSE_CONTROL) != 0u);
    clock_ms = origin + 5000u; ftp_core_tick(clock_ms);
    assert((ftp_core_take_actions() & FTP_ABORT_CONTROL) != 0u);
    ++groups;
}
/* Spy delegates byte storage to a real caller-owned RAM backend. Failures
 * happen before mutation; counters prove that the core uses only the contract. */
static struct {
    ram_file_t file;
    uint8_t a[17], b[17];
    unsigned calls, clears, begins, appends, commits, aborts;
    unsigned fail;
} spy;
static const ftp_storage_t *spy_ram(void) { return ram_file_storage(&spy.file); }
static bool spy_name(void *context, const char *name, size_t length)
{
    assert(context == &spy); ++spy.calls;
    return spy_ram()->ops->matches_name(spy_ram()->context, name, length);
}
static void spy_clear(void *context)
{
    assert(context == &spy); ++spy.calls; ++spy.clears;
    spy_ram()->ops->clear(spy_ram()->context);
}
static void spy_inspect(void *context, ftp_storage_view_t *view)
{
    assert(context == &spy); ++spy.calls;
    spy_ram()->ops->inspect(spy_ram()->context, view);
}
static ftp_storage_result_t spy_begin(void *context)
{
    assert(context == &spy); ++spy.calls; ++spy.begins;
    return spy.fail == 1u ? FTP_STORAGE_ERROR : spy_ram()->ops->begin(spy_ram()->context);
}
static ftp_storage_result_t spy_append(void *context, const uint8_t *bytes, size_t length)
{
    assert(context == &spy); ++spy.calls; ++spy.appends;
    return spy.fail == 2u ? FTP_STORAGE_ERROR :
        spy_ram()->ops->append(spy_ram()->context, bytes, length);
}
static void spy_abort(void *context)
{
    assert(context == &spy); ++spy.calls; ++spy.aborts;
    spy_ram()->ops->abort(spy_ram()->context);
}
static ftp_storage_result_t spy_commit(void *context)
{
    assert(context == &spy); ++spy.calls; ++spy.commits;
    return spy.fail == 3u ? FTP_STORAGE_ERROR : spy_ram()->ops->commit(spy_ram()->context);
}
static const ftp_storage_ops_t spy_ops = {
    spy_name, spy_clear, spy_inspect, spy_begin, spy_append, spy_abort, spy_commit
};
static const ftp_storage_t spy_binding = { &spy_ops, &spy, 17u };
static void spy_start(void)
{
    passive(true); assert(ftp_core_data_connected(clock_ms));
    input("STOR /payload\r\n"); expect("150 Opening binary data connection.\r\n");
    assert(ftp_core_transfer_ready()); (void)ftp_core_take_actions();
}
static void invalid_binding(const ftp_storage_t *binding)
{
    unsigned calls = spy.calls;
    assert(!ftp_core_init(binding));
    assert(spy.calls == calls && ftp_core_is_initialized());
}
static void backend_contract(void)
{
    /* Explicit reset releases the prior test's protocol state before rebind. */
    ftp_core_reset();
    assert(ram_file_init(&spy.file, spy.a, spy.b, 17u, "payload"));
    /* Preexisting committed bytes must survive a valid bind. */
    assert(spy_ram()->ops->begin(spy_ram()->context) == FTP_STORAGE_OK);
    assert(spy_ram()->ops->append(spy_ram()->context, binary, 7u) == FTP_STORAGE_OK);
    assert(spy_ram()->ops->commit(spy_ram()->context) == FTP_STORAGE_OK);
    assert(ftp_core_init(&spy_binding) && spy.calls == 0u);
    file_is(binary, 7u, true);
    invalid_binding(NULL);
    ftp_storage_t bad = spy_binding;
    bad.context = NULL; invalid_binding(&bad);
    bad = spy_binding; bad.ops = NULL; invalid_binding(&bad);
    bad = spy_binding; bad.capacity = 0u; invalid_binding(&bad);
#if SIZE_MAX > UINT_MAX
    bad = spy_binding; bad.capacity = (size_t)UINT_MAX + 1u; invalid_binding(&bad);
#endif
    ftp_storage_ops_t ops;
    bad = spy_binding; bad.ops = &ops;
    for (unsigned i = 0u; i < 7u; ++i) {
        ops = spy_ops;
        switch (i) {
        case 0u: ops.matches_name = NULL; break;
        case 1u: ops.clear = NULL; break;
        case 2u: ops.inspect = NULL; break;
        case 3u: ops.begin = NULL; break;
        case 4u: ops.append = NULL; break;
        case 5u: ops.abort = NULL; break;
        default: ops.commit = NULL; break;
        }
        invalid_binding(&bad); file_is(binary, 7u, true);
    }
    unsigned calls = spy.calls;
    assert(ftp_core_init(&spy_binding) && spy.calls == calls);
    file_is(binary, 7u, true);
    assert(ftp_core_session_open(clock_ms)); expect("220 RAM FTP ready.\r\n"); login();
    input("NOOP\r\n"); invalid_binding(&spy_binding); expect("200 OK.\r\n");
    input("SIZE hello\r\nSIZE /payload\r\n");
    expect("550 File unavailable.\r\n"); expect("213 7\r\n");
    for (unsigned fault = 1u; fault <= 3u; ++fault) {
        size_t n; bool exists;
        const uint8_t *old = ftp_core_file(&n, &exists);
        unsigned begins = spy.begins, appends = spy.appends, commits = spy.commits;
        unsigned aborts = spy.aborts;
        if (fault == 1u) {
            passive(true); assert(ftp_core_data_connected(clock_ms));
            spy.fail = fault; input("STOR payload\r\n");
        } else {
            spy_start(); spy.fail = fault;
            if (fault == 2u) { assert(!ftp_core_upload_chunk(binary + 7u, 10u, clock_ms)); }
            else {
                assert(ftp_core_upload_chunk(binary + 7u, 10u, clock_ms));
                assert(ftp_core_upload_eof(clock_ms));
                assert(ftp_core_data_close_result(false, clock_ms));
                assert(spy.commits == commits); file_is(binary, 7u, true);
                assert(!ftp_core_data_close_result(true, clock_ms));
            }
        }
        expect("451 Storage operation failed.\r\n"); empty(); /* never 226 */
        assert(spy.begins == begins + 1u && spy.aborts > aborts);
        assert(spy.appends == appends + (fault == 1u ? 0u : 1u));
        assert(spy.commits == commits + (fault == 3u ? 1u : 0u));
        file_is(binary, 7u, true);
        assert(ftp_core_file(&n, &exists) == old && n == 7u && exists);
        assert(!ftp_core_upload_eof(clock_ms));
        assert(!ftp_core_data_close_result(true, clock_ms)); empty();
        (void)ftp_core_take_actions(); spy.fail = 0u;
    }
    spy_start(); assert(ftp_core_upload_chunk(binary, 17u, clock_ms));
    assert(ftp_core_upload_eof(clock_ms)); file_is(binary, 7u, true);
    finish(); file_is(binary, 17u, true);
    input("SIZE payload\r\n"); expect("213 17\r\n");
    passive(false); assert(ftp_core_data_connected(clock_ms));
    input("RETR /payload\r\n"); expect("150 Opening binary data connection.\r\n");
    size_t n; const uint8_t *bytes = ftp_core_download_span(&n);
    assert(n == 17u && memcmp(bytes, binary, n) == 0);
    assert(ftp_core_download_enqueued(17u, clock_ms)); empty();
    assert(ftp_core_download_acked(17u, clock_ms)); finish();
    spy_start(); assert(ftp_core_upload_chunk(binary, 17u, clock_ms));
    assert(!ftp_core_upload_chunk(binary + 17u, 1u, clock_ms));
    expect("552 File exceeds storage capacity.\r\n"); empty(); file_is(binary, 17u, true);
    (void)ftp_core_take_actions();
    unsigned clears = spy.clears; ftp_core_reset();
    assert(ftp_core_is_initialized() && spy.clears == clears + 1u);
    file_is(NULL, 0u, false);
    assert(ftp_core_session_open(clock_ms)); expect("220 RAM FTP ready.\r\n"); login();
    spy_start(); assert(ftp_core_upload_chunk(binary, 17u, clock_ms));
    assert(ftp_core_upload_eof(clock_ms)); finish(); file_is(binary, 17u, true);
    ftp_core_reset(); ++groups;
}
int main(void)
{
    assert(!ftp_core_is_initialized());
    assert(ram_file_init(&fixture, fixture_a, fixture_b, FIXTURE_CAPACITY, "hello"));
    assert(ftp_core_init(ram_file_storage(&fixture)));
    for (size_t i = 0u; i < sizeof binary; ++i) { binary[i] = (uint8_t)i; }
    protocol(); round_trips(); preservation(); parser_tests(); asynchronous_queue(); stale_start();
    timers(0u); timers(UINT32_MAX - 3000u); backend_contract();
    printf("ftp_core_test: PASS (%u groups; offline ASan/UBSan assertions)\n", groups);
    return 0;
}
