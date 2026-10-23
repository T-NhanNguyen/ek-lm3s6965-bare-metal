/* HOST model storage is deliberately external: NOT ARM lifetime evidence. */
#include "lm3s6965/boot_startup_producer.h"
#include "lm3s6965/boot_watchdog_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define T2 UINT32_C(0x40032000)
#define G1 UINT32_C(0x400fe104)
static boot_startup_t c;
static uint32_t words[16], gate0, gate1, timer[19], ctl, load, locked;
static unsigned delays, calls, configs, writes, loads, icrs, timer_writes, mode;
static boot_watchdog_t *original;
static const uint32_t hz = 12345679u; /* not a nominal 50MHz macro */
static struct { char op; uint32_t a,v; } trace[256];
static unsigned n;
static void event(char op,uint32_t a,uint32_t v) {
    CHECK(n<256); trace[n].op=op; trace[n].a=a; trace[n++].v=v;
}
static int rd(void *p,uint32_t a,uint32_t *v) {
    CHECK(p==c.context); ++calls;
    if(a==BOOT_WATCHDOG_RCGC0) *v=gate0;
    else {
        CHECK(delays>=1); /* no peripheral access before WDTready */
        if(a==G1) *v=gate1;
        else if(a==0x400fe060u) *v=0x12345678;
        else if(a==0x400fe070u) *v=0x87654321;
        else if(a>=T2 && a<=T2+72 && !(a&3)) {
            CHECK(delays==2); *v=timer[(a-T2)/4];
        } else {
            CHECK(delays==2 && timer[3]==1);
            if(a==BOOT_WATCHDOG_CTL) *v=ctl;
            else if(a==BOOT_WATCHDOG_LOAD) *v=load;
            else if(a==BOOT_WATCHDOG_LOCK) *v=locked;
            else { CHECK(a==BOOT_WATCHDOG_RIS); *v=0; }
        }
    }
    event('R',a,*v); return 0;
}
static int wr(void *p,uint32_t a,uint32_t v) {
    CHECK(p==c.context); ++calls; ++writes; event('W',a,v);
    if(a==BOOT_WATCHDOG_RCGC0) { if(mode!=1) gate0=v; }
    else {
        CHECK(delays>=1);
        if(a==G1) gate1=v;
        else if(a>=T2 && a<=T2+72 && !(a&3)) {
            CHECK(delays==2); ++timer_writes; timer[(a-T2)/4]=v;
        } else {
            CHECK(delays==2 && timer[3]==1);
            if(a==BOOT_WATCHDOG_CTL) ctl=v;
            else if(a==BOOT_WATCHDOG_LOAD) { if(mode!=3) load=v; ++loads; }
            else if(a==BOOT_WATCHDOG_ICR) { CHECK(v==0); ++icrs; }
            else { CHECK(a==BOOT_WATCHDOG_LOCK); locked=(v==0); }
        }
    }
    return 0;
}
static void delay(void *p) {
    CHECK(p==c.context); CHECK(delays<2); ++delays; event('D',0,0);
}
/* Rename ONLY the producer TU's call, forwarding to the real implementation. */
boot_watchdog_result_t producer_observe_configure(boot_watchdog_t *,uint32_t,uint32_t);
boot_watchdog_result_t producer_observe_configure(boot_watchdog_t *w,uint32_t h,uint32_t r) {
    ++configs; CHECK(w==original); CHECK(h==hz && r==hz*5u);
    CHECK(timer[3]==1 && delays==2);
    return boot_watchdog_configure(w,h,r);
}
__attribute__((noinline)) static bool begin(uint32_t h) {
    boot_watchdog_io_t io={c.context,rd,wr};
    bool ok=boot_startup_producer_begin_io(&c,words,0,h,&io,delay);
    for(size_t i=0;i<sizeof io;++i) ((volatile unsigned char *)&io)[i]=0xa5; /* expire descriptor */
    return ok;
}
static void fresh(unsigned fault) {
    memset(&c,0xa5,sizeof c); memset(words,0xa5,sizeof words);
    memset(timer,0,sizeof timer); timer[18]=UINT32_MAX-hz;
    gate0=0x100; gate1=0x80; ctl=load=0; locked=1;
    calls=delays=configs=writes=loads=icrs=timer_writes=n=0;
    mode=fault; original=&c.watchdog;
    if(fault==2) timer[3]=1;
}
static void quarantined(void) {
    CHECK(c.state==BOOT_STARTUP_FAILED && !c.active);
    CHECK(c.watchdog.blocked && !c.watchdog.evidence);
    unsigned before=calls;
    CHECK(boot_watchdog_service(original,BOOT_WATCHDOG_BOOT,1)==BOOT_WATCHDOG_BLOCKED);
    CHECK(calls==before); /* no feed/cleanup/retry by producer */
}
static int op(void *p,boot_handoff_operation_t a,uint32_t v) {
    (void)p;(void)a;(void)v; return 0;
}
static int terminal(void *p,const boot_handoff_intent_t *i) {
    (void)p; CHECK(original==&c.watchdog);
    CHECK(original->owner==BOOT_WATCHDOG_APPLICATION && original->epoch==i->application_epoch);
    CHECK(original->verification==17 && original->phase==2); return 0;
}
static void success(void) {
    fresh(0); CHECK(begin(hz));
    CHECK(n>8 && trace[0].op=='R' && trace[0].a==BOOT_WATCHDOG_RCGC0);
    CHECK(trace[1].op=='W' && trace[1].a==BOOT_WATCHDOG_RCGC0 && trace[1].v==0x108);
    CHECK(trace[2].op=='R' && trace[2].v==0x108 && trace[3].op=='D');
    CHECK(trace[4].op=='R' && trace[4].a==G1 && trace[4].v==0x80);
    CHECK(trace[5].op=='W' && trace[5].a==G1 && trace[5].v==(0x80u|(1u<<18)));
    CHECK(trace[6].op=='R' && trace[6].v==gate1 && trace[7].op=='D');
    CHECK(configs==1 && loads==1 && icrs==1 && timer_writes==6);
    CHECK(load==hz*5u && ctl==3 && c.hz==hz && c.reload==hz*5u);
    CHECK(c.state==BOOT_STARTUP_EMPTY && c.watchdog.epoch==1 && c.watchdog.owner==BOOT_WATCHDOG_BOOT);
    CHECK(c.clock.rcc==0x12345678 && c.clock.rcc2==0x87654321);
    CHECK(c.clock.rcc_mask==UINT32_MAX && c.clock.rcc2_mask==UINT32_MAX);
    CHECK(c.watchdog.io.context==c.context && c.clock.context==c.context);
    CHECK(c.watchdog.io.read_register==rd && c.watchdog.io.write_register==wr);
    boot_startup_gptm_t *g=(void *)c.context;
    CHECK(g->timer==2 && g->registers.context==c.context && g->gate_delay==delay);
    CHECK(g->registers.read_register==rd && g->registers.write_register==wr && g->reader_identity==c.clock.read);
    /* Model verifier milestones, NOT successful cryptographic verification. */
    CHECK(boot_watchdog_boot_report(original,1,17,BOOT_VERIFY_MANIFEST_BEGIN,0,4096)==BOOT_WATCHDOG_OK);
    CHECK(boot_watchdog_boot_report(original,1,17,BOOT_VERIFY_MANIFEST_AUTHENTICATED,0,4096)==BOOT_WATCHDOG_OK);
    boot_handoff_request_t req={0}; boot_handoff_intent_t intent;
    req.image.manifest.execution_base=0x8800; req.image.manifest.payload_length=4096;
    req.image.initial_msp=0x20010000; req.image.reset_vector=0x8901; req.watchdog_epoch=1;
    boot_handoff_t handoff; boot_handoff_io_t io={NULL,op,terminal};
    CHECK(boot_handoff_init(&handoff,&io)==BOOT_HANDOFF_OK);
    unsigned before=calls;
    CHECK(boot_handoff_run(&handoff,original,&req,&intent)==BOOT_HANDOFF_OK);
    CHECK(calls==before && original==&c.watchdog && original->epoch==2);
    CHECK(original->verification==17 && original->phase==2 && !original->evidence);
    CHECK(boot_startup_prepare_handoff(&c,&handoff,BOOT_HANDOFF_OK,&req.image,&intent));
    CHECK(boot_startup_begin_dispatch(&c));
    c.observed=(boot_startup_observations_t){.msp=0x20010000,.vtor=0x8800,.pc=0x8920,.stub=0x8901};
    CHECK(boot_startup_early(&c,0x8901,0x8920)); CHECK(boot_startup_c_ready(&c));
    uint64_t us=0; CHECK(boot_startup_time(&c,&us) && us==1000000);
    CHECK(configs==1 && loads==1 && icrs==1 && original->epoch==2);
    CHECK(original->reload==hz*5u && original->verification==17);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"producer-success")) success();
    else if(!strcmp(argv[1],"producer-overflow")) {
        fresh(0); boot_startup_t saved; uint32_t diag[16];
        memcpy(&saved,&c,sizeof c); memcpy(diag,words,sizeof words);
        CHECK(!begin(UINT32_MAX/5u+1u)); CHECK(!memcmp(&saved,&c,sizeof c));
        CHECK(!memcmp(diag,words,sizeof words) && calls==0 && configs==0);
    } else {
        CHECK(!strcmp(argv[1],"producer-admitted-failure"));
        /* One fresh boot per fault, never retry an admitted capsule. */
        fresh(1); CHECK(!begin(hz)); quarantined();
        CHECK(n==3 && calls==3 && delays==0 && configs==0 && writes==1);
        fresh(2); CHECK(!begin(hz)); quarantined();
        CHECK(n==9 && calls==7 && delays==2 && configs==0 && writes==2);
        CHECK(timer[3]==1 && timer_writes==0 && loads==0 && icrs==0);
        fresh(3); CHECK(!begin(hz)); quarantined();
        CHECK(configs==1 && loads==1 && load==0 && ctl==0 && icrs==0);
        CHECK(trace[n-2].op=='W' && trace[n-2].a==BOOT_WATCHDOG_LOCK && trace[n-2].v==0);
        CHECK(trace[n-1].op=='R' && trace[n-1].a==BOOT_WATCHDOG_LOCK && trace[n-1].v==1);
    }
    return 0;
}
