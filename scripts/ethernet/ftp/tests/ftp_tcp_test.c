/* Offline deterministic raw API fault injection; no IP stack or sockets.
 * Real lwIP 2.2.1 headers ensure adapter signatures and PCB fields match. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lwip/tcp.h"
#include <stdlib.h>
#include "lwip/priv/tcp_priv.h"
#include "../../../../src/ftp_tcp.c"
#include "lm3s6965/ram_file.h"
static ram_file_t fixture;
static uint8_t fixture_a[4096], fixture_b[4096];

struct record {
    struct tcp_pcb p;
    bool dead, closing_owned;
    unsigned credits, aborts, closes, outputs;
    size_t length;
    uint8_t bytes[16384];
};
struct tcp_pcb *tcp_active_pcbs, *tcp_tw_pcbs, *tcp_bound_pcbs;
static struct record records[256];
static unsigned allocated, freed_pbufs, live_pbufs;
static int fail_new, fail_bind, fail_listen, fail_write, fail_output, fail_close;
static uint32_t clock_ms;
static struct record *record(struct tcp_pcb *p)
{
    for (unsigned i = 0; i < allocated; ++i)
        if (&records[i].p == p) return &records[i];
    assert(0); return NULL;
}
static struct record *live(struct tcp_pcb *p)
{
    struct record *r = record(p); assert(!r->dead); return r;
}
u32_t sys_now(void) { return clock_ms; }
struct tcp_pcb *tcp_new_ip_type(u8_t type)
{
    (void)type;
    if (fail_new) { --fail_new; return NULL; }
    assert(allocated < 256);
    struct record *r = &records[allocated++];
    memset(r, 0, sizeof *r);
    r->p.snd_buf = 2144;
    r->p.state = ESTABLISHED;
    return &r->p;
}
err_t tcp_bind(struct tcp_pcb *p, const ip_addr_t *ip, u16_t port)
{
    live(p);
    if (fail_bind) { --fail_bind; return ERR_USE; }
    p->local_ip = *ip;
    p->local_port = port ? port : (u16_t)(49152 + allocated);
    return ERR_OK;
}
struct tcp_pcb *tcp_listen_with_backlog_and_err(struct tcp_pcb *p, u8_t backlog, err_t *e)
{
    (void)backlog; live(p);
    if (fail_listen) { --fail_listen; *e = ERR_MEM; return NULL; }
    p->state = LISTEN; *e = ERR_OK; return p;
}
void tcp_arg(struct tcp_pcb *p, void *a) { live(p); p->callback_arg = a; }
void tcp_recv(struct tcp_pcb *p, tcp_recv_fn f) { live(p); p->recv = f; }
void tcp_sent(struct tcp_pcb *p, tcp_sent_fn f) { live(p); p->sent = f; }
void tcp_err(struct tcp_pcb *p, tcp_err_fn f) { live(p); p->errf = f; }
/* The fake keeps full PCBs for listeners; store accept separately because real
 * listening PCBs have a different layout. */
