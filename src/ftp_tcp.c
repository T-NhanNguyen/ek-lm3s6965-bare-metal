/* Serialized foreground raw TCP adapter. No callback state lives on a stack.
 * Receive ownership: accept one whole chain, defer credit until each prefix is
 * consumed, and refuse subsequent chains without touching them. */
#include "lm3s6965/ftp_tcp.h"
#include "lm3s6965/ftp_core.h"
#include "lwip/tcp.h"
#include "lwip/init.h"
#if LWIP_VERSION_MAJOR != 2 || LWIP_VERSION_MINOR != 2 || LWIP_VERSION_REVISION != 1
#error "FTP close/refused-data ownership requires review for this lwIP version"
#endif
#include "lwip/priv/tcp_priv.h" /* tcp_abandon(reset=0) for offline teardown */
#include "lwip/sys.h"
#include <stdbool.h>
#include <string.h>

struct channel {
    struct tcp_pcb *pcb;
    struct pbuf *rx;
    u16_t offset;
    bool fin, failed, output, closing;
    uint32_t output_since;
};
static struct channel control, data;
static struct tcp_pcb *listener, *passive;
static ip_addr_t local;
static uint32_t now;

static err_t receive(void *, struct tcp_pcb *, struct pbuf *, err_t);
static err_t sent(void *, struct tcp_pcb *, u16_t);
static void error(void *, err_t);

