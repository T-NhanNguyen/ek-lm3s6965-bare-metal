/* HOST model storage is deliberately external: NOT ARM lifetime evidence. */
#include "lm3s6965/boot_sequence.h"
#include "flash_model.h"
#include "verify_fixtures.h"
#include "lm3s6965/boot_watchdog_target.h"
#include "lm3s6965/boot_handoff_target.h"
#include "../../src/boot_watchdog_internal.h"
#include <stddef.h>
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
static struct { char op; uint32_t a,v; } trace[4096];
static unsigned n;
static void boundary(void);
static void checked_write_boundary(void);
static unsigned resc_reads;
static unsigned runtime_calls, feeds;
static struct { uint32_t id, epoch, completed; } history[32];
static void event(char op,uint32_t a,uint32_t v) {
    CHECK(n<4096); trace[n].op=op; trace[n].a=a; trace[n++].v=v;
}
static int rd(void *p,uint32_t a,uint32_t *v) {
    CHECK(p==c.context); boundary(); if(calls==0) CHECK(resc_reads==1); ++calls;
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
    CHECK(p==c.context); boundary(); checked_write_boundary(); if(calls==0) CHECK(resc_reads==1); ++calls; ++writes; event('W',a,v);
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
            else if(a==BOOT_WATCHDOG_ICR) {
                CHECK(v==0); ++icrs;
                if (ctl==3) {
                    CHECK(feeds<32);
                    history[feeds].id=c.watchdog.verification;
                    history[feeds].epoch=c.watchdog.epoch;
                    history[feeds++].completed=c.watchdog.completed;
                }
            }
            else { CHECK(a==BOOT_WATCHDOG_LOCK); locked=(v==0); }
        }
    }
    return 0;
}
static void delay(void *p) {
    CHECK(p==c.context); boundary(); CHECK(delays<2); ++delays; event('D',0,0);
}

/* All aggregate storage is static, reset only for a distinct modeled boot.
 * No retained HOST lifetime, physical countdown, or stack-size claim. */
static boot_sequence_t seq;
static boot_sequence_parameters_t params;
static boot_watchdog_io_t producer;
static boot_reset_register_adapter_t registers;
static boot_store_adapter_t store, inner;
static boot_select_images_t images;
static boot_launch_decision_t out, sentinel;
static flash_model_t flash;
static boot_metadata_t authority;
static uint8_t payload[BOOT_PAYLOAD_MIN];
static uint32_t resc[3], last_clear;
static unsigned resc_writes, read_fault, write_fault, payload_reads;
static unsigned guard_checks, check_guards, inside, serial, capture_order, store_order;
static unsigned token;
static void checked_boundary(void);
static int checked_is_launching(void);
static void checked_reset(void);
static boot_sequence_result_t invoke(void) { return boot_sequence_run(&seq,&params,&out); }
static void boundary(void) {
    checked_boundary();
    CHECK(!memcmp(&out,&sentinel,sizeof out));
    CHECK(seq.consumed==1);
    if (check_guards && !inside && !checked_is_launching()) {
        inside=1;
        unsigned a=calls,b=resc_reads,d=resc_writes,e=payload_reads;
        size_t f=flash.calls;
        boot_sequence_result_t r=invoke();
        CHECK(r.status==BOOT_SEQUENCE_BUSY && r.stage==BOOT_SEQUENCE_NONE);
        CHECK(calls==a && resc_reads==b && resc_writes==d && payload_reads==e && flash.calls==f);
        ++guard_checks; inside=0;
    }
}
static int reset_read(void *p,uint32_t *v) {
    CHECK(p==&token); boundary(); checked_reset(); ++serial; ++resc_reads;
    CHECK(resc_reads<=3);
    if(resc_reads==1) CHECK(calls==0 && configs==0 && flash.calls==0);
    if(resc_reads==2) { CHECK(configs==1 && runtime_calls==1); capture_order=serial; }
    *v=resc[resc_reads-1];
    return resc_reads==read_fault ? -1:0;
}
static int reset_write(void *p,uint32_t v) {
    CHECK(p==&token); boundary(); ++serial; ++resc_writes;
    CHECK(resc_reads==3 && flash.calls==0); last_clear=v;
    return write_fault ? -1:0; /* May already have effected, never replay. */
}
static int metadata_read(void *p,uint32_t a,uint8_t *b,size_t z) {
    CHECK(p==&token); boundary(); ++serial;
    if(!store_order) store_order=serial;
    CHECK(capture_order && store_order>capture_order);
    return inner.read(inner.context,a,b,z);
}
static int metadata_erase(void *p,uint32_t a) { (void)p;(void)a; CHECK(0); return -1; }
static int metadata_program(void *p,uint32_t a,uint32_t v) { (void)p;(void)a;(void)v; CHECK(0); return -1; }
static size_t payload_read(void *p,uint32_t a,uint8_t *b,size_t z) {
    CHECK(p==&token); boundary(); ++serial; ++payload_reads;
    CHECK(a==BOOT_SLOT_A_BASE && z==sizeof payload);
    CHECK(&c.watchdog==original && c.watchdog.verification==1 && c.watchdog.phase==2);
    CHECK(c.watchdog.epoch==2 && store_order>capture_order);
    memcpy(b,payload,z); return z;
}
/* Observation-only TU renames forward exactly once to unmodified production.
 * No synthetic verifier reports, watchdog initialization or policy results. */