static tcp_accept_fn accepts[256];
void tcp_accept(struct tcp_pcb *p, tcp_accept_fn f)
{ live(p); accepts[record(p) - records] = f; }
void tcp_recved(struct tcp_pcb *p, u16_t n) { live(p)->credits += n; }
err_t tcp_write(struct tcp_pcb *p, const void *b, u16_t n, u8_t flags)
{
    struct record *r = live(p);
    assert(flags & TCP_WRITE_FLAG_COPY);
    if (fail_write) { --fail_write; return ERR_MEM; }
    assert(r->length + n <= sizeof r->bytes);
    memcpy(r->bytes + r->length, b, n); r->length += n;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *p)
{
    ++live(p)->outputs;
    if (fail_output) { --fail_output; return ERR_IF; }
    return ERR_OK;
}
err_t tcp_close(struct tcp_pcb *p)
{
    struct record *r = live(p);
    ++r->closes;
    assert(!p->recv && !p->sent && !p->errf && !p->callback_arg);
    r->dead = true;
    if (p->state != LISTEN) {
        r->closing_owned = true;
        p->next = tcp_active_pcbs;
        tcp_active_pcbs = p;
    }
    return ERR_OK;
}
err_t tcp_shutdown(struct tcp_pcb *p, int rx, int tx)
{
    live(p); assert(!rx && tx);
    if (fail_close) {
        --fail_close; p->flags |= TF_CLOSEPEND;
        return ERR_OK; /* Match real lwIP, NOT fictional close ERR_MEM. */
    }
    assert(!(p->flags & TF_CLOSEPEND));
    p->state = FIN_WAIT_1;
    return ERR_OK;
}
err_t tcp_process_refused_data(struct tcp_pcb *p)
{
    struct pbuf *b = p->refused_data;
    p->refused_data = NULL;
    err_t e = p->recv(p->callback_arg, p, b, ERR_OK);
    if (e != ERR_OK) p->refused_data = b;
    return e;
}
void tcp_abandon(struct tcp_pcb *p, int reset)
{
    struct record *r = record(p);
    assert(!r->dead || r->closing_owned);
    if (r->closing_owned) {
        struct tcp_pcb **slot = &tcp_active_pcbs;
        while (*slot && *slot != p) slot = &(*slot)->next;
        assert(*slot == p); *slot = p->next;
        r->closing_owned = false;
    }
    tcp_err_fn f = p->errf;
    void *a = p->callback_arg;
    assert(p->state != LISTEN);
    if (!reset) assert(!p->errf); /* link-down callbacks detached */
    ++r->aborts; r->dead = true;
    /* Synchronous callback after freeing, as in lwIP. */
    if (f) f(a, ERR_ABRT);
}
void tcp_abort(struct tcp_pcb *p) { tcp_abandon(p, 1); }
u8_t pbuf_free(struct pbuf *p)
{
    u8_t count = 0;
    while (p) {
        struct pbuf *next = p->next;
        assert(p->ref == 1);
        free(p->payload); free(p);
        ++freed_pbufs; --live_pbufs; ++count; p = next;
    }
    return count;
}
u16_t pbuf_copy_partial(const struct pbuf *p, void *dest, u16_t n, u16_t off)
{
    u16_t copied = 0;
    while (p && off >= p->len) { off -= p->len; p = p->next; }
    while (p && copied < n) {
        u16_t take = p->len - off;
        if (take > n - copied) take = n - copied;
        memcpy((uint8_t *)dest + copied, (uint8_t *)p->payload + off, take);
        copied += take; off = 0; p = p->next;
    }
    return copied;
}
static struct pbuf *chain(const void *bytes, size_t n, size_t split)
{
    assert(n && n <= 65535);
    struct pbuf *p = calloc(1, sizeof *p);
    assert(p);
    p->len = (u16_t)(n > split ? split : n);
    p->tot_len = (u16_t)n; p->ref = 1;
    p->payload = malloc(p->len); assert(p->payload);
    memcpy(p->payload, bytes, p->len); ++live_pbufs;
    if (n > p->len) p->next = chain((const uint8_t *)bytes + p->len, n - p->len, split);
    return p;
}
static void service(void) { ftp_tcp_service(clock_ms); }
static void loops(unsigned n) { while (n--) service(); }
static void deliver(struct tcp_pcb *p, const void *b, size_t n, size_t split)
{
    assert(p->recv(p->callback_arg, p, chain(b, n, split), ERR_OK) == ERR_OK);
}
static void command(const char *s)
{ deliver(control.pcb, s, strlen(s), 3); loops(40); }
static struct tcp_pcb *connect_to(struct tcp_pcb *l, unsigned peer, err_t expected)
{
    struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_V4);
    assert(p);
    IP_ADDR4(&p->remote_ip, 192, 168, 7, peer);
    assert(accepts[record(l) - records](NULL, p, ERR_OK) == expected);
    return p;
}
static void start(void)
{
    ip4_addr_t ip; IP4_ADDR(&ip, 192, 168, 7, 2);
    assert(ftp_tcp_start(&ip) == ERR_OK);
    struct tcp_pcb *l = listener;
    assert(ftp_tcp_start(&ip) == ERR_OK && listener == l);
    connect_to(listener, 10, ERR_OK); loops(10);
    command("USER anonymous\r\nPASS\r\nTYPE I\r\n");
}
static struct tcp_pcb *passive_connect(void)
{
    command("EPSV\r\n"); assert(passive && passive->local_port >= 49152);
    return connect_to(passive, 10, ERR_OK);
}
static void fin(struct tcp_pcb *p)
{ assert(p->recv(p->callback_arg, p, NULL, ERR_OK) == ERR_OK); }
static void rst(struct tcp_pcb *p)
{
    struct record *r = live(p);
    tcp_err_fn f = p->errf; void *a = p->callback_arg;
    r->dead = true; f(a, ERR_RST);
}
static void file_is(const void *b, size_t n)
{
    size_t size; bool exists;
    const uint8_t *f = ftp_core_file(&size, &exists);
    assert(exists && size == n && !memcmp(f, b, n));
}
static bool contains(struct record *r, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= r->length; ++i)
        if (!memcmp(r->bytes + i, s, n)) return true;
    return false;
}
static void upload(const void *b, size_t n)
{
    struct tcp_pcb *p = passive_connect();
    command("STOR hello\r\n");
    if (n) deliver(p, b, n, 7);
    loops(20); fin(p); loops(20); file_is(b, n);
}
int main(void)
{
    ip4_addr_t ip; IP4_ADDR(&ip, 192, 168, 7, 2);
    ftp_tcp_link_down();
    assert(!ftp_core_is_initialized());
    assert(ftp_tcp_start(&ip) == ERR_VAL && !listener && allocated == 0);
    assert(ram_file_init(&fixture, fixture_a, fixture_b, 4096u, "hello"));
    assert(ftp_core_init(ram_file_storage(&fixture)));
    ftp_core_reset();
    fail_new = 1; assert(ftp_tcp_start(&ip) == ERR_MEM && !listener);
    fail_bind = 1; assert(ftp_tcp_start(&ip) == ERR_USE && !listener);
    fail_listen = 1; assert(ftp_tcp_start(&ip) == ERR_MEM && !listener);
    start();
    struct tcp_pcb *owner = control.pcb;
    connect_to(listener, 11, ERR_ABRT); assert(control.pcb == owner);
    command("PASV\r\n"); assert(passive);
    assert(contains(record(owner), "227 Entering Passive Mode (192,168,7,2,"));
    struct tcp_pcb *l = passive;
    connect_to(l, 11, ERR_ABRT); assert(passive == l && control.pcb == owner);
    struct tcp_pcb *p = connect_to(l, 10, ERR_OK);
    assert(!passive);
    struct tcp_pcb *extra = tcp_new_ip_type(IPADDR_TYPE_V4);
    extra->remote_ip = p->remote_ip;
    assert(accept_data(NULL, extra, ERR_OK) == ERR_ABRT && data.pcb == p);
    /* Already-connected data is held before STOR/150; second pbuf refused
     * completely unchanged, then retried once after prefix processing. */
    const uint8_t original[] = { 1, 0, 3, 4, 5 };
    deliver(p, original, 3, 1);
    struct pbuf *retry = chain(original + 3, 2, 1);
    unsigned freed = freed_pbufs;
    assert(p->recv(p->callback_arg, p, retry, ERR_OK) == ERR_MEM);
    assert(freed_pbufs == freed && record(p)->credits == 0);
    loops(3); assert(record(p)->credits == 0);
    fail_write = 2;
    deliver(owner, "STOR hello\r\n", 12, 2);
    service(); assert(record(p)->credits == 0);
    service(); assert(record(p)->credits == 0);
    service(); assert(record(p)->credits == 3);
    assert(p->recv(p->callback_arg, p, retry, ERR_OK) == ERR_OK);
    loops(4); assert(record(p)->credits == 5);
    fin(p); fail_close = 2;
    service(); assert(data.pcb == p && p->recv && p->errf);
    size_t size; bool exists; ftp_core_file(&size, &exists); assert(!exists);
    service(); assert(data.pcb == p);
    service(); assert(!data.pcb && record(p)->dead); file_is(original, 5);
    loops(10); assert(contains(record(owner), "226 "));

    /* Failed control output is retried without replaying its reply. */
    size_t output_base = record(owner)->length;
    deliver(owner, "NOOP\r\n", 6, 2);
    fail_output = 2; service();
    assert(control.output && record(owner)->length == output_base + 9);
    service(); service();
    assert(!control.output && record(owner)->length == output_base + 9);

    /* Output failure retains only output work; never rewrites accepted bytes.
     * Downloads close only after ALL ACKs, then successful close initiation. */
    p = passive_connect();
    deliver(owner, "RETR hello\r\n", 12, 3);
    fail_write = 1; service(); assert(record(p)->length == 0);
    fail_output = 1; service(); assert(record(p)->length == 0);
    fail_write = 1; service(); assert(record(p)->length == 0);
    fail_output = 2; service();
    assert(data.output && record(p)->length == 5);
    loops(5);
    assert(record(p)->length == 5 && data.pcb == p);
    assert(!memcmp(record(p)->bytes, original, 5));
    fail_output = 2; data.output = true;
    loops(3); assert(record(p)->length == 5 && !record(p)->closes);
    fin(p); service(); assert(data.pcb == p); /* normal half-close */
    assert(p->sent(p->callback_arg, p, 2) == ERR_OK); service();
    assert(!record(p)->closes);
    fail_close = 1;
    assert(p->sent(p->callback_arg, p, 3) == ERR_OK); service();
    assert(data.pcb == p && p->sent); service(); assert(!data.pcb);
    file_is(original, 5);

    /* RST while upload close pending must never commit. */
    p = passive_connect(); command("STOR hello\r\n");
    deliver(p, "replacement", 11, 2); loops(4); fin(p);
    fail_close = 1; service(); assert(data.pcb == p);
    rst(p); service(); file_is(original, 5);

    /* Control/link loss before FIN acceptance also retain the old file. */
    p = passive_connect(); command("STOR hello\r\n");
    deliver(p, "bad", 3, 1); loops(4); fin(p);
    fail_close = 1; service(); assert(data.pcb == p);
    rst(owner); service(); file_is(original, 5);
    assert(!control.pcb && !data.pcb);
    ftp_tcp_link_down(); start(); owner = control.pcb;
    p = passive_connect(); command("STOR hello\r\n");
    deliver(p, "bad", 3, 1); loops(4); fin(p);
    fail_close = 1; service(); assert(data.pcb == p);
    ftp_tcp_link_down(); file_is(original, 5);
    start(); owner = control.pcb;

    /* Accepted prefix of a large control chain survives core backpressure;
     * refused second chain has no ACK/free/input effects. */
    uint8_t commands[600];
    for (unsigned i = 0; i < 100; ++i) memcpy(commands + i * 6, "NOOP\r\n", 6);
    size_t base = record(owner)->length;
    unsigned credits = record(owner)->credits;
    deliver(owner, commands, sizeof commands, 13);
    retry = chain("NOOP\r\n", 6, 2); freed = freed_pbufs;
    assert(owner->recv(owner->callback_arg, owner, retry, ERR_OK) == ERR_MEM);
    assert(record(owner)->credits == credits && freed == freed_pbufs);
    fail_write = 5; loops(5);
    assert(control.rx && control.offset > 0 && control.offset < sizeof commands);
    loops(150); assert(!control.rx && record(owner)->credits == credits + 600);
    assert(record(owner)->length == base + 100 * strlen("200 OK.\r\n"));
    assert(owner->recv(owner->callback_arg, owner, retry, ERR_OK) == ERR_OK);
    loops(10);

    /* Abort and deadlines preserve a committed file, release retained input,
     * and old callbacks cannot mutate a new connection. */
    p = passive_connect(); deliver(p, "staging", 7, 2);
    command("STOR hello\r\nABOR\r\n");
    assert(!data.pcb && record(p)->aborts); file_is(original, 5);
    error(&data, ERR_ABRT); assert(!data.failed); /* invalidated callback state */
    p = passive_connect(); command("STOR hello\r\n");
    deliver(p, "staging", 7, 2); loops(4); fin(p);
    fail_close = 100; service();
    clock_ms += 5000; service(); assert(!data.pcb); fail_close = 0;
    file_is(original, 5);
    p = passive_connect(); deliver(p, "early", 5, 2);
    unsigned outputs = record(p)->outputs;
    ftp_tcp_link_down(); ftp_tcp_link_down();
    assert(record(p)->outputs == outputs && !live_pbufs);
    assert(!tcp_active_pcbs && !tcp_tw_pcbs && !tcp_bound_pcbs);
    file_is(original, 5);
    start();
    owner = control.pcb;
    /* Passive setup allocation/bind/listen failures advertise no port. */
    fail_new = 1; command("EPSV\r\n"); assert(!passive);
    fail_bind = 1; command("EPSV\r\n"); assert(!passive);
    fail_listen = 1; command("EPSV\r\n"); assert(!passive);
    assert(contains(record(owner), "425 "));
    /* FIN and bytes before STOR are retained until complete 150 output. */
    p = passive_connect(); deliver(p, original, 5, 1); fin(p);
    deliver(owner, "STOR hello\r\n", 12, 2);
    fail_output = 2; service(); service();
    file_is(original, 5); assert(data.pcb == p && !record(p)->credits);
    service(); file_is(original, 5); assert(!data.pcb);
    /* Full-size chained upload and partial enqueue/ACK accounting. */
    uint8_t large[4097];
    for (size_t i = 0; i < sizeof large; ++i) large[i] = (uint8_t)i;
    upload(large, 4096);
    p = passive_connect(); command("RETR hello\r\n");
    assert(record(p)->length == 4096 && !record(p)->closes);
    assert(p->sent(p->callback_arg, p, 2144) == ERR_OK); service();
    assert(!record(p)->closes);
    assert(p->sent(p->callback_arg, p, 1952) == ERR_OK); service();
    assert(!data.pcb && !memcmp(record(p)->bytes, large, 4096));
    p = passive_connect(); command("STOR hello\r\n");
    deliver(p, large, sizeof large, 113); loops(20);
    assert(!data.pcb && !live_pbufs); file_is(large, 4096);
    assert(contains(record(owner), "552 "));
    upload("", 0);
    p = passive_connect(); command("RETR hello\r\n"); assert(!data.pcb);
    upload(original, 5);
    p = passive_connect(); command("STOR hello\r\n");
    deliver(p, "bad", 3, 1); fin(control.pcb); service();
    assert(!control.pcb && !data.pcb); file_is(original, 5);
    connect_to(listener, 10, ERR_OK); loops(5);
    owner = control.pcb;
    deliver(owner, "QUIT\r\nNOOP\r\n", 12, 2);
    retry = chain("NOOP\r\n", 6, 2);
    assert(owner->recv(owner->callback_arg, owner, retry, ERR_OK) == ERR_MEM);
    owner->refused_data = retry;
    credits = record(owner)->credits;
    fail_write = 2; service();
    assert(!owner->refused_data && !control.rx && record(owner)->credits == credits + 18);
    deliver(owner, "NOOP\r\n", 6, 2);
    assert(!control.rx && record(owner)->credits == credits + 24);
    service();
    fail_close = 2; service(); assert(control.pcb == owner && owner->recv);
    service(); assert(control.pcb == owner && owner->errf);
    service(); assert(!control.pcb && record(owner)->dead && !record(owner)->aborts);
    ftp_tcp_link_down(); assert(!live_pbufs);
    clock_ms = UINT32_MAX - 10; start(); command("EPSV\r\n");
    clock_ms += 14999; service(); assert(passive);
    ++clock_ms; service(); assert(!passive);
    file_is(original, 5);
    command("NOOP\r\n");
    deliver(control.pcb, "NOOP\r\n", 6, 2);
    fail_write = 100; service();
    clock_ms += 5000; service();
    assert(!control.pcb); fail_write = 0;
    ftp_tcp_link_down(); assert(!live_pbufs && !tcp_active_pcbs);
    /* Ring drained, output still blocked: its independent deadline must
     * abort upload/control at exactly 5 s, not core's 15/60 s deadlines. */
    start(); p = passive_connect(); command("STOR hello\r\n");
    deliver(p, "bad", 3, 1); loops(4);
    deliver(control.pcb, "NOOP\r\n", 6, 2);
    fail_output = 100; service();
    size_t pending; assert(!ftp_core_reply_peek(&pending) && control.output);
    clock_ms += 4999; service(); assert(control.pcb && data.pcb);
    ++clock_ms; service(); assert(!control.pcb && !data.pcb);
    file_is(original, 5); fail_output = 0; ftp_tcp_link_down();
    /* Same deadline for already-enqueued download output, even though the
     * unsent-file core timer no longer applies and no ACK is forthcoming. */
    start(); p = passive_connect();
    deliver(control.pcb, "RETR hello\r\n", 12, 3);
    /* Submit 150, then block data output only. */
    now = clock_ms; input_control(); write_control(); flush_close(&control);
    fail_output = 100; write_data(); flush_close(&data);
    assert(data.output && record(p)->length == 5);
    clock_ms += 4999; service(); assert(data.pcb == p);
    ++clock_ms; service(); assert(!data.pcb && control.pcb);
    fail_output = 0; loops(10); file_is(original, 5);
    ftp_tcp_link_down(); assert(!live_pbufs && !tcp_active_pcbs);
    puts("ftp_tcp_test: all assertions passed (fake raw API, no sockets)");
    return 0;
}
