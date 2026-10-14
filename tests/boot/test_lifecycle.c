#include "lm3s6965/boot_lifecycle.h"
#include "flash_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
/* Existing test_format golden, independently specified; zlib CRC 23f4eb93.
 * Deliberately NOT authenticated. This suite tests structural transformations,
 * not crypto provenance (the verifier's separate suite supplies that gate). */
static const uint8_t golden[168] = {
    0x41,0x42,0x4d,0x31,1,0,0,0,0xa8,0,0,0,1,0,0,0,
    0,0,0,0,255,255,255,255,255,255,255,255,1,0,0,0,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    0x78,0x56,0x34,0x12,0xec,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,255,255,255,255,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0x93,0xeb,0xf4,0x23,0,0,0,0
};
static boot_metadata_t fixture(unsigned slot)
{
    boot_metadata_t r;
    CHECK(boot_metadata_decode(golden, sizeof golden, &r) == BOOT_OK);
    if (slot == 1u) { r.images[1] = r.images[0]; memset(&r.images[0],0,sizeof r.images[0]); }
    r.current_slot = slot; r.confirmed_mask = UINT32_C(1) << slot;
    r.sequence = 100u;
    return r;
}
static boot_upload_evidence_t evidence(const boot_metadata_t *r)
{
    boot_upload_evidence_t e = {0};
    const boot_image_entry_t *entry = &r->images[r->current_slot];
    e.running.manifest.slot = r->current_slot;
    e.running.manifest.execution_base = r->current_slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    e.running.manifest.payload_length = entry->payload_length;
    e.running.manifest.release = entry->release;
    memcpy(e.running.manifest.payload_hash,entry->payload_hash,32u);
    e.running.initial_msp = BOOT_INITIAL_MSP;
    e.running.reset_vector = e.running.manifest.execution_base + 9u;
    e.permitted_slot = 1u-r->current_slot;
    e.incoming = e.running.manifest;
    e.incoming.slot = e.permitted_slot;
    e.incoming.execution_base = e.permitted_slot == 0u ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
    e.incoming.release = 7u; e.incoming.payload_hash[0] = 99u;
    return e;
}
static boot_verified_image_t stored_image(const boot_upload_evidence_t *e)
{
    boot_verified_image_t v = {0}; v.manifest = e->incoming;
    v.initial_msp = BOOT_INITIAL_MSP; v.reset_vector = e->incoming.execution_base + 9u;
    return v;
}
static uint32_t history(unsigned count, unsigned code)
{
    uint32_t a = (uint32_t)count << 2;
    unsigned i;
    for (i=0; i<count; ++i) { a |= (uint32_t)(1u+code%5u) << (5u+3u*i); code /= 5u; }
    if (count == 3u) a |= 3u | 16u;
    return a;
}
static void matrix(void)
{
    unsigned slot, current_count, c, dest_count, d, role;
    for (slot=0; slot<2u; ++slot)
    for (current_count=0; current_count<3u; ++current_count)
    for (c=0; c<(current_count==0u ? 1u : current_count==1u ? 5u : 25u); ++c)
    for (dest_count=0; dest_count<4u; ++dest_count)
    for (d=0; d<(dest_count==0u ? 1u : dest_count==1u ? 5u : dest_count==2u ? 25u : 125u); ++d)
    for (role=0; role<4u; ++role)
    {
        boot_metadata_t r=fixture(slot), invalid, ready, expected;
        boot_upload_evidence_t e;
        boot_verified_image_t v;
        uint8_t before[168], after[168];
        unsigned dest=1u-slot, i;
        if (dest_count==3u && (role==1u || role==2u)) continue;
        r.images[slot].accounting=history(current_count,c); r.images[slot].trial_id=10u;
        if (role!=3u)
        {
            r.images[dest]=r.images[slot]; r.images[dest].release=88u;
            r.images[dest].accounting=history(dest_count,d); r.images[dest].trial_id=20u;
            if (role==1u || role==2u) r.confirmed_mask=3u;
            if (role==2u) r.previous_slot=dest;
        }
        e=evidence(&r); v=stored_image(&e);
        CHECK(boot_metadata_validate(&r)==BOOT_OK);
        CHECK(boot_lifecycle_invalidate(&r,&e,&invalid)==BOOT_LIFECYCLE_CHANGE);
        expected=r; expected.sequence=101u; expected.previous_slot=BOOT_SLOT_NONE;
        expected.confirmed_mask=UINT32_C(1)<<slot;
        memset(&expected.images[dest],0,sizeof expected.images[dest]);
        CHECK(memcmp(&invalid,&expected,sizeof expected)==0);
        CHECK(boot_metadata_encode(&r,0u,before,sizeof before)==BOOT_OK);
        CHECK(boot_metadata_encode(&invalid,0u,after,sizeof after)==BOOT_OK);
        for (i=0; i<168u; ++i)
        {
            unsigned start=32u+48u*dest;
            if (i>=start && i<start+48u) CHECK(after[i]==0u);
            else if (!(i>=12u && i<16u) && !(i>=24u && i<32u) && !(i>=160u && i<164u))
                CHECK(after[i]==before[i]);
        }
        CHECK(boot_lifecycle_accept_ready(&invalid,&e,&v,&ready)==BOOT_LIFECYCLE_CHANGE);
        expected.sequence=102u; expected.candidate_slot=dest;
        memcpy(expected.images[dest].payload_hash,e.incoming.payload_hash,32u);
        expected.images[dest].release=7u; expected.images[dest].payload_length=236u;
        expected.images[dest].accounting=1u; expected.images[dest].trial_id=102u;
        CHECK(memcmp(&ready,&expected,sizeof expected)==0);
        CHECK(boot_metadata_validate(&ready)==BOOT_OK);
        {
            boot_metadata_t trial, retry, intent, confirmed;
            boot_confirm_request_t req;
            boot_deadline_request_t deadline={dest,102u,BOOT_REASON_DEADLINE};
            CHECK(boot_lifecycle_begin_trial(&ready,&v,&trial)==BOOT_LIFECYCLE_CHANGE);
            expected.sequence=103u; expected.images[dest].accounting=BOOT_STATE_TRIAL;
            CHECK(memcmp(&trial,&expected,sizeof trial)==0);
            CHECK(boot_lifecycle_begin_trial(&trial,&v,&retry)==BOOT_LIFECYCLE_NO_CHANGE);
            CHECK(memcmp(&retry,&trial,sizeof retry)==0);
            /* Both directions and every retained reason combination 0..2. */
            trial.images[dest].accounting=history(current_count,c)|BOOT_STATE_TRIAL;
            CHECK(boot_lifecycle_deadline_intent(&trial,&deadline,&v,&intent)==BOOT_LIFECYCLE_CHANGE);
            expected=trial; expected.sequence=104u;
            expected.intent_kind=BOOT_INTENT_DEADLINE; expected.intent_slot=dest;
            expected.intent_trial_id=102u;
            CHECK(memcmp(&intent,&expected,sizeof intent)==0);
            CHECK(boot_lifecycle_deadline_intent(&intent,&deadline,&v,&retry)==BOOT_LIFECYCLE_NO_CHANGE);
            CHECK(memcmp(&retry,&intent,sizeof retry)==0);
            req.slot=dest; req.trial_id=102u;
            memcpy(req.payload_hash,v.manifest.payload_hash,32u);
            CHECK(boot_lifecycle_confirm(&intent,&req,&v,7u,&confirmed)==BOOT_LIFECYCLE_CHANGE);
            expected.sequence=105u; expected.previous_slot=slot; expected.current_slot=dest;
            expected.candidate_slot=BOOT_SLOT_NONE; expected.confirmed_mask=3u;
            expected.images[dest].accounting &= ~BOOT_ACCOUNT_STATE_MASK;
            expected.intent_kind=BOOT_INTENT_NONE; expected.intent_slot=BOOT_SLOT_NONE;
            expected.intent_trial_id=0u;
            CHECK(memcmp(&confirmed,&expected,sizeof confirmed)==0);
            CHECK(boot_lifecycle_confirm(&confirmed,&req,&v,0u,&retry)==BOOT_LIFECYCLE_NO_CHANGE);
            CHECK(memcmp(&retry,&confirmed,sizeof retry)==0);
        }
    }
}
static void fail(const boot_metadata_t *r,const boot_upload_evidence_t *e,
                 const boot_verified_image_t *v,int accept)
{
    boot_metadata_t out, sentinel;
    memset(&sentinel,0xa5,sizeof sentinel); out=sentinel;
    CHECK((accept ? boot_lifecycle_accept_ready(r,e,v,&out) :
        boot_lifecycle_invalidate(r,e,&out)) != BOOT_LIFECYCLE_CHANGE);
    CHECK(memcmp(&out,&sentinel,sizeof out)==0);
}
static void negatives(void)
{
    unsigned slot, mode, accept;
    for (slot=0; slot<2u; ++slot)
    for (accept=0; accept<2u; ++accept)
    for (mode=0; mode<24u; ++mode)
    {
        boot_metadata_t r=fixture(slot);
        boot_upload_evidence_t e=evidence(&r);
        boot_verified_image_t v=stored_image(&e);
        unsigned dest=1u-slot;
        switch(mode)
        {
        case 0: e.permitted_slot=BOOT_SLOT_NONE; break;
        case 1: e.incoming.slot=slot; break;
        case 2: e.incoming.execution_base++; break;
        case 3: e.incoming.payload_length=UINT32_MAX; break;
        case 4: e.incoming.payload_length=232u; break;
        case 5: e.incoming.payload_length=237u; break;
        case 6: e.running.manifest.payload_hash[0]++; break;
        case 7: e.running.manifest.release++; break;
        case 8: e.running.manifest.payload_length+=4u; break;
        case 9: e.running.manifest.slot=dest; break;
        case 10: e.running.initial_msp--; break;
        case 11: e.running.reset_vector &= ~UINT32_C(1); break;
        case 12: e.running.reset_vector=e.running.manifest.execution_base-1u; break;
        case 13: e.running.reset_vector=e.running.manifest.execution_base+237u; break;
        case 14: r.current_slot=BOOT_SLOT_NONE; r.confirmed_mask=0u; break;
        case 15: r.confirmed_mask=0u; break;
        case 16: r.images[dest]=r.images[slot]; r.images[dest].accounting=1u;
            r.images[dest].trial_id=50u; r.candidate_slot=dest; break;
        case 17: r.images[dest]=r.images[slot]; r.images[dest].accounting=2u;
            r.images[dest].trial_id=50u; r.candidate_slot=dest; break;
        case 18: r.sequence=UINT32_MAX; break;
        case 19: r.images[dest].release=1u; break;
        case 20: r.confirmed_mask|=4u; break;
        case 21: r.images[slot].trial_id=101u; break;
        case 22: e.running.manifest.execution_base++; break;
        default: e.permitted_slot=slot; e.incoming=e.running.manifest; break;
        }
        fail(&r,&e,&v,(int)accept);
    }
    for (slot=0; slot<2u; ++slot)
    for (mode=0; mode<13u; ++mode)
    {
        boot_metadata_t r=fixture(slot);
        boot_upload_evidence_t e=evidence(&r);
        boot_verified_image_t v=stored_image(&e);
        switch(mode)
        {
        case 0: v.manifest.payload_hash[0]++; break;
        case 1: v.manifest.release++; break;
        case 2: v.manifest.payload_length+=4u; break;
        case 3: v.manifest.signature[63]++; break;
        case 4: v.manifest.slot=slot; break;
        case 5: v.manifest.execution_base++; break;
        case 6: v.initial_msp--; break;
        case 7: v.reset_vector &= ~UINT32_C(1); break;
        case 8: v.reset_vector=v.manifest.execution_base-1u; break;
        case 9: v.reset_vector=v.manifest.execution_base+237u; break;
        case 10: r.images[1u-slot]=r.images[slot]; break;
        case 11: r.images[1u-slot]=r.images[slot]; r.previous_slot=1u-slot; r.confirmed_mask=3u; break;
        default: v.manifest.payload_length=BOOT_PAYLOAD_MAX+4u; break;
        }
        fail(&r,&e,&v,1);
    }
    {
        boot_metadata_t r=fixture(0u), out, before;
        boot_upload_evidence_t e=evidence(&r);
        boot_verified_image_t v=stored_image(&e);
        fail(NULL,&e,&v,0); fail(&r,NULL,&v,0); fail(&r,&e,NULL,1);
        CHECK(boot_lifecycle_invalidate(&r,&e,NULL)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        before=r;
        CHECK(boot_lifecycle_invalidate(&r,&e,&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_accept_ready(&r,&e,&v,&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(memcmp(&r,&before,sizeof r)==0);
        CHECK(boot_lifecycle_invalidate(&r,&e,(boot_metadata_t *)(void *)&e)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_accept_ready(&r,&e,&v,(boot_metadata_t *)(void *)&v)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        /* Max acceptance sequence is valid, not a missing trial sentinel. */
        r.sequence=UINT32_MAX-1u;
        CHECK(boot_lifecycle_accept_ready(&r,&e,&v,&out)==BOOT_LIFECYCLE_CHANGE);
        CHECK(out.sequence==UINT32_MAX && out.images[1].trial_id==UINT32_MAX);
        CHECK(boot_metadata_validate(&out)==BOOT_OK);
        r.sequence=100u;
        e.incoming.payload_length=BOOT_PAYLOAD_MAX;
        e.incoming.release=UINT32_MAX;
        v=stored_image(&e);
        v.reset_vector=v.manifest.execution_base+BOOT_PAYLOAD_MAX-1u;
        CHECK(boot_lifecycle_invalidate(&r,&e,&out)==BOOT_LIFECYCLE_CHANGE);
        CHECK(boot_lifecycle_accept_ready(&out,&e,&v,&before)==BOOT_LIFECYCLE_CHANGE);
        CHECK(before.images[1].payload_length==BOOT_PAYLOAD_MAX);
        CHECK(before.images[1].release==UINT32_MAX);
        CHECK(boot_lifecycle_accept_ready(&r,&e,&v,NULL)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        /* Zero signature bytes deliberately pass these structural gates; this
         * is NOT permission to substitute forged evidence for verifier output. */
        CHECK(e.incoming.signature[0]==0u);
    }
}
/* Compose pure proposals with reviewed commit-last store. These are metadata
 * cutpoints only, NOT upload erases, reset policy or target hardware evidence. */
static void persistence(void)
{
    unsigned slot, page, phase;
    for (slot=0; slot<2u; ++slot)
    for (page=0; page<2u; ++page)
    {
        flash_model_t base;
        boot_store_adapter_t a;
        boot_store_snapshot_t old, invalid, ready;
        boot_metadata_t r=fixture(slot), inv, acc;
        boot_upload_evidence_t e=evidence(&r);
        boot_verified_image_t v=stored_image(&e);
        unsigned dest=1u-slot;
        r.images[dest]=r.images[slot]; r.previous_slot=dest; r.confirmed_mask=3u;
        flash_model_init(&base);
        CHECK(boot_metadata_encode(&r,0u,base.bytes+page*1024u,168u)==BOOT_OK);
        a=flash_model_adapter(&base);
        CHECK(boot_store_load(&a,&old)==BOOT_STORE_OK);
        CHECK(boot_lifecycle_invalidate(&r,&e,&inv)==BOOT_LIFECYCLE_CHANGE);
        CHECK(base.erases==0u && base.writes==0u);
        CHECK(boot_store_load(&a,&invalid)==BOOT_STORE_OK);
        CHECK(invalid.record.images[dest].payload_length==236u); /* no erase entitlement yet */
        for (phase=0; phase<2u; ++phase)
        {
            size_t call;
            boot_metadata_t proposal=phase==0u ? inv : acc;
            boot_store_snapshot_t expected=phase==0u ? old : invalid;
            for (call=1; call<=50u; ++call)
            {
                unsigned bits;
                for (bits=0; bits<=32u; bits+=16u)
                {
                    flash_model_t m=base;
                    boot_store_snapshot_t out, selected;
                    boot_store_adapter_t cut=flash_model_adapter(&m);
                    m.calls=0u; m.cut_call=call; m.cut_bits=bits;
                    CHECK(boot_store_replace(&cut,&expected,&proposal,&out)!=BOOT_STORE_OK);
                    m.cut_call=0u; m.stopped=0;
                    CHECK(boot_store_load(&cut,&selected)==BOOT_STORE_OK);
                    CHECK(memcmp(&selected.record,&expected.record,sizeof r)==0 ||
                          memcmp(&selected.record,&proposal,sizeof r)==0);
                    CHECK(memcmp(&selected.record.images[slot],&r.images[slot],sizeof r.images[slot])==0);
                    CHECK(selected.record.sequence==((call>47u || (call==47u && bits==32u)) ?
                          proposal.sequence : expected.record.sequence));
                    /* Durable authority, not RAM proposal, describes eligibility. */
                    if (phase==0u) CHECK(selected.record.images[dest].payload_length==
                        (selected.record.sequence==proposal.sequence ? 0u : 236u));
                    else CHECK(selected.record.candidate_slot==
                        (selected.record.sequence==proposal.sequence ? dest : BOOT_SLOT_NONE));
                }
            }
            base.calls=0u;
            if (phase==0u)
            {
                CHECK(boot_store_replace(&a,&old,&inv,&invalid)==BOOT_STORE_OK);
                CHECK(boot_lifecycle_accept_ready(&invalid.record,&e,&v,&acc)==BOOT_LIFECYCLE_CHANGE);
            }
            else CHECK(boot_store_replace(&a,&invalid,&acc,&ready)==BOOT_STORE_OK);
        }
        /* Pure acceptance cannot see stale flash; store detects the race. */
        CHECK(boot_lifecycle_accept_ready(&invalid.record,&e,&v,&acc)==BOOT_LIFECYCLE_CHANGE);
        base.erases=base.writes=0u;
        CHECK(boot_store_replace(&a,&invalid,&acc,&old)==BOOT_STORE_STALE);
        CHECK(base.erases==0u && base.writes==0u);
    }
}
static boot_metadata_t execution_fixture(unsigned slot, boot_verified_image_t *v)
{
    boot_metadata_t r=fixture(slot), invalid, ready;
    boot_upload_evidence_t e=evidence(&r);
    *v=stored_image(&e);
    CHECK(boot_lifecycle_invalidate(&r,&e,&invalid)==BOOT_LIFECYCLE_CHANGE);
    CHECK(boot_lifecycle_accept_ready(&invalid,&e,v,&ready)==BOOT_LIFECYCLE_CHANGE);
    return ready;
}
static void execution_negatives(void)
{
    unsigned slot, op, mode;
    for (slot=0; slot<2u; ++slot)
    for (op=0; op<3u; ++op)
    for (mode=0; mode<25u; ++mode)
    {
        boot_verified_image_t v;
        boot_metadata_t r=execution_fixture(slot,&v), out, sentinel;
        unsigned dest=1u-slot;
        boot_confirm_request_t q={0};
        boot_deadline_request_t d={dest,102u,BOOT_REASON_DEADLINE};
        boot_lifecycle_result_t result;
        q.slot=dest; q.trial_id=102u; memcpy(q.payload_hash,v.manifest.payload_hash,32u);
        if (op!=0u) r.images[dest].accounting=BOOT_STATE_TRIAL;
        memset(&sentinel,0xa5,sizeof sentinel); out=sentinel;
        switch (mode)
        {
        case 0: v.manifest.payload_hash[0]++; break;
        case 1: v.manifest.release++; break;
        case 2: v.manifest.payload_length+=4u; break;
        case 3: v.manifest.execution_base++; break;
        case 4: v.initial_msp--; break;
        case 5: v.reset_vector &= ~UINT32_C(1); break;
        case 6: v.reset_vector=v.manifest.execution_base-1u; break;
        case 7: v.reset_vector=v.manifest.execution_base+237u; break;
        case 8: r.sequence=UINT32_MAX; break;
        case 9: r.confirmed_mask=4u; break;
        case 10: q.slot=slot; d.slot=slot; if (op==0u) v.manifest.slot=slot; break;
        case 11: q.trial_id--; d.trial_id--; if (op==0u) r.images[dest].trial_id=103u; break;
        case 12: q.payload_hash[31]++; d.reason=BOOT_REASON_WATCHDOG;
            if (op==0u) r.candidate_slot=BOOT_SLOT_NONE; break;
        case 13: r.images[dest].accounting=BOOT_STATE_READY;
            if (op==0u) r.images[dest].accounting=BOOT_STATE_NONE; break;
        case 14: r.images[dest].accounting=history(3u,0u); r.candidate_slot=BOOT_SLOT_NONE; break;
        case 15: q.slot=BOOT_SLOT_NONE; d.slot=BOOT_SLOT_NONE;
            if (op==0u) v.manifest.payload_length=UINT32_MAX; break;
        case 16: r.intent_kind=BOOT_INTENT_DEADLINE; r.intent_slot=dest;
            r.intent_trial_id=101u; break;
        default: break;
        }
        if (mode>=17u && op==0u) continue;
        if (mode>=17u && op==2u) continue;
        result=op==0u ? boot_lifecycle_begin_trial(&r,&v,&out) : op==1u ?
            boot_lifecycle_confirm(&r,&q,&v,mode==24u ? 15u : mode>=17u ? mode-17u : 7u,&out) :
            boot_lifecycle_deadline_intent(&r,&d,&v,&out);
        CHECK(result!=BOOT_LIFECYCLE_CHANGE && result!=BOOT_LIFECYCLE_NO_CHANGE);
        CHECK(memcmp(&out,&sentinel,sizeof out)==0);
    }
    for (slot=0; slot<2u; ++slot)
    {
        boot_verified_image_t v;
        boot_metadata_t r=execution_fixture(slot,&v), out, before;
        boot_confirm_request_t q={0};
        boot_deadline_request_t d={1u-slot,102u,BOOT_REASON_DEADLINE};
        q.slot=1u-slot; q.trial_id=102u; memcpy(q.payload_hash,v.manifest.payload_hash,32u);
        r.images[1u-slot].accounting=BOOT_STATE_TRIAL; r.sequence=UINT32_MAX;
        CHECK(boot_lifecycle_begin_trial(&r,&v,&out)==BOOT_LIFECYCLE_NO_CHANGE);
        CHECK(boot_lifecycle_confirm(&r,&q,&v,7u,&out)==BOOT_LIFECYCLE_ERROR_EXHAUSTED);
        r.intent_kind=BOOT_INTENT_DEADLINE; r.intent_slot=1u-slot; r.intent_trial_id=102u;
        CHECK(boot_lifecycle_deadline_intent(&r,&d,&v,&out)==BOOT_LIFECYCLE_NO_CHANGE);
        r.images[1u-slot].trial_id=UINT32_MAX; r.intent_trial_id=UINT32_MAX;
        d.trial_id=UINT32_MAX; q.trial_id=UINT32_MAX;
        CHECK(boot_lifecycle_begin_trial(&r,&v,&out)==BOOT_LIFECYCLE_NO_CHANGE);
        CHECK(boot_lifecycle_deadline_intent(&r,&d,&v,&out)==BOOT_LIFECYCLE_NO_CHANGE);
        before=r;
        CHECK(boot_lifecycle_begin_trial(&r,&v,&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_confirm(&r,&q,&v,7u,&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_deadline_intent(&r,&d,&v,&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(memcmp(&r,&before,sizeof r)==0);
        CHECK(boot_lifecycle_begin_trial(NULL,&v,&out)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_begin_trial(&r,NULL,&out)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_begin_trial(&r,&v,NULL)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_begin_trial(&r,&v,(boot_metadata_t *)(void *)&v)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_confirm(&r,NULL,&v,7u,&out)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_confirm(&r,&q,&v,7u,(boot_metadata_t *)(void *)&q)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_deadline_intent(&r,NULL,&v,&out)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_deadline_intent(&r,&d,&v,(boot_metadata_t *)(void *)&d)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        /* Factory zero retained ID is a valid duplicate, not a new trial. */
        r=fixture(slot); v=evidence(&r).running;
        q.slot=slot; q.trial_id=0u; memcpy(q.payload_hash,v.manifest.payload_hash,32u);
        CHECK(boot_lifecycle_confirm(&r,&q,&v,UINT32_MAX,&out)==BOOT_LIFECYCLE_NO_CHANGE);
        r.sequence=UINT32_MAX; r.images[slot].trial_id=UINT32_MAX; q.trial_id=UINT32_MAX;
        CHECK(boot_lifecycle_confirm(&r,&q,&v,0u,&out)==BOOT_LIFECYCLE_NO_CHANGE);
        q.trial_id=0u;
        CHECK(boot_lifecycle_confirm(&r,&q,&v,7u,&out)==BOOT_LIFECYCLE_ERROR_IDENTITY);
    }
}
static void execution_histories(void)
{
    unsigned slot, ac, bc, a, b;
    for (slot=0;slot<2u;++slot)
    for (ac=0;ac<3u;++ac) for (a=0;a<(ac==0u?1u:ac==1u?5u:25u);++a)
    for (bc=0;bc<3u;++bc) for (b=0;b<(bc==0u?1u:bc==1u?5u:25u);++b)
    {
        boot_verified_image_t v;
        boot_metadata_t r=execution_fixture(slot,&v), out, expected;
        boot_confirm_request_t q={0};
        unsigned dest=1u-slot;
        r.images[slot].accounting=history(ac,a); r.images[slot].trial_id=10u;
        r.images[dest].accounting=history(bc,b)|BOOT_STATE_TRIAL;
        q.slot=dest; q.trial_id=102u; memcpy(q.payload_hash,v.manifest.payload_hash,32u);
        expected=r; expected.sequence=103u; expected.current_slot=dest;
        expected.previous_slot=slot; expected.candidate_slot=BOOT_SLOT_NONE;
        expected.confirmed_mask=3u; expected.images[dest].accounting=history(bc,b);
        CHECK(boot_lifecycle_confirm(&r,&q,&v,7u,&out)==BOOT_LIFECYCLE_CHANGE);
        CHECK(memcmp(&out,&expected,sizeof out)==0);
    }
}
/* Integration caller: branch on NO_CHANGE before invoking the writer. The
 * loaded snapshot is the durable authority, not a cached proposal. */
static void no_write(flash_model_t *m, const boot_store_snapshot_t *snapshot,
    boot_lifecycle_result_t result, const boot_metadata_t *out)
{
    boot_store_adapter_t adapter=flash_model_adapter(m);
    boot_store_snapshot_t committed;
    size_t erases=m->erases, writes=m->writes;
    CHECK(result==BOOT_LIFECYCLE_NO_CHANGE);
    CHECK(memcmp(out,&snapshot->record,sizeof *out)==0);
    if (result==BOOT_LIFECYCLE_CHANGE)
        CHECK(boot_store_replace(&adapter,snapshot,out,&committed)==BOOT_STORE_OK);
    CHECK(m->erases==erases && m->writes==writes);
}
/* Confirmation follows the retained running identity, not its current role.
 * Derive previous from real confirmation; the roleless variant is a codec-valid
 * retained-entry fixture (cleared role does not clear history/confirmation),
 * not a new recovery policy or a claim that these helpers clear previous. */
static void confirmed_role_duplicates(void)
{
    unsigned slot, roleless, id, page, health;
    const uint32_t health_values[]={0u, BOOT_HEALTH_REQUIRED, UINT32_MAX};
    for (slot=0u;slot<2u;++slot)
    for (roleless=0u;roleless<2u;++roleless)
    for (id=0u;id<3u;++id)
    for (page=0u;page<2u;++page)
    {
        boot_verified_image_t candidate, running;
        boot_metadata_t trial=execution_fixture(slot,&candidate), r, out, sentinel;
        boot_confirm_request_t q={0};
        flash_model_t m;
        boot_store_adapter_t adapter;
        boot_store_snapshot_t snapshot;
        running=evidence(&trial).running;
        trial.images[1u-slot].accounting=BOOT_STATE_TRIAL;
        q.slot=1u-slot; q.trial_id=trial.images[1u-slot].trial_id;
        memcpy(q.payload_hash,candidate.manifest.payload_hash,32u);
        CHECK(boot_lifecycle_confirm(&trial,&q,&candidate,BOOT_HEALTH_REQUIRED,&r)==BOOT_LIFECYCLE_CHANGE);
        CHECK(r.previous_slot==slot && r.current_slot==1u-slot);
        if (roleless) r.previous_slot=BOOT_SLOT_NONE;
        if (id!=0u) r.images[slot].trial_id=id==1u ? 10u : UINT32_MAX;
        if (id==2u) r.sequence=UINT32_MAX;
        CHECK(boot_metadata_validate(&r)==BOOT_OK);
        flash_model_init(&m);
        CHECK(boot_metadata_encode(&r,0u,m.bytes+page*1024u,168u)==BOOT_OK);
        adapter=flash_model_adapter(&m);
        CHECK(boot_store_load(&adapter,&snapshot)==BOOT_STORE_OK);
        CHECK(memcmp(&r,&snapshot.record,sizeof r)==0);
        q.slot=slot; q.trial_id=r.images[slot].trial_id;
        memcpy(q.payload_hash,running.manifest.payload_hash,32u);
        for (health=0u;health<3u;++health)
        {
            boot_lifecycle_result_t result=boot_lifecycle_confirm(&snapshot.record,&q,
                &running,health_values[health],&out);
            no_write(&m,&snapshot,result,&out);
            CHECK(m.erases==0u && m.writes==0u);
        }
        memset(&sentinel,0xa5,sizeof sentinel); out=sentinel;
        q.trial_id^=1u;
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,&out)==BOOT_LIFECYCLE_ERROR_IDENTITY);
        q.trial_id^=1u; q.slot=1u-slot;
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,&out)==BOOT_LIFECYCLE_ERROR_IDENTITY);
        q.slot=slot; q.payload_hash[0]^=1u;
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,&out)==BOOT_LIFECYCLE_ERROR_IDENTITY);
        q.payload_hash[0]^=1u; running.initial_msp--;
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,&out)==BOOT_LIFECYCLE_ERROR_IMAGE);
        running.initial_msp++; running.manifest.release++;
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,&out)==BOOT_LIFECYCLE_ERROR_IDENTITY);
        running.manifest.release--;
        CHECK(memcmp(&out,&sentinel,sizeof out)==0);
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(memcmp(&r,&snapshot.record,sizeof r)==0);
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,(boot_metadata_t *)(void *)&q)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_confirm(&r,&q,&running,0u,(boot_metadata_t *)(void *)&running)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        /* A retained NONE entry without confirmation is not a duplicate. */
        r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask &= ~(UINT32_C(1)<<slot);
        CHECK(boot_metadata_validate(&r)==BOOT_OK);
        CHECK(boot_lifecycle_confirm(&r,&q,&running,BOOT_HEALTH_REQUIRED,&out)==BOOT_LIFECYCLE_ERROR_ADMISSION);
        CHECK(memcmp(&out,&sentinel,sizeof out)==0);
    }
}
static void execution_persistence(void)
{
    unsigned slot, page, phase;
    for (slot=0;slot<2u;++slot) for (page=0;page<2u;++page)
    {
        boot_verified_image_t v;
        boot_metadata_t ready=execution_fixture(slot,&v);
        boot_confirm_request_t q={0};
        boot_deadline_request_t d={1u-slot,102u,BOOT_REASON_DEADLINE};
        flash_model_t base;
        boot_store_adapter_t adapter;
        boot_store_snapshot_t snapshot;
        q.slot=1u-slot; q.trial_id=102u; memcpy(q.payload_hash,v.manifest.payload_hash,32u);
        flash_model_init(&base);
        CHECK(boot_metadata_encode(&ready,0u,base.bytes+page*1024u,168u)==BOOT_OK);
        adapter=flash_model_adapter(&base);
        CHECK(boot_store_load(&adapter,&snapshot)==BOOT_STORE_OK);
        for (phase=0;phase<3u;++phase)
        {
            boot_metadata_t proposal, duplicate;
            boot_lifecycle_result_t result;
            size_t call;
            result=phase==0u ? boot_lifecycle_begin_trial(&snapshot.record,&v,&proposal) :
                phase==1u ? boot_lifecycle_deadline_intent(&snapshot.record,&d,&v,&proposal) :
                boot_lifecycle_confirm(&snapshot.record,&q,&v,7u,&proposal);
            CHECK(result==BOOT_LIFECYCLE_CHANGE);
            for (call=1u;call<=50u;++call)
            {
                unsigned bits;
                for (bits=0u;bits<=32u;bits+=16u)
                {
                    flash_model_t m=base;
                    boot_store_adapter_t cut=flash_model_adapter(&m);
                    boot_store_snapshot_t out, selected;
                    m.calls=0u; m.cut_call=call; m.cut_bits=bits;
                    CHECK(boot_store_replace(&cut,&snapshot,&proposal,&out)!=BOOT_STORE_OK);
                    m.cut_call=0u; m.stopped=0;
                    CHECK(boot_store_load(&cut,&selected)==BOOT_STORE_OK);
                    CHECK(memcmp(&selected.record,&snapshot.record,sizeof ready)==0 ||
                          memcmp(&selected.record,&proposal,sizeof ready)==0);
                    CHECK(selected.record.sequence==((call>47u || (call==47u && bits==32u)) ?
                        proposal.sequence : snapshot.record.sequence));
                    CHECK(memcmp(&selected.record.images[slot],&ready.images[slot],sizeof ready.images[slot])==0);
                    CHECK(selected.record.images[1u-slot].trial_id==102u);
                    /* Never any app-side failure count increment. */
                    CHECK((selected.record.images[1u-slot].accounting & ~BOOT_ACCOUNT_STATE_MASK)==0u);
                }
            }
            CHECK(boot_store_replace(&adapter,&snapshot,&proposal,&snapshot)==BOOT_STORE_OK);
            result=phase==0u ? boot_lifecycle_begin_trial(&snapshot.record,&v,&duplicate) :
                phase==1u ? boot_lifecycle_deadline_intent(&snapshot.record,&d,&v,&duplicate) :
                boot_lifecycle_confirm(&snapshot.record,&q,&v,0u,&duplicate);
            no_write(&base,&snapshot,result,&duplicate);
        }
        /* Exhaustion prohibits mutations, not validated no-write outcomes.
         * Model durable max-sequence snapshots independently for each helper. */
        for (phase=0;phase<3u;++phase)
        {
            flash_model_t max;
            boot_store_adapter_t max_adapter;
            boot_store_snapshot_t max_snapshot;
            boot_metadata_t r=ready, out;
            boot_lifecycle_result_t result;
            r.sequence=UINT32_MAX; r.images[1u-slot].trial_id=UINT32_MAX;
            r.images[1u-slot].accounting=BOOT_STATE_TRIAL;
            if (phase==1u)
            {
                r.intent_kind=BOOT_INTENT_DEADLINE; r.intent_slot=1u-slot;
                r.intent_trial_id=UINT32_MAX;
            }
            if (phase==2u)
            {
                r.previous_slot=slot; r.current_slot=1u-slot;
                r.candidate_slot=BOOT_SLOT_NONE; r.confirmed_mask=3u;
                r.images[1u-slot].accounting=BOOT_STATE_NONE;
            }
            flash_model_init(&max);
            CHECK(boot_metadata_encode(&r,0u,max.bytes+page*1024u,168u)==BOOT_OK);
            max_adapter=flash_model_adapter(&max);
            CHECK(boot_store_load(&max_adapter,&max_snapshot)==BOOT_STORE_OK);
            d.trial_id=UINT32_MAX; q.trial_id=UINT32_MAX;
            result=phase==0u ? boot_lifecycle_begin_trial(&max_snapshot.record,&v,&out) :
                phase==1u ? boot_lifecycle_deadline_intent(&max_snapshot.record,&d,&v,&out) :
                boot_lifecycle_confirm(&max_snapshot.record,&q,&v,0u,&out);
            no_write(&max,&max_snapshot,result,&out);
            CHECK(max.erases==0u && max.writes==0u);
            if (phase==1u)
            {
                d.trial_id--;
                CHECK(boot_lifecycle_deadline_intent(&r,&d,&v,&out)==BOOT_LIFECYCLE_ERROR_IDENTITY);
                v.initial_msp--;
                d.trial_id=UINT32_MAX;
                CHECK(boot_lifecycle_deadline_intent(&r,&d,&v,&out)==BOOT_LIFECYCLE_ERROR_IMAGE);
                v.initial_msp++;
            }
        }
        d.trial_id=102u;
        /* A confirmed identity can no longer propose even an old deadline. */
        {
            boot_metadata_t out;
            CHECK(boot_lifecycle_deadline_intent(&snapshot.record,&d,&v,&out)==BOOT_LIFECYCLE_ERROR_ADMISSION);
        }
    }
}
static boot_resident_identity_t resident(const boot_metadata_t *r, unsigned slot)
{
    boot_resident_identity_t id={0};
    id.slot=slot; id.release=r->images[slot].release;
    id.payload_length=r->images[slot].payload_length;
    id.trial_id=r->images[slot].trial_id;
    memcpy(id.payload_hash,r->images[slot].payload_hash,32u);
    return id;
}
static boot_metadata_t recovery_fixture(unsigned slot, boot_verified_image_t *previous,
    boot_invalid_current_t *invalid)
{
    boot_metadata_t r=fixture(slot);
    boot_upload_evidence_t e=evidence(&r);
    *previous=stored_image(&e);
    r.images[1u-slot]=r.images[slot];
    memcpy(r.images[1u-slot].payload_hash,previous->manifest.payload_hash,32u);
    r.images[1u-slot].release=previous->manifest.release;
    r.images[1u-slot].trial_id=20u;
    r.previous_slot=1u-slot; r.confirmed_mask=3u;
    invalid->identity=resident(&r,slot); invalid->failure=BOOT_VERIFY_ERROR_HASH;
    return r;
}
static unsigned combinations(unsigned count)
{
    return count==0u ? 1u : count==1u ? 5u : count==2u ? 25u : 125u;
}
static void recovery_matrices(void)
{
    unsigned slot, ac, bc, a, b, reason, intent, consume;
    for (slot=0u;slot<2u;++slot)
    for (ac=0u;ac<3u;++ac) for (a=0u;a<combinations(ac);++a)
    for (bc=0u;bc<3u;++bc) for (b=0u;b<combinations(bc);++b)
    {
        boot_verified_image_t v;
        boot_invalid_current_t invalid;
        boot_metadata_t r=recovery_fixture(slot,&v,&invalid), out, expected;
        r.images[slot].accounting=history(ac,a);
        r.images[1u-slot].accounting=history(bc,b);
        expected=r; expected.sequence++; expected.current_slot=1u-slot;
        expected.previous_slot=BOOT_SLOT_NONE; expected.confirmed_mask=UINT32_C(1)<<(1u-slot);
        CHECK(boot_lifecycle_promote_previous(&r,&invalid,&v,&out)==BOOT_LIFECYCLE_CHANGE);
        CHECK(memcmp(&out,&expected,sizeof out)==0);
        CHECK(boot_metadata_validate(&out)==BOOT_OK);
        for (reason=1u;reason<=5u;++reason)
        for (intent=0u;intent<2u;++intent)
        for (consume=0u;consume<2u;++consume)
        {
            boot_counted_failure_t failure;
            r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask=UINT32_C(1)<<slot;
            r.candidate_slot=1u-slot; r.images[1u-slot].accounting=history(bc,b)|BOOT_STATE_TRIAL;
            r.intent_kind=intent ? BOOT_INTENT_DEADLINE : BOOT_INTENT_NONE;
            r.intent_slot=intent ? 1u-slot : BOOT_SLOT_NONE;
            r.intent_trial_id=intent ? 20u : 0u;
            failure.identity=resident(&r,1u-slot); failure.reason=reason;
            failure.consume_intent=consume;
            expected=r; expected.sequence++;
            expected.images[1u-slot].accounting=
                (history(bc,b) & ~(UINT32_C(3)<<2)) | ((bc+1u)<<2) |
                (reason<<(5u+3u*bc)) | (bc==2u ? BOOT_STATE_REJECTED|BOOT_ACCOUNT_ROLLBACK : BOOT_STATE_TRIAL);
            if (bc==2u) expected.candidate_slot=BOOT_SLOT_NONE;
            if (consume || bc==2u)
            {
                expected.intent_kind=BOOT_INTENT_NONE; expected.intent_slot=BOOT_SLOT_NONE;
                expected.intent_trial_id=0u;
            }
            CHECK(boot_lifecycle_apply_counted_failure(&r,&failure,&out)==BOOT_LIFECYCLE_CHANGE);
            CHECK(memcmp(&out,&expected,sizeof out)==0);
            CHECK(boot_metadata_validate(&out)==BOOT_OK);
        }
    }
}
static void recovery_negatives(void)
{
    unsigned slot, op, mode;
    for (slot=0u;slot<2u;++slot) for (op=0u;op<2u;++op)
    for (mode=0u;mode<31u;++mode)
    {
        boot_verified_image_t v;
        boot_invalid_current_t invalid;
        boot_counted_failure_t failure;
        boot_metadata_t r=recovery_fixture(slot,&v,&invalid), sentinel, out;
        boot_resident_identity_t *id=op ? &invalid.identity : &failure.identity;
        boot_lifecycle_result_t result;
        if (!op)
        {
            r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask=UINT32_C(1)<<slot;
            r.candidate_slot=1u-slot; r.images[1u-slot].accounting=BOOT_STATE_TRIAL;
            failure.identity=resident(&r,1u-slot); failure.reason=BOOT_REASON_WATCHDOG;
            failure.consume_intent=0u;
        }
        memset(&sentinel,0xa5,sizeof sentinel); out=sentinel;
        switch (mode)
        {
        case 0: id->slot=BOOT_SLOT_NONE; break;
        case 1: id->slot=1u-id->slot; break;
        case 2: id->payload_hash[31]++; break;
        case 3: id->release++; break;
        case 4: id->payload_length+=4u; break;
        case 5: id->trial_id++; break;
        case 6: r.sequence=UINT32_MAX; break;
        case 7: r.confirmed_mask=4u; break;
        case 8: if (op) invalid.failure=BOOT_VERIFY_OK; else failure.reason=0u; break;
        case 9: if (op) invalid.failure=BOOT_VERIFY_ERROR_ARGUMENT; else failure.reason=6u; break;
        case 10: if (op) invalid.failure=BOOT_VERIFY_ERROR_CRYPTO; else failure.reason=7u; break;
        case 11: if (op) invalid.failure=BOOT_VERIFY_ERROR_READ; else failure.reason=UINT32_MAX; break;
        case 12: if (op) invalid.failure=BOOT_VERIFY_ERROR_ABORTED; else failure.consume_intent=2u; break;
        case 13: if (op) r.previous_slot=BOOT_SLOT_NONE; else r.images[1u-slot].accounting=BOOT_STATE_READY; break;
        case 14: if (op) r.current_slot=BOOT_SLOT_NONE; else
            { r.images[1u-slot].accounting=BOOT_STATE_NONE; r.candidate_slot=BOOT_SLOT_NONE; r.confirmed_mask=3u; } break;
        case 15: if (op) { r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask=UINT32_C(1)<<slot;
            r.candidate_slot=1u-slot; r.images[1u-slot].accounting=BOOT_STATE_READY; }
            else { r.images[1u-slot].accounting=history(3u,0u); r.candidate_slot=BOOT_SLOT_NONE; } break;
        case 16: if (op) v.initial_msp--; else r.intent_kind=2u; break;
        case 17: if (op) v.manifest.payload_hash[0]++; else r.images[1u-slot].trial_id=101u; break;
        case 18: if (op) v.manifest.release++; else r.images[1u-slot].accounting|=UINT32_C(1)<<14; break;
        case 19: if (op) v.manifest.payload_length+=4u; else r.intent_kind=BOOT_INTENT_DEADLINE; break;
        case 20: if (op) { r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask=UINT32_C(1)<<slot;
            r.candidate_slot=1u-slot; r.images[1u-slot].accounting=BOOT_STATE_TRIAL; }
            else r.candidate_slot=BOOT_SLOT_NONE; break;
        case 25: if (op) invalid.failure=(boot_verify_result_t)UINT32_MAX;
            else r.images[1u-slot].accounting=BOOT_STATE_NONE; break;
        case 26: if (op) v.reset_vector &= ~UINT32_C(1);
            else failure.consume_intent=UINT32_MAX; break;
        case 27: if (op) v.reset_vector=v.manifest.execution_base-1u;
            else { r.intent_kind=BOOT_INTENT_DEADLINE; r.intent_slot=1u-slot; r.intent_trial_id=19u; } break;
        case 28: if (op) v.reset_vector=v.manifest.execution_base+237u;
            else { r.images[1u-slot].accounting=BOOT_STATE_NONE; r.candidate_slot=BOOT_SLOT_NONE; } break;
        case 29: if (op) { r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask=UINT32_C(1)<<slot;
            memset(&r.images[1u-slot],0,sizeof r.images[1u-slot]); }
            else id->slot=2u; break;
        case 30: if (op) v.manifest.execution_base++;
            else r.current_slot=2u; break;
        default: break;
        }
        result=op ? boot_lifecycle_promote_previous(mode==21u ? NULL : &r,
            mode==22u ? NULL : &invalid,mode==23u ? NULL : &v,mode==24u ? NULL : &out) :
            boot_lifecycle_apply_counted_failure(mode==21u ? NULL : &r,
                mode==22u ? NULL : &failure,mode==23u ? &r : mode==24u ? NULL : &out);
        CHECK(result!=BOOT_LIFECYCLE_CHANGE && result!=BOOT_LIFECYCLE_NO_CHANGE);
        CHECK(memcmp(&out,&sentinel,sizeof out)==0);
    }
    for (slot=0u;slot<2u;++slot)
    {
        boot_verified_image_t v;
        boot_invalid_current_t invalid;
        boot_metadata_t r=recovery_fixture(slot,&v,&invalid), out, before=r;
        const boot_verify_result_t failures[]={BOOT_VERIFY_ERROR_FORMAT,BOOT_VERIFY_ERROR_TARGET,
            BOOT_VERIFY_ERROR_SIGNATURE,BOOT_VERIFY_ERROR_HASH,BOOT_VERIFY_ERROR_MSP,
            BOOT_VERIFY_ERROR_THUMB,BOOT_VERIFY_ERROR_VECTOR_RANGE};
        size_t i;
        for (i=0u;i<sizeof failures/sizeof failures[0];++i)
        {
            invalid.failure=failures[i];
            CHECK(boot_lifecycle_promote_previous(&r,&invalid,&v,&out)==BOOT_LIFECYCLE_CHANGE);
        }
        CHECK(boot_lifecycle_promote_previous(&r,&invalid,&v,&r)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(memcmp(&r,&before,sizeof r)==0);
        CHECK(boot_lifecycle_promote_previous(&r,&invalid,&v,(boot_metadata_t *)(void *)&invalid)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        CHECK(boot_lifecycle_promote_previous(&r,&invalid,&v,(boot_metadata_t *)(void *)&v)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
        r.sequence=UINT32_MAX-1u;
        CHECK(boot_lifecycle_promote_previous(&r,&invalid,&v,&out)==BOOT_LIFECYCLE_CHANGE);
        CHECK(out.sequence==UINT32_MAX);
        {
            boot_counted_failure_t f;
            r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask=UINT32_C(1)<<slot;
            r.candidate_slot=1u-slot; r.images[1u-slot].accounting=BOOT_STATE_TRIAL;
            f.identity=resident(&r,1u-slot); f.reason=BOOT_REASON_COMBINED; f.consume_intent=1u;
            CHECK(boot_lifecycle_apply_counted_failure(&r,&f,(boot_metadata_t *)(void *)&f)==BOOT_LIFECYCLE_ERROR_ARGUMENT);
            CHECK(boot_lifecycle_apply_counted_failure(&r,&f,&out)==BOOT_LIFECYCLE_CHANGE);
            CHECK(out.sequence==UINT32_MAX);
        }
    }
}
static void recovery_persistence(void)
{
    unsigned slot, page, op;
    for (slot=0u;slot<2u;++slot) for (page=0u;page<2u;++page) for (op=0u;op<2u;++op)
    {
        boot_verified_image_t v;
        boot_invalid_current_t invalid;
        boot_counted_failure_t failure;
        boot_metadata_t r=recovery_fixture(slot,&v,&invalid), proposal;
        flash_model_t base;
        boot_store_adapter_t adapter;
        boot_store_snapshot_t snapshot, committed;
        size_t call;
        if (!op)
        {
            r.previous_slot=BOOT_SLOT_NONE; r.confirmed_mask=UINT32_C(1)<<slot;
            r.candidate_slot=1u-slot; r.images[1u-slot].accounting=history(2u,24u)|BOOT_STATE_TRIAL;
            r.intent_kind=BOOT_INTENT_DEADLINE; r.intent_slot=1u-slot; r.intent_trial_id=20u;
            failure.identity=resident(&r,1u-slot); failure.reason=BOOT_REASON_COMBINED; failure.consume_intent=1u;
        }
        CHECK((op ? boot_lifecycle_promote_previous(&r,&invalid,&v,&proposal) :
            boot_lifecycle_apply_counted_failure(&r,&failure,&proposal))==BOOT_LIFECYCLE_CHANGE);
        flash_model_init(&base);
        CHECK(boot_metadata_encode(&r,0u,base.bytes+page*1024u,168u)==BOOT_OK);
        adapter=flash_model_adapter(&base);
        CHECK(boot_store_load(&adapter,&snapshot)==BOOT_STORE_OK);
        CHECK(base.erases==0u && base.writes==0u);
        for (call=1u;call<=50u;++call)
        {
            unsigned bits;
            for (bits=0u;bits<=32u;bits+=16u)
            {
                flash_model_t m=base;
                boot_store_adapter_t cut=flash_model_adapter(&m);
                boot_store_snapshot_t out, selected;
                m.calls=0u; m.cut_call=call; m.cut_bits=bits;
                CHECK(boot_store_replace(&cut,&snapshot,&proposal,&out)!=BOOT_STORE_OK);
                m.cut_call=0u; m.stopped=0;
                CHECK(boot_store_load(&cut,&selected)==BOOT_STORE_OK);
                CHECK(memcmp(&selected.record,&r,sizeof r)==0 ||
                    memcmp(&selected.record,&proposal,sizeof r)==0);
                CHECK(selected.record.sequence==((call>47u || (call==47u && bits==32u)) ?
                    proposal.sequence : r.sequence));
                /* Reload is required even if commit succeeded but read failed;
                 * no fallback execution is performed by this test or helper. */
            }
        }
        CHECK(boot_store_replace(&adapter,&snapshot,&proposal,&committed)==BOOT_STORE_OK);
        CHECK(memcmp(&committed.record,&proposal,sizeof proposal)==0);
        base.erases=base.writes=0u;
        CHECK(boot_store_replace(&adapter,&snapshot,&proposal,&committed)==BOOT_STORE_STALE);
        CHECK(base.erases==0u && base.writes==0u);
    }
}
int main(void)
{
    matrix(); negatives(); persistence(); execution_negatives(); execution_histories(); confirmed_role_duplicates(); execution_persistence();
    recovery_matrices(); recovery_negatives(); recovery_persistence();
    printf("boot lifecycle: %u checks passed\n",checks); return 0;
}