boot_watchdog_result_t sequence_observe_configure(boot_watchdog_t *,uint32_t,uint32_t);
boot_watchdog_result_t sequence_observe_configure(boot_watchdog_t *w,uint32_t h,uint32_t r) {
    CHECK(w==original && w==&c.watchdog && h==hz && r==5u*hz);
    CHECK(resc_reads==1 && delays==2 && timer[3]==1);
    ++configs; return boot_watchdog_configure(w,h,r);
}
boot_runtime_watchdog_result_t sequence_observe_runtime(boot_runtime_watchdog_t *,boot_watchdog_t *,
    const boot_reset_register_adapter_t *,const boot_store_adapter_t *,const boot_select_images_t *,
    boot_verify_read_fn,void *,boot_launch_decision_t *);
boot_runtime_watchdog_result_t sequence_observe_runtime(boot_runtime_watchdog_t *b,boot_watchdog_t *w,
    const boot_reset_register_adapter_t *r,const boot_store_adapter_t *s,const boot_select_images_t *i,
    boot_verify_read_fn read,void *ctx,boot_launch_decision_t *o) {
    CHECK(b==&seq.bridge && w==original && w==&c.watchdog && o==&out);
    CHECK(configs==1 && w->epoch==1 && w->verification==0 && feeds==0);
    CHECK(load==5u*hz && resc_reads==1 && flash.calls==0);
    ++runtime_calls;
    return boot_runtime_watchdog_run(b,w,r,s,i,read,ctx,o);
}
static void put32(uint8_t *p,uint32_t v) { for(unsigned j=0;j<4;++j) p[j]=(uint8_t)(v>>(8*j)); }
static void fresh(void) {
    memset(&c,0xa5,sizeof c); memset(words,0xa5,sizeof words);
    memset(timer,0,sizeof timer); timer[18]=UINT32_MAX-hz;
    gate0=0x100; gate1=0x80; ctl=load=0; locked=1;
    calls=delays=configs=writes=loads=icrs=timer_writes=n=mode=0;
    runtime_calls=feeds=resc_reads=resc_writes=payload_reads=read_fault=write_fault=0;
    check_guards=guard_checks=inside=serial=capture_order=store_order=0;
    original=&c.watchdog;
    boot_sequence_init(&seq);
    flash_model_init(&flash); inner=flash_model_adapter(&flash);
    store=(boot_store_adapter_t){&token,metadata_read,metadata_erase,metadata_program};
    registers=(boot_reset_register_adapter_t){&token,reset_read,reset_write};
    producer=(boot_watchdog_io_t){c.context,rd,wr};
    memset(&images,0,sizeof images); memcpy(images.wire[0],fixture_manifest[0],BOOT_MANIFEST_BYTES);
    images.size[0]=BOOT_MANIFEST_BYTES;
    boot_manifest_t m;
    CHECK(boot_manifest_decode(images.wire[0],images.size[0],&m)==BOOT_OK);
    CHECK(m.payload_length==sizeof payload && m.slot==BOOT_SLOT_A);
    for(unsigned j=0;j<sizeof payload;++j) payload[j]=(uint8_t)(j*17u);
    memset(payload+100,0xff,8); memset(payload+sizeof payload-4,0xff,4);
    put32(payload,BOOT_INITIAL_MSP); put32(payload+4,BOOT_SLOT_A_BASE+9);
    memset(&authority,0,sizeof authority);
    authority.sequence=100; authority.current_slot=BOOT_SLOT_A;
    authority.previous_slot=authority.candidate_slot=authority.intent_slot=BOOT_SLOT_NONE;
    authority.confirmed_mask=1;
    memcpy(authority.images[0].payload_hash,m.payload_hash,32);
    authority.images[0].release=m.release; authority.images[0].payload_length=m.payload_length;
    authority.images[0].trial_id=20;
    CHECK(boot_metadata_validate(&authority)==BOOT_OK);
    CHECK(boot_metadata_encode(&authority,BOOT_COMMIT_VALID,flash.bytes,BOOT_METADATA_BYTES)==BOOT_OK);
    memset(&sentinel,0xa5,sizeof sentinel); out=sentinel;
    resc[0]=resc[1]=resc[2]=2; last_clear=UINT32_MAX;
    params=(boot_sequence_parameters_t){&c,words,hz,&producer,delay,&registers,&store,&images,payload_read,&token};
}
static void duplicate(void) {
    unsigned a=calls,b=resc_reads,d=resc_writes,e=payload_reads,f=feeds;
    size_t g=flash.calls; boot_launch_decision_t saved=out;
    boot_sequence_result_t r=invoke();
    CHECK(r.status==BOOT_SEQUENCE_CONSUMED && r.stage==BOOT_SEQUENCE_NONE);
    CHECK(calls==a && resc_reads==b && resc_writes==d && payload_reads==e && feeds==f && flash.calls==g);
    CHECK(!memcmp(&out,&saved,sizeof out));
}
static void success(boot_sequence_result_t r) {
    CHECK(r.status==BOOT_SEQUENCE_OK && r.stage==BOOT_SEQUENCE_RUNTIME);
    CHECK(r.runtime.status==BOOT_RUNTIME_WATCHDOG_OK && r.runtime.valid==7);
    CHECK(r.runtime.runtime.status==BOOT_RUNTIME_OK && r.runtime.runtime.valid==3);
    CHECK(r.runtime.runtime.selection.evidence_mask==1);
    CHECK(r.runtime.runtime.selection.verify_result[0]==BOOT_VERIFY_OK);
    CHECK(out.image.manifest.slot==BOOT_SLOT_A && out.image.initial_msp==BOOT_INITIAL_MSP);
    CHECK(out.image.reset_vector==BOOT_SLOT_A_BASE+9);
    CHECK(!memcmp(&out.snapshot.record,&authority,sizeof authority));
    CHECK(payload_reads==1 && configs==1 && runtime_calls==1 && loads==1 && icrs==3 && feeds==2);
    CHECK(history[0].id==1 && history[0].epoch==1 && history[0].completed==0);
    CHECK(history[1].id==1 && history[1].epoch==2 && history[1].completed==sizeof payload);
    CHECK(c.watchdog.epoch==3 && c.watchdog.verification==1 && c.watchdog.evidence==0 && !c.watchdog.blocked);
    CHECK(c.watchdog.owner==BOOT_WATCHDOG_BOOT && c.watchdog.reload==5u*hz && load==5u*hz);
    CHECK(c.watchdog.io.context==c.context && c.watchdog.io.read_register==rd && c.watchdog.io.write_register==wr);
    CHECK(c.state==BOOT_STARTUP_EMPTY && flash.erases==0 && flash.writes==0);
    duplicate();
}
static void reset_failed(boot_sequence_result_t r,boot_reset_handler_result_t expected,uint32_t valid) {
    CHECK(r.status==BOOT_SEQUENCE_FAILED && r.stage==BOOT_SEQUENCE_RUNTIME);
    CHECK(r.runtime.status==BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED && r.runtime.valid==3);
    CHECK(r.runtime.runtime.status==BOOT_RUNTIME_RESET_FAILED && r.runtime.runtime.valid==1);
    CHECK(r.runtime.runtime.reset_status==expected && r.runtime.runtime.reset.valid==valid);
    CHECK(flash.calls==0 && payload_reads==0 && feeds==0 && configs==1 && runtime_calls==1);
    CHECK(loads==1 && icrs==1 && c.watchdog.epoch==1 && c.watchdog.verification==0);
    CHECK(c.watchdog.blocked && !c.watchdog.evidence && !memcmp(&out,&sentinel,sizeof out));
    duplicate();
}
#include "sequence_checked.h"

