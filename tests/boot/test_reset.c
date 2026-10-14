#include "lm3s6965/boot_reset.h"
#include "flash_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
/* Independent literal truth tables indexed by the five exact-part RESC bits.
 * POR contributes no category. SW contributes only in the intent table. */
static const uint32_t without_intent[32] = {
    0,2,0,2,3,5,3,5,1,5,1,5,5,5,5,5,
    0,2,0,2,3,5,3,5,1,5,1,5,5,5,5,5
};
static const uint32_t with_intent[32] = {
    0,2,0,2,3,5,3,5,1,5,1,5,5,5,5,5,
    4,5,4,5,5,5,5,5,5,5,5,5,5,5,5,5
};
static uint32_t history(unsigned count, unsigned code)
{
    uint32_t a = (uint32_t)count << 2;
    unsigned i;
    for (i=0; i<count; ++i) { a |= (uint32_t)(1u+code%5u) << (5u+3u*i); code /= 5u; }
    return a;
}
static boot_metadata_t fixture(unsigned slot, unsigned state, uint32_t h,
    uint32_t other, unsigned intent)
{
    boot_metadata_t r = {0};
    r.sequence=100u; r.current_slot=1u-slot;
    r.candidate_slot=(state==1u || state==2u) ? slot : BOOT_SLOT_NONE;
    r.previous_slot=r.intent_slot=BOOT_SLOT_NONE;
    r.confirmed_mask=UINT32_C(1)<<(1u-slot);
    r.images[slot].payload_length=236u; r.images[slot].release=7u;
    r.images[slot].payload_hash[0]=99u; r.images[slot].trial_id=20u;
    r.images[slot].accounting=h|state;
    r.images[1u-slot].payload_length=236u; r.images[1u-slot].release=8u;
    r.images[1u-slot].payload_hash[0]=88u; r.images[1u-slot].trial_id=10u;
    r.images[1u-slot].accounting=other;
    if (intent!=0u) { r.intent_kind=1u; r.intent_slot=slot; r.intent_trial_id=20u; }
    return r;
}
/* Runner composition: NO_CHANGE never calls replacement, hence zero writes.
 * This is test glue, not an added production flash/runtime API. */
