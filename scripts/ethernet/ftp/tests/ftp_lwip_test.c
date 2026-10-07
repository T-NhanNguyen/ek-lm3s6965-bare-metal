/* Memory Ethernet peer, not a fake TCP API. All receive traffic traverses
 * ethernet_input -> IPv4/TCP checksums -> real lwIP callbacks. No host I/O. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/stats.h"
#include "lwip/priv/tcp_priv.h"
#include "netif/ethernet.h"
#include "lm3s6965/ftp_tcp.h"
#include "lm3s6965/ftp_core.h"
#include "lm3s6965/ram_file.h"
static ram_file_t fixture;
static uint8_t fixture_a[4096], fixture_b[4096];

static uint32_t clock_ms;
u32_t sys_now(void) { return clock_ms; }
static struct netif nic;
static const uint8_t server_mac[6] = {2,0,0,0x69,0x65,2};
static const uint8_t peer_mac[6] = {2,0,0,0,0,1};
static const uint8_t peer_ip[4] = {192,168,7,1}, server_ip[4] = {192,168,7,2};
struct peer {
    uint16_t port, remote;
    uint32_t tx, rx;
    bool syn, pending, ack, fin;
    uint8_t bytes[32768];
    size_t length;
};
static struct peer ctrl, data;
static unsigned duplicates, arp_replies, busy_calls, resets;
static bool busy;
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p)<<16)|get16(p+2); }
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p,(uint16_t)(v>>16)); put16(p+2,(uint16_t)v); }
static uint32_t sum(const uint8_t *p, size_t n, uint32_t s)
{
    while (n>=2) { s+=get16(p); p+=2; n-=2; }
    if (n) s+=(uint16_t)(p[0]<<8);
    return s;
}
static uint16_t checksum(uint32_t s)
{
    while (s>>16) s=(s&65535)+(s>>16);
    return (uint16_t)~s;
}
static uint32_t pseudo(const uint8_t *ip, size_t n)
{ return sum(ip+12,8,6+(uint32_t)n); }
static void inject(const uint8_t *f, size_t n)
{
    struct pbuf *p=pbuf_alloc(PBUF_RAW,(u16_t)n,PBUF_POOL);
    assert(p && pbuf_take(p,f,(u16_t)n)==ERR_OK);
    assert(ethernet_input(p,&nic)==ERR_OK);
}
static err_t output(struct netif *n, struct pbuf *p)
{
    uint8_t f[1514]; (void)n;
    if (busy) { ++busy_calls; return ERR_MEM; }
    assert(p->tot_len<=sizeof f);
    assert(pbuf_copy_partial(p,f,p->tot_len,0)==p->tot_len);
    if (get16(f+12)==0x0806) { ++arp_replies; return ERR_OK; }
    assert(get16(f+12)==0x0800);
    const uint8_t *ip=f+14;
    size_t ih=(ip[0]&15)*4u, total=get16(ip+2);
    assert(total+14<=p->tot_len && checksum(sum(ip,ih,0))==0);
    assert(ip[9]==6);
    const uint8_t *t=ip+ih;
    size_t tn=total-ih, th=(t[12]>>4)*4u;
    assert(tn>=th && checksum(sum(t,tn,pseudo(ip,tn)))==0);
    struct peer *c=get16(t+2)==ctrl.port ? &ctrl : &data;
    assert(get16(t+2)==c->port && get16(t)==c->remote);
    if (t[13]&TCP_RST) { ++resets; return ERR_OK; }
    uint32_t seq=get32(t+4);
    if (t[13]&TCP_SYN) { c->rx=seq+1; c->syn=true; c->pending=true; return ERR_OK; }
    size_t len=tn-th;
    if (seq==c->rx) {
        assert(c->length+len<sizeof c->bytes);
        memcpy(c->bytes+c->length,t+th,len); c->length+=len;
        c->bytes[c->length]=0; c->rx+=(uint32_t)len;
        if (t[13]&TCP_FIN) { ++c->rx; c->fin=true; }
    } else if (len) { ++duplicates; }
    if (len || (t[13]&TCP_FIN)) c->pending=true;
    return ERR_OK;
}
static err_t setup(struct netif *n)
{
    n->name[0]='m'; n->name[1]='e'; n->mtu=1500;
    n->hwaddr_len=6; memcpy(n->hwaddr,server_mac,6);
    n->flags=NETIF_FLAG_BROADCAST|NETIF_FLAG_ETHARP;
    n->output=etharp_output; n->linkoutput=output; return ERR_OK;
}
static void packet(struct peer *c, unsigned flags, const void *bytes, size_t len)
{
    uint8_t f[1514]={0}; assert(len<=1400);
    memcpy(f,server_mac,6); memcpy(f+6,peer_mac,6); put16(f+12,0x0800);
    uint8_t *ip=f+14, *t=ip+20;
    ip[0]=0x45; put16(ip+2,(uint16_t)(40+len)); ip[8]=64; ip[9]=6;
    memcpy(ip+12,peer_ip,4); memcpy(ip+16,server_ip,4);
    put16(ip+10,checksum(sum(ip,20,0)));
    put16(t,c->port); put16(t+2,c->remote); put32(t+4,c->tx); put32(t+8,c->rx);
    t[12]=0x50; t[13]=(uint8_t)flags; put16(t+14,2144);
    if (len) memcpy(t+20,bytes,len);
    put16(t+16,checksum(sum(t,20+len,pseudo(ip,20+len))));
    c->tx+=(uint32_t)len+!!(flags&TCP_SYN)+!!(flags&TCP_FIN);
    inject(f,54+len);
}
static void pump(unsigned count)
{
    while (count--) {
        ftp_tcp_service(clock_ms);
        struct peer *cs[]={&ctrl,&data};
        for (unsigned i=0;i<2;++i) if (cs[i]->pending && cs[i]->ack) {
            cs[i]->pending=false; packet(cs[i],TCP_ACK,NULL,0);
        }
    }
}
static void advance(unsigned ms)
{
    while (ms) { unsigned d=ms>250?250:ms; clock_ms+=d; ms-=d; sys_check_timeouts(); pump(2); }
}
static void arp(void)
{
    uint8_t f[60]={0}; memset(f,255,6); memcpy(f+6,peer_mac,6); put16(f+12,0x0806);
    uint8_t *a=f+14; put16(a,1); put16(a+2,0x0800); a[4]=6; a[5]=4; put16(a+6,1);
    memcpy(a+8,peer_mac,6); memcpy(a+14,peer_ip,4); memcpy(a+24,server_ip,4);
    inject(f,sizeof f); assert(arp_replies);
}
static void connect_peer(struct peer *c, uint16_t port, uint16_t remote)
{
    memset(c,0,sizeof *c); c->port=port; c->remote=remote; c->tx=1000+port; c->ack=true;
    packet(c,TCP_SYN,NULL,0); assert(c->syn);
    c->pending=false; packet(c,TCP_ACK,NULL,0); pump(12);
}
static void clear_control(void) { ctrl.length=0; ctrl.bytes[0]=0; }
static void expect(const char *text) { assert(strstr((char *)ctrl.bytes,text)); }
static void command(const char *s) { packet(&ctrl,TCP_ACK|TCP_PSH,s,strlen(s)); pump(32); }
static void login(uint16_t port)
{
    connect_peer(&ctrl,port,21); expect("220 "); clear_control();
    command("US"); assert(ctrl.length==0);
    command("ER anonymous\r"); assert(ctrl.length==0);
    command("\nPASS offline\r\nTYPE I\r\n");
    expect("331 "); expect("230 "); expect("200 Binary"); clear_control();
}
static void passive(bool epsv)
{
    clear_control(); command(epsv?"EPSV\r\n":"PASV\r\n");
    unsigned port=0,a,b,c,d,hi,lo;
    if (epsv) assert(sscanf((char *)ctrl.bytes,"229 Entering Extended Passive Mode (|||%u|",&port)==1);
    else {
        assert(sscanf((char *)ctrl.bytes,"227 Entering Passive Mode (%u,%u,%u,%u,%u,%u",&a,&b,&c,&d,&hi,&lo)==6);
        assert(a==192 && b==168 && c==7 && d==2); port=hi*256+lo;
    }
    assert(port>0 && port<=65535);
    connect_peer(&data,(uint16_t)(30000+port%10000),(uint16_t)port); clear_control();
}
static void upload(const uint8_t *bytes, size_t n, bool epsv, bool finish)
{
    passive(epsv); command("STOR hello\r\n"); expect("150 "); clear_control();
    for (size_t i=0;i<n;) {
        size_t k=n-i>512?512:n-i; packet(&data,TCP_ACK|TCP_PSH,bytes+i,k); pump(16); i+=k;
    }
    if (finish) { packet(&data,TCP_ACK|TCP_FIN,NULL,0); pump(32); expect("226 Transfer complete.\r\n"); assert(data.fin); }
}
static void download(const uint8_t *bytes, size_t n, bool epsv, bool lag)
{
    passive(epsv); data.ack=!lag; command("RETR hello\r\n"); expect("150 ");
    if (lag && n) {
        assert(data.length>0 && data.length<=TCP_SND_BUF); assert(!data.fin);
        assert(!strstr((char *)ctrl.bytes,"226 "));
        unsigned before=duplicates; advance(3250);
        assert(duplicates>before); assert(!strstr((char *)ctrl.bytes,"226 "));
        /* ACK prefixes to open the send window, then withhold the final ACK.
         * Even when every byte has reached this peer, 226/FIN must wait. */
        for (unsigned i=0;data.length<n && i<20;++i) {
            data.pending=false; packet(&data,TCP_ACK,NULL,0); pump(8);
        }
        assert(data.length==n && !data.fin);
        assert(!strstr((char *)ctrl.bytes,"226 "));
        data.pending=true; data.ack=true;
    }
    pump(100); expect("226 Transfer complete.\r\n");
    assert(data.length==n && memcmp(data.bytes,bytes,n)==0 && data.fin);
    packet(&data,TCP_ACK|TCP_FIN,NULL,0); pump(4);
}
static struct tcp_pcb *peer_pcb(const struct peer *c)
{
    for (struct tcp_pcb *p=tcp_active_pcbs;p;p=p->next)
        if (p->remote_port==c->port && p->local_port==c->remote) return p;
    assert(0); return NULL;
}
static void file_is(const uint8_t *bytes, size_t n)
{
    size_t length; bool exists;
    const uint8_t *f=ftp_core_file(&length,&exists);
    assert(exists && length==n && !memcmp(f,bytes,n));
}
static void fin_starvation(const uint8_t *bytes, unsigned outcome)
{
    upload(bytes,123,true,false); clear_control();
    struct tcp_pcb *p=peer_pcb(&data);
    packet(&data,TCP_ACK|TCP_FIN,NULL,0);
    void *held[MEMP_NUM_TCP_SEG]; unsigned n=0;
    while (n<MEMP_NUM_TCP_SEG && (held[n]=memp_malloc(MEMP_TCP_SEG))) ++n;
    assert(memp_malloc(MEMP_TCP_SEG)==NULL);
    pump(4);
    assert(p->state==CLOSE_WAIT && !(p->flags & TF_CLOSEPEND));
    assert(!data.fin && !strstr((char *)ctrl.bytes,"226 "));
    file_is(bytes,4096);
    if (outcome==0) {
        packet(&data,TCP_RST|TCP_ACK,NULL,0); pump(4);
        file_is(bytes,4096);
    } else if (outcome==1) {
        advance(4999); file_is(bytes,4096);
        assert(peer_pcb(&data)==p && !data.fin);
        advance(1); file_is(bytes,4096);
        assert(!strstr((char *)ctrl.bytes,"226 "));
    }
    for (unsigned i=0;i<n;++i) memp_free(MEMP_TCP_SEG,held[i]);
    pump(32);
    if (outcome==2) {
        expect("226 "); assert(data.fin); file_is(bytes,123);
        packet(&data,TCP_ACK,NULL,0); pump(4);
        upload(bytes,4096,true,true);
    } else expect("426 ");
}
static void pools_empty(void)
{
    assert(!tcp_active_pcbs && !tcp_tw_pcbs && !tcp_bound_pcbs && !tcp_listen_pcbs.listen_pcbs);
    assert(lwip_stats.mem.used==0);
    const unsigned pools[]={MEMP_PBUF_POOL,MEMP_PBUF,MEMP_TCP_PCB,MEMP_TCP_PCB_LISTEN,MEMP_TCP_SEG};
    for (unsigned i=0;i<sizeof pools/sizeof pools[0];++i) assert(lwip_stats.memp[pools[i]]->used==0);
}
int main(void)
{
    assert(LWIP_VERSION_MAJOR==2 && LWIP_VERSION_MINOR==2 && LWIP_VERSION_REVISION==1);
    lwip_init(); ip4_addr_t ip,mask,gw;
    IP4_ADDR(&ip,192,168,7,2); IP4_ADDR(&mask,255,255,255,0); ip4_addr_set_zero(&gw);
    assert(netif_add(&nic,&ip,&mask,&gw,NULL,setup,ethernet_input));
    netif_set_default(&nic); netif_set_up(&nic); netif_set_link_up(&nic); arp();
    assert(!ftp_core_is_initialized());
    assert(ftp_tcp_start(&ip)==ERR_VAL);
    assert(!tcp_listen_pcbs.listen_pcbs && !tcp_active_pcbs);
    assert(ram_file_init(&fixture,fixture_a,fixture_b,4096u,"hello"));
    assert(ftp_core_init(ram_file_storage(&fixture)));
    ftp_core_reset();
    assert(ftp_tcp_start(&ip)==ERR_OK); login(20001);
    uint8_t bytes[4097]; for (size_t i=0;i<sizeof bytes;++i) bytes[i]=(uint8_t)(i*37);
    const size_t lengths[]={0,17,4096};
    for (unsigned i=0;i<3;++i) {
        upload(bytes,lengths[i],i!=1,true);
        download(bytes,lengths[i],i==1,lengths[i]!=0);
    }
    /* Real FIN allocation exhaustion: tcp_close would mask ERR_MEM as OK.
     * Verify retained ownership, reset/5-s rollback, and recovery/commit. */
    fin_starvation(bytes,0); fin_starvation(bytes,1); fin_starvation(bytes,2);
    /* Failed staging must never replace the complete 4096-byte file. */
    upload(bytes,123,true,false); packet(&data,TCP_RST|TCP_ACK,NULL,0); pump(32); expect("426 ");
    download(bytes,4096,false,false);
    upload(bytes,4097,false,false); expect("552 "); download(bytes,4096,true,false);
    /* Reply-ring and retained pbuf backpressure with many coalesced commands. */
    char commands[600]; for (unsigned i=0;i<100;++i) memcpy(commands+i*6,"NOOP\r\n",6);
    clear_control(); busy=true; packet(&ctrl,TCP_ACK|TCP_PSH,commands,sizeof commands); pump(20);
    assert(busy_calls && ctrl.length==0); busy=false; pump(250);
    assert(ctrl.length==100*strlen("200 OK.\r\n"));
    for (unsigned i=0;i<100;++i) assert(memcmp(ctrl.bytes+i*9,"200 OK.\r\n",9)==0);
    /* QUIT owns one pbuf while lwIP refuses the following chain. Write and
     * output pressure retain both; later trailing packets are also drained. */
    clear_control(); unsigned resets_before=resets;
    packet(&ctrl,TCP_ACK|TCP_PSH,"QUIT\r\nNOOP\r\n",12);
    packet(&ctrl,TCP_ACK|TCP_PSH,"NOOP\r\n",6);
    struct tcp_pcb *cp=peer_pcb(&ctrl);
    assert(cp->refused_data);
    void *held[MEMP_NUM_TCP_SEG]; unsigned hn=0;
    while (hn<MEMP_NUM_TCP_SEG && (held[hn]=memp_malloc(MEMP_TCP_SEG))) ++hn;
    busy=true; pump(4);
    assert(!cp->refused_data && cp->rcv_wnd==TCP_WND && ctrl.length==0);
    packet(&ctrl,TCP_ACK|TCP_PSH,"NOOP\r\n",6);
    pump(4); assert(!cp->refused_data && cp->rcv_wnd==TCP_WND);
    assert(!ctrl.fin && ctrl.length==0);
    for (unsigned i=0;i<hn;++i) memp_free(MEMP_TCP_SEG,held[i]);
    pump(4); assert(!ctrl.fin && ctrl.length==0);
    busy=false; pump(32);
    assert(ctrl.fin && resets==resets_before && ctrl.length==strlen("221 Goodbye.\r\n"));
    expect("221 Goodbye.\r\n");
    packet(&ctrl,TCP_ACK|TCP_FIN,NULL,0); pump(8);
    login(20002); download(bytes,4096,true,false);
    upload(bytes,100,true,false); unsigned before=duplicates;
    netif_set_link_down(&nic); netif_set_down(&nic); ftp_tcp_link_down(); etharp_cleanup_netif(&nic);
    pools_empty(); advance(1000); assert(duplicates==before); pools_empty();
    netif_set_up(&nic); netif_set_link_up(&nic); arp(); assert(ftp_tcp_start(&ip)==ERR_OK);
    login(20003); download(bytes,4096,false,false);
    /* Ring lifetime is over after tcp_write, but failed tcp_output is still
     * bounded. QUIT must not wait indefinitely under linkoutput pressure. */
    clear_control(); busy=true; packet(&ctrl,TCP_ACK|TCP_PSH,"QUIT\r\n",6); pump(4);
    size_t pending; assert(!ftp_core_reply_peek(&pending));
    advance(4999); assert(peer_pcb(&ctrl));
    advance(1); busy=false;
    for (struct tcp_pcb *p=tcp_active_pcbs;p;p=p->next) assert(p->remote_port!=ctrl.port);
    assert(!ctrl.fin && ctrl.length==0);
    ftp_tcp_link_down(); etharp_cleanup_netif(&nic); pools_empty();
    puts("FTP real lwIP: ARP/checksums, fragmented/coalesced login, EPSV/PASV, binary 0/17/4096, RST/overflow preservation, ACK lag/retransmission, output/reply backpressure, reconnect/link teardown/pools passed");
    return 0;
}