int main(int argc,char **argv) {
    CHECK(argc==2); fresh();
    if (checked_case(argv[1])) { /* Focused launch roster, isolated process. */ }
    else if(!strcmp(argv[1],"signed-run")) success(invoke());
    else if(!strcmp(argv[1],"early-stop")) {
        static boot_startup_t saved; uint32_t diag[16];
        memcpy(&saved,&c,sizeof c); memcpy(diag,words,sizeof words); read_fault=1;
        boot_sequence_result_t r=invoke();
        CHECK(r.status==BOOT_SEQUENCE_FAILED && r.stage==BOOT_SEQUENCE_EARLY_READ);
        CHECK(resc_reads==1 && !resc_writes && !calls && !configs && !runtime_calls && !flash.calls);
        CHECK(!memcmp(&saved,&c,sizeof c) && !memcmp(diag,words,sizeof words) && !memcmp(&out,&sentinel,sizeof out)); duplicate();
    } else if(!strcmp(argv[1],"producer-stop")) {
        mode=1; boot_sequence_result_t r=invoke();
        CHECK(r.status==BOOT_SEQUENCE_FAILED && r.stage==BOOT_SEQUENCE_PRODUCER);
        CHECK(resc_reads==1 && !resc_writes && calls==3 && writes==1 && delays==0);
        CHECK(!configs && !runtime_calls && !flash.calls && !payload_reads);
        CHECK(c.state==BOOT_STARTUP_FAILED && c.watchdog.blocked && !c.watchdog.evidence);
        CHECK(!memcmp(&out,&sentinel,sizeof out)); duplicate();
    } else if(!strcmp(argv[1],"capture-read-failure")) {
        read_fault=2; reset_failed(invoke(),BOOT_RESET_HANDLER_CAPTURE_FAILED,0);
        CHECK(resc_reads==2 && resc_writes==0);
    } else if(!strcmp(argv[1],"capture-fullword-mismatch")) {
        resc[1]^=0x80000000u; reset_failed(invoke(),BOOT_RESET_HANDLER_CAPTURE_FAILED,0);
        CHECK(resc_reads==2 && resc_writes==0);
    } else if(!strcmp(argv[1],"fresh-clear")) {
        resc[0]=resc[1]=0x80000002u; resc[2]=0xc000001au;
        success(invoke()); CHECK(resc_reads==3 && resc_writes==1 && last_clear==0xc0000018u);
    } else if(!strcmp(argv[1],"zero-supported")) {
        resc[0]=resc[1]=0x80000000u; success(invoke());
        CHECK(resc_reads==2 && resc_writes==0);
    } else if(!strcmp(argv[1],"clear-faults")) {
        read_fault=3; reset_failed(invoke(),BOOT_RESET_HANDLER_CLEAR_READ_FAILED,BOOT_RESET_REPORT_CAPTURE);
        CHECK(resc_reads==3 && resc_writes==0);
        fresh(); write_fault=1; reset_failed(invoke(),BOOT_RESET_HANDLER_CLEAR_WRITE_FAILED,BOOT_RESET_REPORT_CAPTURE);
        CHECK(resc_reads==3 && resc_writes==1 && last_clear==0);
    } else if(!strcmp(argv[1],"guards")) {
        check_guards=1; success(invoke()); CHECK(guard_checks>20);
    } else if(!strcmp(argv[1],"structural")) {
        /* Sequential corrections are allowed ONLY before admission. */
        for(unsigned bad=0;bad<9;++bad) {
            boot_sequence_parameters_t p=params;
            boot_launch_decision_t *o=&out;
            if(bad==0) o=NULL;
            if(bad==1) o=(void *)&seq;
            if(bad==2) o=(void *)((unsigned char *)&out+1);
            if(bad==3) p.actual_hz=0;
            if(bad==4) p.actual_hz=UINT32_MAX/5u+1;
            if(bad==5) p.continuity=NULL;
            if(bad==6) p.reader=NULL;
            if(bad==7) p.reader_context=&out;
            if(bad==8) p.diagnostic=(void *)&out;
            unsigned char before[sizeof seq]; memcpy(before,&seq,sizeof seq);
            boot_sequence_result_t r=boot_sequence_run(&seq,&p,o);
            CHECK(r.status==BOOT_SEQUENCE_ARGUMENT && r.stage==BOOT_SEQUENCE_NONE);
            CHECK(!seq.consumed && !memcmp(before,&seq,sizeof seq));
            CHECK(!calls && !resc_reads && !flash.calls && !payload_reads && !memcmp(&out,&sentinel,sizeof out));
        }
        success(invoke());
    } else {
        CHECK(!strcmp(argv[1],"signature-reject")); images.wire[0][96]^=1;
        boot_sequence_result_t r=invoke();
        CHECK(r.status==BOOT_SEQUENCE_FAILED && r.stage==BOOT_SEQUENCE_RUNTIME);
        CHECK(r.runtime.status==BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED && r.runtime.runtime.valid==3);
        CHECK(r.runtime.runtime.selection.evidence_mask==1 && r.runtime.runtime.selection.verify_result[0]==BOOT_VERIFY_ERROR_SIGNATURE);
        CHECK(!payload_reads && !feeds && configs==1 && c.watchdog.verification==1 && c.watchdog.epoch==1);
        CHECK(c.watchdog.blocked && !c.watchdog.evidence && !memcmp(&out,&sentinel,sizeof out)); duplicate();
    }
    printf("SEQUENCE_CASE_OK %s\n",argv[1]); return 0;
}