static void callbacks(struct channel *c, bool attach)
{
    tcp_arg(c->pcb, attach ? c : NULL);
    tcp_recv(c->pcb, attach ? receive : NULL);
    tcp_sent(c->pcb, attach ? sent : NULL);
    tcp_err(c->pcb, attach ? error : NULL);
}
static void release_rx(struct channel *c)
{
    if (c->rx) pbuf_free(c->rx);
    c->rx = NULL;
    c->offset = 0;
}
static void discard(struct channel *c, bool offline)
{
    struct tcp_pcb *p = c->pcb;
    if (p) callbacks(c, false);
    /* Invalidate BEFORE abort: lwIP may synchronously invoke tcp_err. */
    c->pcb = NULL;
    release_rx(c);
    c->fin = c->failed = c->output = c->closing = false;
    if (p) {
        if (offline) tcp_abandon(p, 0);
        else tcp_abort(p);
    }
}
static void close_listener(struct tcp_pcb **slot)
{
    struct tcp_pcb *p = *slot;
    *slot = NULL;
    if (p) {
        tcp_arg(p, NULL);
        tcp_accept(p, NULL);
        /* lwIP 2.2.1 LISTEN close is unconditional; never tcp_abort LISTEN. */
        (void)tcp_close(p);
    }
}
static err_t receive(void *arg, struct tcp_pcb *p, struct pbuf *b, err_t e)
{
    struct channel *c = arg;
    if (c->pcb != p) return ERR_MEM;
    if (b && c == &control && ftp_core_quitting()) {
        tcp_recved(p, b->tot_len);
        pbuf_free(b);
        return ERR_OK;
    }
    if (b && c->rx) return ERR_MEM; /* no free/credit/parser/timer effects */
    if (b) c->rx = b;
    else c->fin = true;
    if (e != ERR_OK) c->failed = true;
    return ERR_OK;
}
static err_t sent(void *arg, struct tcp_pcb *p, u16_t n)
{
    struct channel *c = arg;
    if (c->pcb == p && c == &data &&
        !ftp_core_download_acked(n, sys_now())) c->failed = true;
    return ERR_OK;
}
static void error(void *arg, err_t e)
{
    struct channel *c = arg;
    (void)e;
    if (!c || !c->pcb) return;
    /* tcp_err owns no live PCB. Do not detach, abort, or dereference it. */
    c->pcb = NULL;
    release_rx(c);
    c->output = c->closing = c->fin = false;
    c->failed = true;
}
static err_t accept_control(void *arg, struct tcp_pcb *p, err_t e)
{
    (void)arg;
    if (e != ERR_OK || control.pcb || control.failed ||
        !ftp_core_session_open(sys_now())) {
        tcp_abort(p);
        return ERR_ABRT;
    }
    memset(&control, 0, sizeof control);
    control.pcb = p;
    callbacks(&control, true);
    return ERR_OK;
}
static err_t accept_data(void *arg, struct tcp_pcb *p, err_t e)
{
    (void)arg;
    if (e != ERR_OK || !control.pcb || data.pcb || data.failed ||
        !ip_addr_cmp(&p->remote_ip, &control.pcb->remote_ip) ||
        !ftp_core_data_connected(sys_now())) {
        tcp_abort(p);
        return ERR_ABRT;
    }
    memset(&data, 0, sizeof data);
    data.pcb = p;
    callbacks(&data, true);
    /* Prevent a second accept before the next foreground service. */
    close_listener(&passive);
    return ERR_OK;
}
static err_t make_listener(u16_t port, tcp_accept_fn accept,
                           struct tcp_pcb **result)
{
    struct tcp_pcb *p = tcp_new_ip_type(IPADDR_TYPE_V4), *l;
    err_t e;
    if (!p) return ERR_MEM;
    e = tcp_bind(p, &local, port);
    if (e != ERR_OK) { tcp_abort(p); return e; }
    l = tcp_listen_with_backlog_and_err(p, 1, &e);
    if (!l) { tcp_abort(p); return e; }
    tcp_arg(l, NULL);
    tcp_accept(l, accept);
    *result = l;
    return ERR_OK;
}
err_t ftp_tcp_start(const ip4_addr_t *address)
{
    if (!address) return ERR_ARG;
    if (!ftp_core_is_initialized()) return ERR_VAL;
    if (listener) return ERR_OK;
    ip_addr_copy_from_ip4(local, *address);
    return make_listener(21, accept_control, &listener);
}
void ftp_tcp_link_down(void)
{
    close_listener(&passive);
    close_listener(&listener);
    discard(&data, true);
    discard(&control, true);
    /* This dedicated NO_SYS image has no other TCP users. Successful close
     * transfers ownership to lwIP (including FIN retransmission). Remove
     * those residual PCBs too, via stack-owned lists, not stale app pointers.
     * No resets or pending FIN/data may escape into the next ready epoch. */
    while (tcp_active_pcbs) tcp_abandon(tcp_active_pcbs, 0);
    while (tcp_tw_pcbs) tcp_abandon(tcp_tw_pcbs, 0);
    while (tcp_bound_pcbs) tcp_abandon(tcp_bound_pcbs, 0);
    ftp_core_session_lost();
    (void)ftp_core_take_actions();
}
static void credit(struct channel *, u16_t);
static void actions(void)
{
    unsigned a = ftp_core_take_actions();
    if (a & FTP_CLOSE_PASSIVE) close_listener(&passive);
    if (a & FTP_ABORT_DATA) discard(&data, false);
    if (a & FTP_ABORT_CONTROL) discard(&control, false);
    if (a & FTP_CLOSE_DATA) data.closing = true;
    if (a & FTP_CLOSE_CONTROL) {
        control.closing = true;
        /* QUIT stops parsing; credit/discard trailing accepted transport input. */
        if (control.pcb && control.rx)
            credit(&control, (u16_t)(control.rx->tot_len - control.offset));
    }
    if (a & FTP_OPEN_PASSIVE) {
        const ip4_addr_t *ip = ip_2_ip4(&local);
        uint8_t octets[4] = { ip4_addr1(ip), ip4_addr2(ip),
                              ip4_addr3(ip), ip4_addr4(ip) };
        bool ok = make_listener(0, accept_data, &passive) == ERR_OK;
        if (!ftp_core_passive_result(ok, octets,
                                     ok ? passive->local_port : 0, now))
            close_listener(&passive);
    }
}
static void credit(struct channel *c, u16_t n)
{
    c->offset = (u16_t)(c->offset + n);
    tcp_recved(c->pcb, n);
    if (c->offset == c->rx->tot_len) release_rx(c);
}
static void input_control(void)
{
    unsigned budget = FTP_CONTROL_CAPACITY;
    /* One-byte preflights also handle arbitrarily many lines in one chain.
     * The cursor advances only on OK, including across reply backpressure. */
    while (control.pcb && control.rx && budget--) {
        uint8_t b;
        if (ftp_core_quitting()) {
            credit(&control, (u16_t)(control.rx->tot_len - control.offset));
            break;
        }
        if (pbuf_copy_partial(control.rx, &b, 1, control.offset) != 1) {
            control.failed = true;
            break;
        }
        size_t accepted;
        if (ftp_core_control_span(&b, 1, now, &accepted) != FTP_INPUT_OK) break;
        credit(&control, 1);
        actions();
    }
    /* Serialized foreground, outside tcp_input. The pinned private helper
     * reoffers refused_data through receive(), which credits/discards QUIT
     * tails. Do this before close, not just on the one-shot close action. */
    if (control.pcb && ftp_core_quitting() && control.pcb->refused_data)
        (void)tcp_process_refused_data(control.pcb);
}
static void input_data(void)
{
    uint8_t bytes[256];
    if (!data.pcb || control.output || !ftp_core_transfer_ready()) return;
    if (data.rx) {
        u16_t n = (u16_t)(data.rx->tot_len - data.offset);
        if (n > sizeof bytes) n = sizeof bytes;
        if (ftp_core_direction() != FTP_UPLOAD) {
            data.failed = true;
            return;
        }
        if (pbuf_copy_partial(data.rx, bytes, n, data.offset) != n) {
            data.failed = true;
            return;
        }
        if (ftp_core_upload_chunk(bytes, n, now)) credit(&data, n);
        actions();
    }
    if (data.pcb && data.fin && !data.rx && !data.closing) {
        if (ftp_core_direction() == FTP_UPLOAD) {
            (void)ftp_core_upload_eof(now);
            actions();
        } else {
            /* A download peer may half-close its unused transmit direction.
             * Its ACKs must still all arrive before we send our FIN. */
            data.fin = false;
        }
    }
}
static void write_control(void)
{
    size_t n;
    const uint8_t *b;
    if (!control.pcb || control.failed) return;
    b = ftp_core_reply_peek(&n);
    if (b) {
        if (n > tcp_sndbuf(control.pcb)) n = tcp_sndbuf(control.pcb);
        if (n && tcp_write(control.pcb, b, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            (void)ftp_core_reply_consume(n, now);
            if (!control.output) control.output_since = now;
            control.output = true;
        }
    }
    actions();
}
static void write_data(void)
{
    size_t n;
    const uint8_t *b;
    if (!data.pcb || data.failed || control.output || ftp_core_direction() != FTP_DOWNLOAD ||
        !ftp_core_transfer_ready() || data.closing) return;
    b = ftp_core_download_span(&n);
    if (!n) {
        (void)ftp_core_download_enqueued(0, now);
    } else if (b) {
        if (n > tcp_sndbuf(data.pcb)) n = tcp_sndbuf(data.pcb);
        if (n && tcp_write(data.pcb, b, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            (void)ftp_core_download_enqueued(n, now);
            if (!data.output) data.output_since = now;
            data.output = true;
        }
    }
    actions();
}
static void flush_close(struct channel *c)
{
    struct tcp_pcb *p;
    err_t e;
    if (!c->pcb || c->failed) return;
    if (c->output) {
        if (tcp_output(c->pcb) != ERR_OK) return; /* retry output, not write */
        c->output = false;
    }
    if (!c->closing) return;
    /* Never graceful-close with uncredited receive bytes (lwIP sends RST). */
    if (c->rx) return;
    p = c->pcb;
    /* Pinned lwIP 2.2.1: TX-only shutdown retains application ownership,
     * but tcp_close_shutdown_fin masks ERR_MEM as ERR_OK + TF_CLOSEPEND.
     * Clear that private flag on failure: no timer may enqueue FIN behind
     * our deadline/error checks. Never infer success from tcp_close alone.
     * TX-only avoids the unread-data RST path; after success the state is
     * FIN_WAIT_1/LAST_ACK, where tcp_close safely releases ownership. */
    e = tcp_shutdown(p, 0, 1);
    if (e == ERR_OK && (p->flags & TF_CLOSEPEND)) {
        tcp_clear_flags(p, TF_CLOSEPEND);
        e = ERR_MEM;
    }
    if (e == ERR_OK) {
        callbacks(c, false);
        (void)tcp_close(p); /* Already in a FIN state: no allocation or RST. */
        c->pcb = NULL;
        c->fin = c->closing = false;
        if (c == &data) (void)ftp_core_data_close_result(true, now);
        else ftp_core_control_closed();
    } else {
        if (e != ERR_MEM) c->failed = true;
        /* Core's original pending-close deadline bounds ERR_MEM retries. */
    }
}
void ftp_tcp_service(uint32_t time)
{
    now = time;
    /* Independent of core reply lifetime: tcp_write may drain the ring while
     * tcp_output still cannot submit anything. Enqueues do not reset this. */
    if (control.output && (uint32_t)(now - control.output_since) >= 5000u)
        control.failed = true;
    if (data.output && (uint32_t)(now - data.output_since) >= 5000u)
        data.failed = true;
    if (control.failed || (control.pcb && control.fin)) {
        ftp_core_session_lost();
        discard(&control, false);
        discard(&data, false);
        close_listener(&passive);
    } else if (data.failed) {
        discard(&data, false);
        ftp_core_data_error(now);
    }
    ftp_core_tick(now);
    actions();
    input_control();
    write_control();
    flush_close(&control); /* submit 150 before enabling any data work */
    input_data();
    write_data();
    flush_close(&data);
    actions();
}
