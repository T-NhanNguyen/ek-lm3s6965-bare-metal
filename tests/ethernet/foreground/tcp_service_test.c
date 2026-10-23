#include "check.h"
#define main tcp_legacy_main
#include "../ftp/ftp_tcp_test.c"
#undef main
static unsigned steps, begins, ends;
static bool recursive;
static bool matches(void *p, const char *name, size_t n, bool up)
{ CHECK(p == &steps); return up && n == 6 && !memcmp(name, "update", 6); }
static bool begin(void *p, const char *name, size_t n, bool up, ftp_storage_view_t *v)
{ CHECK(matches(p, name, n, up)); (void)v; ++begins; return true; }
static ftp_operation_result_t feed(void *p, const uint8_t *b, size_t n, size_t *used)
{ CHECK(p == &steps); (void)b; *used = n; return FTP_OPERATION_OK; }
static bool eof(void *p) { CHECK(p == &steps); return true; }
static ftp_operation_result_t step(void *p)
{
    CHECK(p == &steps); ++steps;
    if (recursive) CHECK(!ftp_tcp_service(clock_ms));
    return FTP_OPERATION_WAIT;
}
static void end(void *p) { CHECK(p == &steps); ++ends; }
static const ftp_operation_ops_t ops = {matches, begin, feed, eof, step, end};
static const ftp_operation_t operation_fixture = {&ops, &steps};
static void init_ram(void)
{
    CHECK(ram_file_init(&fixture, fixture_a, fixture_b, 4096u, "hello"));
    CHECK(ftp_core_init(ram_file_storage(&fixture))); ftp_core_reset();
}
static int run_case(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "ftp_missing_core_sticky")) {
        CHECK(!ftp_core_is_initialized());
        CHECK(!ftp_tcp_service(0));
        CHECK(allocated == 0 && steps == 0);
        init_ram(); CHECK(!ftp_tcp_service(1));
        CHECK(allocated == 0 && steps == 0); return 0;
    }
    CHECK(!strcmp(argv[1], "ftp_one_step_then_reentry_sticky"));
    init_ram(); CHECK(ftp_core_bind_operation(&operation_fixture)); start();
    struct tcp_pcb *p = passive_connect();
    deliver(control.pcb, "STOR update\r\n", 13, 2);
    CHECK(steps == 0 && begins == 0); /* raw receive owns bytes only */
    CHECK(ftp_tcp_service(clock_ms));
    CHECK(steps == 1 && begins == 1 && ends == 0);
    /* Handled control backpressure is completion and permits only one step. */
    deliver(control.pcb, "NOOP\r\n", 6, 2);
    const unsigned before = steps; fail_write = 1;
    CHECK(ftp_tcp_service(clock_ms)); CHECK(steps == before + 1);
    deliver(p, "abc", 3, 1); CHECK(steps == before + 1);
    recursive = true;
    CHECK(!ftp_tcp_service(clock_ms)); CHECK(steps == before + 2);
    CHECK(!ftp_tcp_service(clock_ms)); CHECK(steps == before + 2);
    ftp_tcp_link_down(); CHECK(ends == 1 && !live_pbufs);
    ip4_addr_t ip; IP4_ADDR(&ip, 192,168,7,2);
    CHECK(ftp_tcp_start(&ip) == ERR_OK);
    CHECK(!ftp_tcp_service(clock_ms)); CHECK(steps == before + 2);
    ftp_tcp_link_down(); return 0;
}

int main(int argc, char **argv)
{
    int result = run_case(argc, argv);
    if (result == 0) printf("FOREGROUND_CASE_OK %s\n", argv[1]);
    return result;
}
