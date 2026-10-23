#include "lm3s6965/boot_reset_handler.h"
#include "flash_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
typedef struct
{
    flash_model_t flash;
    boot_store_adapter_t inner;
    uint32_t flags, fresh, written;
    unsigned register_calls, register_cut;
    int write_effect, stale;
    char log[256];
    size_t logs;
} model_t;
static void log_op(model_t *m, char c)
{ CHECK(m->logs < sizeof m->log); m->log[m->logs++] = c; }
static int reg_read(void *ctx, uint32_t *out)
{
    model_t *m=ctx;
    log_op(m,'R'); ++m->register_calls;
    *out=m->register_calls==1u ? m->flags : m->fresh;
    return m->register_calls==m->register_cut ? -1 : 0;
}
static int reg_write(void *ctx, uint32_t value)
{
    model_t *m=ctx;
    log_op(m,'W'); ++m->register_calls; m->written=value;
    if (m->register_calls!=m->register_cut || m->write_effect) m->flags=value;
    return m->register_calls==m->register_cut ? -1 : 0;
}
static int flash_read(void *ctx, uint32_t address, uint8_t *bytes, size_t size)
{
    model_t *m=ctx;
    log_op(m,'r');
    /* External writer simulation: break exclusive ownership to verify STALE. */
    if (m->stale && m->flash.calls==2u) {
        boot_metadata_t r;
        CHECK(boot_metadata_decode(m->flash.bytes,BOOT_METADATA_BYTES,&r)==BOOT_OK);
        ++r.sequence;
        CHECK(boot_metadata_encode(&r,BOOT_COMMIT_VALID,m->flash.bytes,BOOT_METADATA_BYTES)==BOOT_OK);
    }
    return m->inner.read(m->inner.context,address,bytes,size);
}
static int flash_erase(void *ctx, uint32_t address)
{ model_t *m=ctx; log_op(m,'e'); return m->inner.erase_page(m->inner.context,address); }
static int flash_program(void *ctx, uint32_t address, uint32_t word)
{ model_t *m=ctx; log_op(m,'p'); return m->inner.program_word(m->inner.context,address,word); }
static boot_metadata_t fixture(unsigned state, unsigned intent)
{
    boot_metadata_t r={0};
    r.sequence=100u; r.current_slot=0u; r.previous_slot=BOOT_SLOT_NONE;
    r.candidate_slot=state==1u || state==2u ? 1u : BOOT_SLOT_NONE;
    r.confirmed_mask=1u; r.intent_slot=BOOT_SLOT_NONE;
    r.images[0].payload_length=r.images[1].payload_length=236u;
    r.images[0].payload_hash[0]=88u; r.images[1].payload_hash[0]=99u;
    r.images[1].trial_id=20u; r.images[1].accounting=state;
    if (state==3u) r.images[1].accounting=3u|12u|16u|32u|256u|2048u;
    if (intent) { r.intent_kind=1u; r.intent_slot=1u; r.intent_trial_id=20u; }
    CHECK(boot_metadata_validate(&r)==BOOT_OK);
    return r;
}
static void init(model_t *m, const boot_metadata_t *r, uint32_t flags, uint32_t fresh)
{
    memset(m,0,sizeof *m); flash_model_init(&m->flash);
    m->inner=flash_model_adapter(&m->flash); m->flags=flags; m->fresh=fresh;
    CHECK(boot_metadata_encode(r,BOOT_COMMIT_VALID,m->flash.bytes,BOOT_METADATA_BYTES)==BOOT_OK);
}
static boot_reset_handler_result_t run(model_t *m, boot_reset_handler_report_t *p)
{
    boot_reset_register_adapter_t r={m,reg_read,reg_write};
    boot_store_adapter_t s={m,flash_read,flash_erase,flash_program};
    return boot_reset_handler_run(&r,&s,p);
}
static void matrix(void)
{
    static const uint32_t reasons[32]={0,2,0,2,3,5,3,5,1,5,1,5,5,5,5,5,4,5,4,5,5,5,5,5,5,5,5,5,5,5,5,5};
    static const uint32_t high[]={0u,32u,UINT32_C(0x80000000),UINT32_C(0xffffffe0)};
    unsigned mask,newbits,h;
    for(mask=0;mask<32u;++mask) for(newbits=0;newbits<32u;++newbits)
    for(h=0;h<4u;++h) {
        boot_metadata_t initial=fixture(2u,1u), desired;
        model_t m; boot_reset_handler_report_t p; boot_reset_decision_t d;
        uint32_t captured=mask|high[h], fresh=newbits|high[h];
        init(&m,&initial,captured,fresh);
        CHECK(run(&m,&p)==BOOT_RESET_HANDLER_OK);
        CHECK(p.captured_flags==captured && (p.valid&BOOT_RESET_REPORT_SNAPSHOT)!=0u);
        CHECK(p.decision.reason==reasons[mask]);
        CHECK(m.log[0]=='R');
        if(mask) { CHECK(memcmp(m.log,reasons[mask] ? "RRWrrrr" : "RRWrr",reasons[mask] ? 7u : 5u)==0);
            CHECK(m.written==(fresh&~(mask&31u))); }
        else CHECK(memcmp(m.log,"Rrr",3u)==0 && m.register_calls==1u);
        CHECK(boot_reset_propose(&initial,captured,&desired,&d)==p.lifecycle_result);
        CHECK(memcmp(&p.snapshot.record,&desired,sizeof desired)==0);
        if(reasons[mask]) {
            CHECK(m.flash.erases==1u && m.flash.writes==42u);
            CHECK(p.snapshot.record.intent_kind==0u && p.decision.consume_intent==1u);
            CHECK(m.log[7]=='e');
        } else CHECK(m.flash.erases==0u && m.flash.writes==0u && p.snapshot.record.intent_kind==1u);
    }
}
static void boundaries(void)
{
    unsigned state,cut;
    boot_reset_handler_report_t p;
    model_t m;
    for(state=0;state<4u;++state) {
        boot_metadata_t r=fixture(state,0u);
        init(&m,&r,6u,6u); CHECK(run(&m,&p)==BOOT_RESET_HANDLER_OK);
        CHECK(p.decision.counted==(state==2u ? 1u : 0u));
        if(state==2u) CHECK(p.decision.reason==BOOT_REASON_BROWNOUT);
    }
    for(cut=1;cut<=3u;++cut) {
        boot_metadata_t r=fixture(2u,1u);
        init(&m,&r,UINT32_C(0x80000008),UINT32_C(0x80000008)); m.register_cut=cut; m.write_effect=1;
        CHECK(run(&m,&p)==(cut==1u ? BOOT_RESET_HANDLER_CAPTURE_FAILED : cut==2u ? BOOT_RESET_HANDLER_CLEAR_READ_FAILED : BOOT_RESET_HANDLER_CLEAR_WRITE_FAILED));
        CHECK(m.flash.calls==0u && m.register_calls==cut);
        CHECK(p.valid==(cut==1u ? 0u : BOOT_RESET_REPORT_CAPTURE));
        if(cut>1u) CHECK(p.captured_flags==UINT32_C(0x80000008));
        if(cut==3u) CHECK(m.flags==UINT32_C(0x80000000));
    }
    {
        boot_metadata_t r=fixture(2u,1u); r.sequence=UINT32_MAX;
        init(&m,&r,2u,2u); CHECK(run(&m,&p)==BOOT_RESET_HANDLER_OK);
        CHECK(m.flash.writes==0u && p.snapshot.record.sequence==UINT32_MAX);
        init(&m,&r,8u,8u); CHECK(run(&m,&p)==BOOT_RESET_HANDLER_CLASSIFY_FAILED);
        CHECK(p.lifecycle_result==BOOT_LIFECYCLE_ERROR_EXHAUSTED && m.flash.writes==0u);
        r=fixture(2u,1u); init(&m,&r,8u,8u); m.flash.bytes[0]^=1u;
        CHECK(run(&m,&p)==BOOT_RESET_HANDLER_RECOVERY && p.store_result==BOOT_STORE_NO_RECORD);
        CHECK(m.flash.erases==0u && p.captured_flags==8u);
        init(&m,&r,8u,8u); m.stale=1;
        CHECK(run(&m,&p)==BOOT_RESET_HANDLER_STORE_FAILED && p.store_result==BOOT_STORE_STALE);
        CHECK(m.flash.erases==0u && (p.valid&BOOT_RESET_REPORT_SNAPSHOT)==0u);
        init(&m,&r,8u,8u);
        ++r.images[0].release;
        CHECK(boot_metadata_encode(&r,BOOT_COMMIT_VALID,m.flash.bytes+BOOT_ERASE_BYTES,BOOT_METADATA_BYTES)==BOOT_OK);
        CHECK(run(&m,&p)==BOOT_RESET_HANDLER_RECOVERY && p.store_result==BOOT_STORE_CONFLICT);
        CHECK(m.flash.erases==0u);
        /* Same supported bit relatching is indistinguishable and cleared. */
        init(&m,&r,8u,8u); CHECK(run(&m,&p)==BOOT_RESET_HANDLER_OK);
        CHECK(m.flags==0u && p.decision.counted==1u);
        /* Silent staging mismatch propagates VERIFY, not success/rollback. */
        init(&m,&r,8u,8u); m.flash.cut_call=7u; m.flash.silent_program=1;
        CHECK(run(&m,&p)==BOOT_RESET_HANDLER_STORE_FAILED && p.store_result==BOOT_STORE_VERIFY);
        CHECK((p.valid&BOOT_RESET_REPORT_SNAPSHOT)==0u);
    }
}
static void cutpoints(void)
{
    boot_metadata_t r=fixture(2u,1u), desired;
    boot_reset_decision_t d;
    boot_reset_handler_report_t p;
    model_t complete;
    size_t cut,total;
    unsigned after,bits;
    CHECK(boot_reset_propose(&r,8u,&desired,&d)==BOOT_LIFECYCLE_CHANGE);
    init(&complete,&r,8u,8u); CHECK(run(&complete,&p)==BOOT_RESET_HANDLER_OK);
    total=complete.flash.calls;
    for(cut=1;cut<=total;++cut) for(after=0;after<2u;++after)
    for(bits=0;bits<=32u;++bits) {
        model_t m; boot_store_snapshot_t loaded; boot_store_adapter_t a;
        init(&m,&r,8u,8u); m.flash.cut_call=cut; m.flash.fail_after=(int)after; m.flash.cut_bits=bits;
        CHECK(run(&m,&p)==BOOT_RESET_HANDLER_STORE_FAILED);
        CHECK(p.captured_flags==8u && (p.valid&BOOT_RESET_REPORT_CLEAR)!=0u);
        CHECK((p.valid&BOOT_RESET_REPORT_SNAPSHOT)==0u && m.flash.calls==cut);
        if(cut>=total-3u) CHECK(p.store_result==BOOT_STORE_UNCERTAIN);
        else CHECK(p.store_result==BOOT_STORE_IO);
        m.flash.stopped=0; m.flash.cut_call=SIZE_MAX; a=flash_model_adapter(&m.flash);
        CHECK(boot_store_load(&a,&loaded)==BOOT_STORE_OK);
        CHECK(memcmp(&loaded.record,&r,sizeof r)==0 || memcmp(&loaded.record,&desired,sizeof desired)==0);
        /* Count and intent are inseparable old/new even on a completed but
         * failure-reported commit. Never infer rollback from the handler. */
        CHECK(loaded.record.intent_kind==(loaded.record.sequence==100u ? 1u : 0u));
        /* Next boot sees zero flags: no general replay; precommit clear loses
         * the event and preserves old pending intent, as explicitly accepted. */
        m.logs=0; m.register_calls=0; m.fresh=m.flags;
        CHECK(m.flags==0u); { unsigned writes=m.flash.writes, erases=m.flash.erases;
            CHECK(run(&m,&p)==BOOT_RESET_HANDLER_OK);
            CHECK(m.flash.writes==writes && m.flash.erases==erases);
            CHECK(memcmp(&p.snapshot.record,&loaded.record,sizeof loaded.record)==0);
        }
    }
}
static void arguments(void)
{
    boot_metadata_t record=fixture(2u,1u);
    model_t m; boot_reset_handler_report_t p,saved;
    boot_reset_register_adapter_t r;
    boot_store_adapter_t s;
    union { boot_reset_handler_report_t report; boot_reset_register_adapter_t reg; boot_store_adapter_t store; } alias;
    init(&m,&record,8u,8u); r=(boot_reset_register_adapter_t){&m,reg_read,reg_write};
    s=(boot_store_adapter_t){&m,flash_read,flash_erase,flash_program};
    memset(&p,0xa5,sizeof p); saved=p;
    CHECK(boot_reset_handler_run(NULL,&s,&p)==BOOT_RESET_HANDLER_ARGUMENT);
    CHECK(boot_reset_handler_run(&r,NULL,&p)==BOOT_RESET_HANDLER_ARGUMENT);
    CHECK(boot_reset_handler_run(&r,&s,NULL)==BOOT_RESET_HANDLER_ARGUMENT);
    r.read=NULL; CHECK(boot_reset_handler_run(&r,&s,&p)==BOOT_RESET_HANDLER_ARGUMENT); r.read=reg_read;
    r.write=NULL; CHECK(boot_reset_handler_run(&r,&s,&p)==BOOT_RESET_HANDLER_ARGUMENT); r.write=reg_write;
    s.read=NULL; CHECK(boot_reset_handler_run(&r,&s,&p)==BOOT_RESET_HANDLER_ARGUMENT); s.read=flash_read;
    s.erase_page=NULL; CHECK(boot_reset_handler_run(&r,&s,&p)==BOOT_RESET_HANDLER_ARGUMENT); s.erase_page=flash_erase;
    s.program_word=NULL; CHECK(boot_reset_handler_run(&r,&s,&p)==BOOT_RESET_HANDLER_ARGUMENT); s.program_word=flash_program;
    alias.reg=r; CHECK(boot_reset_handler_run(&alias.reg,&s,&alias.report)==BOOT_RESET_HANDLER_ARGUMENT);
    alias.store=s; CHECK(boot_reset_handler_run(&r,&alias.store,&alias.report)==BOOT_RESET_HANDLER_ARGUMENT);
    CHECK(memcmp(&p,&saved,sizeof p)==0 && m.logs==0u);
}
int main(void)
{
    arguments(); matrix(); boundaries(); cutpoints();
    printf("%u reset handler checks passed\n",checks); return 0;
}