static void store_nochange(const boot_metadata_t *r, uint32_t flags)
{
    flash_model_t m;
    boot_store_adapter_t a;
    boot_store_snapshot_t s, committed;
    boot_metadata_t out;
    boot_reset_decision_t d;
    boot_lifecycle_result_t result;
    flash_model_init(&m);
    CHECK(boot_metadata_encode(r,0u,m.bytes,168u)==BOOT_OK);
    a=flash_model_adapter(&m);
    CHECK(boot_store_load(&a,&s)==BOOT_STORE_OK);
    result=boot_reset_propose(&s.record,flags,&out,&d);
    if (result==BOOT_LIFECYCLE_CHANGE)
        CHECK(boot_store_replace(&a,&s,&out,&committed)==BOOT_STORE_OK);
    CHECK(result==BOOT_LIFECYCLE_NO_CHANGE);
    CHECK(memcmp(&out,r,sizeof out)==0);
    CHECK(m.writes==0u && m.erases==0u);
}
static void matrix(void)
{
    static const unsigned powers[4]={1u,5u,25u,125u};
    static const uint32_t high[4]={0u,0x20u,0x80000000u,0xffffffe0u};
    unsigned slot,state,count,code,oc,oh,intent,mask,h;
    for (slot=0;slot<2u;++slot)
    for (state=0;state<4u;++state)
    for (count=0;count<4u;++count)
    for (code=0;code<powers[count];++code)
    for (oc=0;oc<3u;++oc)
    for (oh=0;oh<powers[oc];++oh)
    for (intent=0;intent<2u;++intent)
    {
        boot_metadata_t r;
        if ((state==1u && count!=0u) || (state==2u && count==3u) ||
            (state==0u && count==3u) || (state==3u && count!=3u) ||
            (intent!=0u && state!=2u)) continue;
        r=fixture(slot,state,history(count,code),history(oc,oh),intent);
        if (state==3u) r.images[slot].accounting |=16u;
        CHECK(boot_metadata_validate(&r)==BOOT_OK);
        for (mask=0;mask<32u;++mask)
        for (h=0;h<4u;++h)
        {
            boot_metadata_t out,expected=r;
            boot_reset_decision_t d;
            uint32_t reason=state==2u ? (intent!=0u ? with_intent[mask] : without_intent[mask]) : 0u;
            boot_lifecycle_result_t result=boot_reset_propose(&r,(uint32_t)mask|high[h],&out,&d);
            CHECK(d.supported_flags==mask && d.trial_applicable==(state==2u ? 1u : 0u));
            CHECK(d.counted==(reason!=0u ? 1u : 0u) && d.reason==reason);
            CHECK(d.consume_intent==(reason!=0u ? intent : 0u));
            if (reason!=0u)
            {
                expected.sequence=101u;
                expected.images[slot].accounting=history(count,code)|
                    ((uint32_t)(count+1u)<<2)|2u|(reason<<(5u+3u*count));
                /* Replace count bits, not OR them with the old count. */
                expected.images[slot].accounting &=~UINT32_C(12);
                expected.images[slot].accounting |=(uint32_t)(count+1u)<<2;
                if (count==2u) { expected.images[slot].accounting=
                    (expected.images[slot].accounting & ~UINT32_C(3))|3u|16u;
                    expected.candidate_slot=BOOT_SLOT_NONE; }
                if (intent!=0u) { expected.intent_kind=0u; expected.intent_slot=BOOT_SLOT_NONE; expected.intent_trial_id=0u; }
                CHECK(result==BOOT_LIFECYCLE_CHANGE);
            }
            else CHECK(result==BOOT_LIFECYCLE_NO_CHANGE);
            CHECK(memcmp(&out,&expected,sizeof out)==0);
            CHECK(boot_metadata_validate(&out)==BOOT_OK);
        }
    }
    for (slot=0;slot<2u;++slot)
    for (intent=0;intent<2u;++intent)
    for (mask=0;mask<32u;++mask)
    {
        boot_metadata_t r=fixture(slot,2u,0u,0u,intent);
        if ((intent!=0u ? with_intent[mask] : without_intent[mask])==0u)
        { store_nochange(&r,mask); r.sequence=UINT32_MAX; store_nochange(&r,mask|0xffffffe0u); }
    }
}
static void boundaries(void)
{
    unsigned slot,intent,mask,count,code,role;
    static const unsigned powers[3]={1u,5u,25u};
    for(slot=0;slot<2u;++slot)
    for(count=0;count<3u;++count)
    for(code=0;code<powers[count];++code)
    for(intent=0;intent<2u;++intent)
    for(mask=0;mask<32u;++mask)
    {
        boot_metadata_t r=fixture(slot,2u,history(count,code),0u,intent),out,sentinel;
        boot_reset_decision_t d,ds;
        uint32_t reason=intent!=0u ? with_intent[mask] : without_intent[mask];
        r.sequence=UINT32_MAX;
        memset(&sentinel,0xa5,sizeof sentinel); out=sentinel;
        memset(&ds,0x5a,sizeof ds); d=ds;
        if(reason!=0u) {
            CHECK(boot_reset_propose(&r,mask|0xffffffe0u,&out,&d)==BOOT_LIFECYCLE_ERROR_EXHAUSTED);
            CHECK(memcmp(&out,&sentinel,sizeof out)==0 && memcmp(&d,&ds,sizeof d)==0);
        } else {
            CHECK(boot_reset_propose(&r,mask|0xffffffe0u,&out,&d)==BOOT_LIFECYCLE_NO_CHANGE);
            CHECK(memcmp(&out,&r,sizeof r)==0);
        }
    }
    /* Confirmed NONE with retained history in current/previous/roleless forms,
     * and an absent empty slot, all remain uncounted for every raw mask. */
    for(slot=0;slot<2u;++slot)
    for(count=0;count<3u;++count)
    for(code=0;code<powers[count];++code)
    for(role=0;role<4u;++role)
    for(mask=0;mask<32u;++mask)
    {
        boot_metadata_t r=fixture(slot,0u,history(count,code),0u,0u);
        if(role<3u) {
            r.confirmed_mask=3u;
            if(role==0u) r.current_slot=slot;
            if(role==1u) r.previous_slot=slot;
        } else memset(&r.images[slot],0,sizeof r.images[slot]);
        store_nochange(&r,mask|0xffffffe0u);
    }
}
static void errors(void)
{
    boot_metadata_t r=fixture(0u,2u,0u,0u,1u),out,sentinel,bad;
    boot_reset_decision_t d,ds;
    unsigned i;
    memset(&sentinel,0xa5,sizeof sentinel); memset(&ds,0x5a,sizeof ds);
    for (i=0;i<7u;++i)
    {
        bad=r;
        switch(i) {
        case 0: bad.intent_slot=1u; break;
        case 1: bad.intent_trial_id=19u; break;
        case 2: bad.intent_kind=2u; break;
        case 3: bad.intent_kind=0u; break;
        case 4: bad.images[0].accounting=1u; break;
        case 5: bad.confirmed_mask=3u; break;
        default: bad.images[0].accounting|=0x4000u; break;
        }
        out=sentinel; d=ds;
        CHECK(boot_reset_propose(&bad,0u,&out,&d)==BOOT_LIFECYCLE_ERROR_RECORD);
        CHECK(memcmp(&out,&sentinel,sizeof out)==0 && memcmp(&d,&ds,sizeof d)==0);
    }
    r.sequence=UINT32_MAX; out=sentinel; d=ds;
    CHECK(boot_reset_propose(&r,16u,&out,&d)==BOOT_LIFECYCLE_ERROR_EXHAUSTED);
    CHECK(memcmp(&out,&sentinel,sizeof out)==0 && memcmp(&d,&ds,sizeof d)==0);
    CHECK(boot_reset_propose(&r,0u,&r,&d)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
    CHECK(boot_reset_propose(NULL,0u,&out,&d)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
    CHECK(boot_reset_propose(&r,0u,NULL,&d)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
    CHECK(boot_reset_propose(&r,0u,&out,NULL)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
    CHECK(boot_reset_propose(&r,0u,&out,(boot_reset_decision_t *)(void *)&out)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
    CHECK(boot_reset_propose(&r,0u,&out,(boot_reset_decision_t *)(void *)&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
    CHECK(memcmp(&out,&sentinel,sizeof out)==0);
}
static void cuts(void)
{
    unsigned slot,source,count;
    for(slot=0;slot<2u;++slot)
    for(source=0;source<2u;++source)
    for(count=0;count<3u;++count)
    {
        flash_model_t base,m;
        boot_store_adapter_t a;
        boot_store_snapshot_t s,committed,reloaded,sentinel;
        boot_metadata_t r=fixture(slot,2u,history(count,0u),history(2u,9u),1u),desired;
        boot_reset_decision_t d;
        size_t call,bits,calls;
        flash_model_init(&base);
        CHECK(boot_metadata_encode(&r,0u,base.bytes+source*1024u,168u)==BOOT_OK);
        a=flash_model_adapter(&base); CHECK(boot_store_load(&a,&s)==BOOT_STORE_OK);
        CHECK(boot_reset_propose(&s.record,24u,&desired,&d)==BOOT_LIFECYCLE_CHANGE);
        CHECK(d.reason==5u && d.consume_intent==1u);
        base.calls=0u; m=base; a=flash_model_adapter(&m);
        CHECK(boot_store_replace(&a,&s,&desired,&committed)==BOOT_STORE_OK); calls=m.calls;
        memset(&sentinel,0xa5,sizeof sentinel);
        for(call=1;call<=calls;++call)
        {
            size_t max=call==3u ? 8192u : ((call>=5u && call<=45u) || call==47u ? 32u : 0u);
            for(bits=0;bits<=max;++bits)
            {
                m=base; m.cut_call=call; m.cut_bits=bits; a=flash_model_adapter(&m); committed=sentinel;
                CHECK(boot_store_replace(&a,&s,&desired,&committed)!=BOOT_STORE_OK);
                CHECK(memcmp(&committed,&sentinel,sizeof committed)==0);
                m.stopped=0; m.cut_call=0u;
                CHECK(boot_store_load(&a,&reloaded)==BOOT_STORE_OK);
                CHECK(memcmp(&reloaded.record,&r,sizeof r)==0 || memcmp(&reloaded.record,&desired,sizeof desired)==0);
                /* Old count WITH intent or new count WITHOUT intent, never split. */
                CHECK((reloaded.record.intent_kind==1u)==(reloaded.record.sequence==r.sequence));
            }
            m=base; m.cut_call=call; m.fail_after=1; a=flash_model_adapter(&m);
            CHECK(boot_store_replace(&a,&s,&desired,&committed)!=BOOT_STORE_OK);
            m.stopped=0; m.cut_call=0u;
            CHECK(boot_store_load(&a,&reloaded)==BOOT_STORE_OK);
            CHECK(memcmp(&reloaded.record,&r,sizeof r)==0 || memcmp(&reloaded.record,&desired,sizeof desired)==0);
        }
    }
}
static boot_verified_image_t image(const boot_metadata_t *r, unsigned slot)
{
    boot_verified_image_t v={0};
    v.manifest.slot=slot; v.manifest.execution_base=slot==0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    v.manifest.payload_length=r->images[slot].payload_length; v.manifest.release=r->images[slot].release;
    memcpy(v.manifest.payload_hash,r->images[slot].payload_hash,32u);
    v.initial_msp=BOOT_INITIAL_MSP; v.reset_vector=v.manifest.execution_base+9u;
    return v;
}
static void deadlines(void)
{
    unsigned slot,n;
    for(slot=0;slot<2u;++slot)
    {
        boot_metadata_t r=fixture(slot,2u,0u,history(2u,9u),0u),intent,next,retry,confirmed;
        boot_verified_image_t v=image(&r,slot);
        boot_deadline_request_t req={slot,20u,4u};
        boot_confirm_request_t confirm;
        boot_reset_decision_t d;
        confirm.slot=slot; confirm.trial_id=20u; memcpy(confirm.payload_hash,v.manifest.payload_hash,32u);
        for(n=1;n<=3u;++n)
        {
            CHECK(boot_lifecycle_deadline_intent(&r,&req,&v,&intent)==BOOT_LIFECYCLE_CHANGE);
            CHECK(intent.images[slot].accounting==r.images[slot].accounting);
            CHECK(boot_reset_propose(&intent,2u,&retry,&d)==BOOT_LIFECYCLE_NO_CHANGE);
            CHECK(memcmp(&intent,&retry,sizeof intent)==0); /* KEEP, including stale intent */
            CHECK(boot_reset_propose(&retry,16u,&next,&d)==BOOT_LIFECYCLE_CHANGE);
            CHECK(d.reason==4u && d.counted==1u && d.consume_intent==1u);
            CHECK(((next.images[slot].accounting>>2)&3u)==n);
            CHECK(((next.images[slot].accounting>>(5u+3u*(n-1u)))&7u)==4u);
            CHECK(boot_reset_propose(&next,16u,&retry,&d)==BOOT_LIFECYCLE_NO_CHANGE);
            CHECK(memcmp(&retry,&next,sizeof next)==0); /* old single intent not counted twice */
            if(n<3u) {
                CHECK(boot_lifecycle_confirm(&next,&confirm,&v,7u,&confirmed)==BOOT_LIFECYCLE_CHANGE);
                CHECK(confirmed.images[slot].accounting==(next.images[slot].accounting&~UINT32_C(3)));
                CHECK(memcmp(&confirmed.images[1u-slot],&r.images[1u-slot],sizeof r.images[0])==0);
                CHECK(boot_reset_propose(&confirmed,31u,&retry,&d)==BOOT_LIFECYCLE_NO_CHANGE);
            }
            r=next;
        }
        CHECK(r.candidate_slot==BOOT_SLOT_NONE && (r.images[slot].accounting&19u)==19u);
    }
}
int main(void)
{
    matrix(); boundaries(); errors(); deadlines(); cuts();
    printf("boot reset: %u checks passed\n",checks);
    return 0;
}
