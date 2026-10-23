/* Included only by sequence_host.c: authentic sequence/adapter integration.
 * All large arenas have static storage; these are modeled registers, not MMIO.
 * No signing, fabricated controller state, or nonreturning app execution. */
static boot_handoff_target_t target;
static boot_handoff_t handoff;
static boot_handoff_context_t observed;
static boot_sequence_launch_parameters_t launch_params;
static boot_runtime_watchdog_launch_t launch_out, launch_sentinel;
static unsigned launching, published, transfers, adapter_calls, adapter_writes;
static int checked_is_launching(void) { return launching!=0; }
static unsigned fault_kind, nested_checks, mutate_inputs, mutated, wdt_at_capture;
static uint32_t enabled[2], pending[2], active[2], icsr, systick, vtor;
static struct { char op; uint32_t a, v; } adapter_trace[4096];
static unsigned an, operation_count, terminal_count, fault_end_an, fault_end_n;
static unsigned operation_values[16];
static void checked_write_boundary(void) {
    /* A retained clock read during adapter/prepare may only read. */
    if (launching && an) CHECK(0);
}
static int operation_forward(void *ctx,boot_handoff_operation_t op,uint32_t value) {
    CHECK(ctx==&target && operation_count<16);
    CHECK((unsigned)op==operation_count);
    operation_values[operation_count++]=value;
    return boot_handoff_target_operation(ctx,op,value);
}
static int terminal_forward(void *ctx,const boot_handoff_intent_t *intent) {
    CHECK(ctx==&target); ++terminal_count;
    CHECK(c.watchdog.owner==BOOT_WATCHDOG_APPLICATION && c.watchdog.epoch==4);
    return boot_handoff_target_terminal_intent(ctx,intent);
}
static void ae(char op, uint32_t a, uint32_t v) {
    CHECK(an < 4096); adapter_trace[an].op=op;
    adapter_trace[an].a=a; adapter_trace[an++].v=v;
}
static boot_sequence_result_t launch_invoke(void) {
    return boot_sequence_launch(&seq,&launch_params,&launch_out);
}
static void checked_boundary(void) {
    if (!launching) return;
    if (!published) CHECK(!memcmp(&launch_out,&launch_sentinel,sizeof launch_out));
    if (check_guards && !inside && !published) {
        inside=1;
        unsigned a=calls,b=resc_reads,d=resc_writes,e=payload_reads,f=an;
        size_t g=flash.calls;
        boot_sequence_result_t r=invoke();
        CHECK(r.status==BOOT_SEQUENCE_BUSY && r.stage==BOOT_SEQUENCE_NONE);
        r=launch_invoke();
        CHECK(r.status==BOOT_SEQUENCE_BUSY && r.stage==BOOT_SEQUENCE_NONE);
        CHECK(a==calls && b==resc_reads && d==resc_writes && e==payload_reads && f==an && g==flash.calls);
        CHECK(!memcmp(&launch_out,&launch_sentinel,sizeof launch_out));
        ++nested_checks; inside=0;
    }
}
static void checked_reset(void) {
    if (launching && mutate_inputs && !mutated && resc_reads==0) {
        /* Only original input copies change AFTER admission/snapshot. */
        launch_params.common.actual_hz=0;
        launch_params.observed=NULL; launch_params.handoff=NULL;
        observed.primask=0; mutated=1;
    }
}
static uint32_t model_read(uint32_t a) {
    if(a==0xe000e180u || a==0xe000e184u) return enabled[(a-0xe000e180u)/4];
    if(a==0xe000e280u || a==0xe000e284u) return pending[(a-0xe000e280u)/4];
    if(a==0xe000e300u || a==0xe000e304u) return active[(a-0xe000e300u)/4];
    if(a==0xe000ed04u) return icsr & ~0x0a000000u; /* WO clear bits */
    if(a==0xe000ed08u) return vtor;
    CHECK(a==0xe000e010u); return systick;
}
static void model_write(uint32_t a,uint32_t v) {
    if(a==0xe000e180u || a==0xe000e184u) enabled[(a-0xe000e180u)/4]&=~v;
    else if(a==0xe000e280u || a==0xe000e284u) pending[(a-0xe000e280u)/4]&=~v;
    else if(a==0xe000ed04u) {
        if(v&0x08000000u) icsr&=~0x10000000u;
        if(v&0x02000000u) icsr&=~0x04000000u;
    } else if(a==0xe000ed08u) vtor=v;
    else if(a==0xe000e010u) systick=v;
    else CHECK(a==0xe000e300u || a==0xe000e304u); /* ACTIVE is read-only */
}
static void model_oracle(void) {
    enabled[1]=pending[1]=0x1800; active[1]=0x800; icsr=0x94000000;
    model_write(0xe000e184u,0xfff); model_write(0xe000e284u,0xfff);
    model_write(0xe000e304u,0xfff); model_write(0xe000ed04u,0x0a000000);
    CHECK(model_read(0xe000e184u)==0x1000);
    CHECK(model_read(0xe000e284u)==0x1000);
    CHECK(model_read(0xe000e304u)==0x800);
    CHECK(model_read(0xe000ed04u)==0x80000000);
    memset(enabled,0,sizeof enabled); memset(pending,0,sizeof pending);
    memset(active,0,sizeof active); icsr=0;
}
static int adapter_read(void *ctx,uint32_t a,uint32_t *v) {
    CHECK(ctx==&token); boundary(); ++adapter_calls;
    if(a>=0xe000e000u) *v=model_read(a);
    else if(a==0x400fe100u) *v=fault_kind==2 ? 8u : gate0;
    else if(a==G1) *v=gate1;
    else if(a==0x400fe108u) *v=0;
    else if(a==0x400fe060u) *v=0x12345678;
    else if(a==0x400fe070u) *v=0x87654321;
    else if(a>=T2 && a<=T2+72 && !(a&3)) *v=timer[(a-T2)/4];
    else if(a==BOOT_WATCHDOG_CTL) *v=ctl;
    else if(a==BOOT_WATCHDOG_RIS) *v=0;
    else {
        CHECK(a==0x400fc014u || a==0x400fc01cu || a==0x400fe054u || a==0x400fd010u || a==0x400fd008u);
        *v=0;
    }
    ae('R',a,*v); return 0;
}
static int adapter_write(void *ctx,uint32_t a,uint32_t v) {
    CHECK(ctx==&token); boundary(); ++adapter_calls; ++adapter_writes;
    /* Closed write set: never WDT/Timer2/HIB/gate/clock, even on error. */
    CHECK(a==0xe000e010u || a==0xe000e180u || a==0xe000e184u ||
          a==0xe000e280u || a==0xe000e284u || a==0xe000ed04u || a==0xe000ed08u);
    model_write(a,v); ae('W',a,v);
    if(fault_kind==3 && a==0xe000ed08u) { fault_end_an=an; fault_end_n=n; return -1; }
    return 0;
}
static int adapter_observe(void *ctx,boot_handoff_context_t *o) {
    CHECK(ctx==&token); boundary(); ++adapter_calls; ae('O',0,0);
    if(fault_kind==4 && c.watchdog.owner==BOOT_WATCHDOG_APPLICATION) {
        unsigned before=an, old=n;
        CHECK(boot_handoff_target_operation(&target,BOOT_HANDOFF_EXCLUDE,0)==-1);
        CHECK(an==before && n==old); /* Deliberately ignore failure in caller. */
        fault_end_an=an; fault_end_n=n;
    }
    *o=(boot_handoff_context_t){0,2,1,0xe0,1}; return 0;
}
static int adapter_barrier(void *ctx,boot_handoff_target_barrier_t b) {
    CHECK(ctx==&token); boundary(); ++adapter_calls; ae('B',(uint32_t)b,0); return 0;
}
boot_runtime_watchdog_result_t sequence_observe_launch(boot_runtime_watchdog_t *,boot_watchdog_t *,
    boot_handoff_t *,const boot_handoff_context_t *,const boot_reset_register_adapter_t *,
    const boot_store_adapter_t *,const boot_select_images_t *,boot_verify_read_fn,void *,boot_runtime_watchdog_launch_t *);
boot_runtime_watchdog_result_t sequence_observe_launch(boot_runtime_watchdog_t *b,boot_watchdog_t *w,
    boot_handoff_t *h,const boot_handoff_context_t *o,const boot_reset_register_adapter_t *r,
    const boot_store_adapter_t *s,const boot_select_images_t *i,boot_verify_read_fn reader,void *ctx,boot_runtime_watchdog_launch_t *outp) {
    CHECK(b==&seq.bridge && w==original && h==&handoff && outp==&launch_out);
    CHECK(configs==1 && w->epoch==1 && w->verification==0 && feeds==0);
    CHECK(load==5u*hz && resc_reads==1 && flash.calls==0);
    CHECK(o->primask==1 && o->control==2 && o->basepri==0xe0 && o->faultmask==1);
    ++runtime_calls;
    return boot_runtime_watchdog_launch(b,w,h,o,r,s,i,reader,ctx,outp);
}
boot_watchdog_result_t sequence_observe_transfer(boot_watchdog_t *,uint32_t);
boot_watchdog_result_t sequence_observe_transfer(boot_watchdog_t *w,uint32_t epoch) {
    CHECK(w==original && epoch==3 && w->epoch==3 && w->owner==BOOT_WATCHDOG_BOOT);
    ++transfers; return boot_watchdog_transfer_held(w,epoch);
}
static void checked_fresh(void) {
    launching=1; published=transfers=adapter_calls=adapter_writes=fault_kind=nested_checks=mutate_inputs=mutated=an=0;
    wdt_at_capture=0;
    operation_count=terminal_count=fault_end_an=fault_end_n=0; memset(operation_values,0,sizeof operation_values);
    memset(&target,0,sizeof target); memset(&handoff,0,sizeof handoff);
    model_oracle(); systick=0; vtor=0; /* Genuine reset: no COUNTFLAG fiction. */
    /* Reset HIB gate; real producer adds WDT and Timer2, no unrelated gates. */
    gate0=0x40; gate1=0;
    observed=(boot_handoff_context_t){0,2,1,0xe0,1};
    boot_handoff_target_io_t io={&token,adapter_read,adapter_write,adapter_observe,adapter_barrier};
    CHECK(boot_handoff_target_init(&target,&c,&io,&handoff)==0);
    /* Forward exactly once, preserving actual adapter outcomes. */
    handoff.io.operation=operation_forward; handoff.io.terminal_intent=terminal_forward;
    launch_params=(boot_sequence_launch_parameters_t){params,&handoff,&observed};
    memset(&launch_sentinel,0xa5,sizeof launch_sentinel); launch_out=launch_sentinel;
}
static void checked_consumed(void) {
    unsigned a=an,b=n,d=resc_reads,e=resc_writes,f=payload_reads,g=feeds,h=configs;
    unsigned tr=transfers,ac=adapter_calls,oc=operation_count,tc=terminal_count,rc=runtime_calls;
    size_t fc=flash.calls;
    static boot_runtime_watchdog_launch_t saved; saved=launch_out;
    boot_sequence_result_t r=invoke();
    CHECK(r.status==BOOT_SEQUENCE_CONSUMED && r.stage==BOOT_SEQUENCE_NONE);
    r=launch_invoke(); CHECK(r.status==BOOT_SEQUENCE_CONSUMED && r.stage==BOOT_SEQUENCE_NONE);
    CHECK(a==an && b==n && d==resc_reads && e==resc_writes && f==payload_reads && g==feeds && h==configs && fc==flash.calls);
    CHECK(tr==transfers && ac==adapter_calls && oc==operation_count && tc==terminal_count && rc==runtime_calls);
    CHECK(!memcmp(&saved,&launch_out,sizeof saved));
}
static void transaction(void) {
    CHECK(configs==1 && runtime_calls==1 && payload_reads==1 && loads==1 && icrs==3 && feeds==2);
    CHECK(resc_reads==3 && resc_writes==1 && last_clear==0);
    CHECK(history[0].id==1 && history[0].epoch==1 && history[0].completed==0);
    CHECK(history[1].id==1 && history[1].epoch==2 && history[1].completed==BOOT_PAYLOAD_MIN);
    CHECK(flash.erases==0 && flash.writes==0 && c.state==BOOT_STARTUP_EMPTY);
    CHECK(c.watchdog.reload==5u*hz && load==5u*hz && c.watchdog.verification==1);
    CHECK(c.watchdog.io.context==c.context && c.watchdog.io.read_register==rd && c.watchdog.io.write_register==wr);
}
static unsigned oracle_cursor;
static void expect_event(char op,uint32_t address,uint32_t value) {
    CHECK(oracle_cursor<an);
    CHECK(adapter_trace[oracle_cursor].op==op && adapter_trace[oracle_cursor].a==address && adapter_trace[oracle_cursor].v==value);
    ++oracle_cursor;
}
static void expect_observation(void) { expect_event('O',0,0); }
static void expect_banks_pend(uint32_t pend0) {
    expect_event('R',0xe000e180,0);
    expect_event('R',0xe000e280,pend0);
    expect_event('R',0xe000e300,0);
    expect_event('R',0xe000e184,0);
    expect_event('R',0xe000e284,0);
    expect_event('R',0xe000e304,0);
    expect_event('R',0xe000ed04,0);
}
static void expect_banks(void) {
    expect_banks_pend(0);
}
static void expect_sources(void) {
    /* Independently enumerated boot-only source allowlist; retained clock
     * register reads are separately recorded in the producer trace. */
    const uint32_t addresses[]={0x400fe100,0x400fe104,0x400fe108,0x400fc014,0x400fc01c,
        0x400fe054,0x400fd010,0x400fd008,0x40000008,0x40000010,
        0x40032000,0x40032004,0x4003200c,0x40032018,0x4003201c,0x40032028,
        0x400fe060,0x400fe070,0x40000010};
    const uint32_t values[]={0x48,0x40000,0,0,0,0,0,0,3,0,0,2,1,0,0,0xffffffff,0x12345678,0x87654321,0};
    for(unsigned j=0;j<19;++j) expect_event('R',addresses[j],values[j]);
}
static void expect_clear(uint32_t address,uint32_t mask) {
    expect_observation(); expect_banks(); expect_event('W',address,mask);
    expect_event('R',address,0); expect_banks();
}
static void expect_clear_pend(uint32_t address,uint32_t mask,uint32_t pre,uint32_t post) {
    /* Pre-clear full-bank gate allows live WDT PEND0 0x40000 (IRQ18 disabled);
     * the ICPR write clears it, post-clear is strict zero. Never WDTICR. */
    expect_observation(); expect_banks_pend(pre); expect_event('W',address,mask);
    expect_event('R',address,0); expect_banks_pend(post);
}
static void exact_adapter_trace_wdt(void) {
    const uint32_t WDT=0x40000u; /* IRQ18 PEND0 bit */
    oracle_cursor=0; expect_observation(); expect_banks_pend(WDT); expect_event('R',0xe000e010,0);
    expect_sources(); expect_event('B',BOOT_HANDOFF_TARGET_EXCLUDE,0); expect_observation();
    expect_observation(); expect_event('R',0xe000e010,0);
    expect_event('W',0xe000e010,0); expect_event('R',0xe000e010,0);
    expect_clear_pend(0xe000e180,0xffffffff,WDT,WDT); expect_clear_pend(0xe000e184,0xfff,WDT,WDT);
    expect_observation(); expect_sources(); expect_banks_pend(WDT);
    expect_observation(); expect_banks_pend(WDT); expect_event('W',0xe000ed04,0x0a000000); expect_event('R',0xe000ed04,0);
    expect_clear_pend(0xe000e280,0xffffffff,WDT,0); expect_clear(0xe000e284,0xfff);
    expect_observation(); expect_event('B',BOOT_HANDOFF_TARGET_SYNC,0); expect_observation(); expect_sources(); expect_banks();
    expect_observation(); expect_event('W',0xe000ed08,BOOT_SLOT_A_BASE); expect_event('R',0xe000ed08,BOOT_SLOT_A_BASE);
    expect_observation(); expect_event('B',BOOT_HANDOFF_TARGET_SYNC,0); expect_observation(); expect_sources(); expect_banks();
    expect_event('R',0xe000ed08,BOOT_SLOT_A_BASE);
    expect_observation(); expect_sources(); expect_banks(); expect_event('R',0xe000e010,0); expect_event('R',0xe000ed08,BOOT_SLOT_A_BASE);
    CHECK(oracle_cursor==an);
}
static void exact_adapter_trace(void) {
    if (wdt_at_capture) { exact_adapter_trace_wdt(); return; }
    oracle_cursor=0; expect_observation(); expect_banks(); expect_event('R',0xe000e010,0);
    expect_sources(); expect_event('B',BOOT_HANDOFF_TARGET_EXCLUDE,0); expect_observation();
    expect_observation(); expect_event('R',0xe000e010,0);
    expect_event('W',0xe000e010,0); expect_event('R',0xe000e010,0);
    expect_clear(0xe000e180,0xffffffff); expect_clear(0xe000e184,0xfff);
    expect_observation(); expect_sources(); expect_banks();
    expect_observation(); expect_banks(); expect_event('W',0xe000ed04,0x0a000000); expect_event('R',0xe000ed04,0);
    expect_clear(0xe000e280,0xffffffff); expect_clear(0xe000e284,0xfff);
    expect_observation(); expect_event('B',BOOT_HANDOFF_TARGET_SYNC,0); expect_observation(); expect_sources(); expect_banks();
    expect_observation(); expect_event('W',0xe000ed08,BOOT_SLOT_A_BASE); expect_event('R',0xe000ed08,BOOT_SLOT_A_BASE);
    expect_observation(); expect_event('B',BOOT_HANDOFF_TARGET_SYNC,0); expect_observation(); expect_sources(); expect_banks();
    expect_event('R',0xe000ed08,BOOT_SLOT_A_BASE);
    expect_observation(); expect_sources(); expect_banks(); expect_event('R',0xe000e010,0); expect_event('R',0xe000ed08,BOOT_SLOT_A_BASE);
    CHECK(oracle_cursor==an);
}
static void write_oracle(void) {
    const uint32_t addresses[]={0xe000e010,0xe000e180,0xe000e184,0xe000ed04,0xe000e280,0xe000e284,0xe000ed08};
    const uint32_t values[]={0,0xffffffff,0xfff,0x0a000000,0xffffffff,0xfff,BOOT_SLOT_A_BASE};
    unsigned w=0,b=0;
    for(unsigned j=0;j<an;++j) {
        if(adapter_trace[j].op=='W') {
            CHECK(w<7 && adapter_trace[j].a==addresses[w] && adapter_trace[j].v==values[w]); ++w;
            CHECK(j+1<an && adapter_trace[j+1].op=='R' && adapter_trace[j+1].a==addresses[w-1]);
            CHECK(adapter_trace[j+1].v==(w==7 ? BOOT_SLOT_A_BASE : 0u));
        }
        if(adapter_trace[j].op=='B') {
            CHECK(b<3 && adapter_trace[j].a==(b==0 ? BOOT_HANDOFF_TARGET_EXCLUDE : BOOT_HANDOFF_TARGET_SYNC)); ++b;
        }
    }
    CHECK(w==7 && b==3); exact_adapter_trace();
    CHECK(operation_count==(unsigned)BOOT_HANDOFF_BARRIER_AFTER_VTOR+1 && terminal_count==1);
    for(unsigned j=0;j<operation_count;++j)
        CHECK(operation_values[j]==(j==BOOT_HANDOFF_SET_VTOR ? BOOT_SLOT_A_BASE : 0u));
    /* Complete captured bank order precedes any write or exclusion. */
    const uint32_t capture[]={0xe000e180,0xe000e280,0xe000e300,0xe000e184,0xe000e284,0xe000e304,0xe000ed04,0xe000e010};
    CHECK(adapter_trace[0].op=='O');
    for(unsigned j=0;j<8;++j) CHECK(adapter_trace[j+1].op=='R' && adapter_trace[j+1].a==capture[j]);
}
static void checked_success(boot_sequence_result_t r) {
    CHECK(r.status==BOOT_SEQUENCE_OK && r.stage==BOOT_SEQUENCE_RUNTIME);
    CHECK(r.runtime.status==BOOT_RUNTIME_WATCHDOG_OK && r.runtime.valid==15 && r.runtime.handoff==BOOT_HANDOFF_OK);
    CHECK(r.runtime.runtime.status==BOOT_RUNTIME_OK && r.runtime.runtime.valid==3);
    CHECK(r.runtime.runtime.selection.evidence_mask==1 && r.runtime.runtime.selection.verify_result[0]==BOOT_VERIFY_OK);
    CHECK(!memcmp(&launch_out.decision.snapshot.record,&authority,sizeof authority));
    CHECK(launch_out.decision.image.manifest.slot==BOOT_SLOT_A);
    CHECK(launch_out.decision.image.initial_msp==BOOT_INITIAL_MSP && launch_out.decision.image.reset_vector==BOOT_SLOT_A_BASE+9);
    const boot_handoff_intent_t expected={BOOT_SLOT_A_BASE,BOOT_INITIAL_MSP,BOOT_SLOT_A_BASE+9,0,0,0,0,4};
    CHECK(!memcmp(&launch_out.intent,&expected,sizeof expected));
    CHECK(!memcmp(&target.intent,&expected,sizeof expected) && target.recorded==1 && !target.failed);
    CHECK(handoff.finished && !handoff.blocked && transfers==1);
    CHECK(c.watchdog.owner==BOOT_WATCHDOG_APPLICATION && c.watchdog.epoch==4 && !c.watchdog.blocked && !c.watchdog.evidence);
    transaction(); write_oracle(); published=1;
    unsigned f=feeds,cf=configs,tr=transfers;
    CHECK(boot_startup_prepare(&c,&r.runtime,&launch_out));
    CHECK(c.state==BOOT_STARTUP_PREPARED && c.entry_epoch==4);
    CHECK(!memcmp(&c.image,&launch_out.decision.image,sizeof c.image) && !memcmp(&c.intent,&expected,sizeof expected));
    CHECK(boot_startup_begin_dispatch(&c));
    CHECK(c.state==BOOT_STARTUP_DISPATCHING && c.dispatch_us==1000000);
    CHECK(feeds==f && configs==cf && transfers==tr);
    CHECK(c.watchdog.owner==BOOT_WATCHDOG_APPLICATION && c.watchdog.epoch==4 && !c.watchdog.blocked && !c.watchdog.evidence);
    checked_consumed();
}
static void checked_failure(boot_sequence_result_t r,unsigned application) {
    CHECK(r.status==BOOT_SEQUENCE_FAILED && r.stage==BOOT_SEQUENCE_RUNTIME);
    CHECK(r.runtime.status==BOOT_RUNTIME_WATCHDOG_HANDOFF_FAILED && r.runtime.handoff==BOOT_HANDOFF_IO);
    CHECK(r.runtime.valid==15 && r.runtime.runtime.status==BOOT_RUNTIME_OK && r.runtime.runtime.valid==3);
    CHECK(target.failed && !target.recorded && handoff.blocked && !handoff.finished);
    CHECK(c.watchdog.blocked && !c.watchdog.evidence);
    CHECK(c.watchdog.owner==(application ? BOOT_WATCHDOG_APPLICATION : BOOT_WATCHDOG_BOOT));
    CHECK(c.watchdog.epoch==(application ? 4u : 3u) && transfers==application);
    CHECK(!memcmp(&launch_out,&launch_sentinel,sizeof launch_out)); transaction();
    unsigned a=an,b=n,ac=adapter_calls;
    CHECK(boot_handoff_target_operation(&target,BOOT_HANDOFF_EXCLUDE,0)==-1);
    CHECK(boot_handoff_target_terminal_intent(&target,&launch_out.intent)==-1);
    checked_consumed(); CHECK(an==a && n==b && adapter_calls==ac);
}
/* Backed aligned arenas: compare union bytes once, including aliased spans. */
static union { max_align_t align; unsigned char bytes[sizeof(boot_runtime_watchdog_launch_t)+sizeof(boot_sequence_launch_parameters_t)+1024]; } alias_arena, alias_before;
static void admission(void) {
    static boot_startup_t cs; static boot_handoff_t hs; static boot_sequence_t ss;
    uint32_t ds[16];
    for(unsigned bad=0;bad<6;++bad) {
        memset(&alias_arena,0xa5,sizeof alias_arena);
        boot_sequence_launch_parameters_t *p=&launch_params;
        boot_runtime_watchdog_launch_t *o=&launch_out;
        boot_sequence_launch_parameters_t local=launch_params;
        if(bad==0) {
            p=(void *)alias_arena.bytes; *p=local;
            o=(void *)(alias_arena.bytes+offsetof(boot_sequence_launch_parameters_t,handoff));
        }
        if(bad==1) { local.handoff=(void *)alias_arena.bytes; local.observed=(void *)alias_arena.bytes; p=&local; }
        if(bad==2) { local.observed=(void *)&c; p=&local; }
        if(bad==3) {
            o=(void *)alias_arena.bytes;
            local.common.diagnostic=(void *)(alias_arena.bytes+offsetof(boot_runtime_watchdog_launch_t,intent)); p=&local;
        }
        if(bad==4) { handoff.io.context=(void *)&launch_params.handoff; }
        if(bad==5) observed.primask=0;
        memcpy(&cs,&c,sizeof c); memcpy(&hs,&handoff,sizeof hs); memcpy(&ss,&seq,sizeof ss); memcpy(ds,words,sizeof ds);
        alias_before=alias_arena;
        boot_sequence_launch_parameters_t ps=*p;
        boot_handoff_context_t os=observed;
        boot_sequence_result_t r=boot_sequence_launch(&seq,p,o);
        CHECK(r.status==BOOT_SEQUENCE_ARGUMENT && r.stage==BOOT_SEQUENCE_NONE);
        CHECK(!seq.consumed && !memcmp(&ss,&seq,sizeof ss));
        CHECK(!memcmp(&cs,&c,sizeof c) && !memcmp(&hs,&handoff,sizeof hs) && !memcmp(ds,words,sizeof ds));
        CHECK(!memcmp(&alias_before,&alias_arena,sizeof alias_arena) && !memcmp(&ps,p,sizeof ps) && !memcmp(&os,&observed,sizeof os));
        CHECK(!memcmp(&launch_out,&launch_sentinel,sizeof launch_out));
        CHECK(!calls && !resc_reads && !resc_writes && !flash.calls && !payload_reads && !an && !configs);
        handoff.io.context=&target; observed.primask=1;
    }
    check_guards=1; mutate_inputs=1;
    boot_sequence_result_t r=launch_invoke();
    CHECK(mutated && nested_checks>20);
    launch_params=(boot_sequence_launch_parameters_t){params,&handoff,&observed}; observed.primask=1;
    checked_success(r);
    /* Distinct reverse-order boot: unchanged decision success oracle. */
    unsigned previous_calls=adapter_calls, previous_ops=operation_count, previous_terminals=terminal_count;
    launching=0; fresh(); success(invoke());
    CHECK(adapter_calls==previous_calls && operation_count==previous_ops && terminal_count==previous_terminals);
    r=boot_sequence_launch(&seq,&launch_params,&launch_out);
    CHECK(r.status==BOOT_SEQUENCE_CONSUMED && r.stage==BOOT_SEQUENCE_NONE);
    CHECK(c.state==BOOT_STARTUP_EMPTY && c.watchdog.owner==BOOT_WATCHDOG_BOOT && c.watchdog.epoch==3);
}
static int checked_case(const char *name) {
    if(strncmp(name,"launch-",7) && strncmp(name,"target-",7)) return 0;
    checked_fresh();
    if(!strcmp(name,"launch-admission-cross-api")) admission();
    else if(!strcmp(name,"launch-authentic-prepare")) checked_success(launch_invoke());
    else if(!strcmp(name,"target-wdt-pending-exempt")) {
        /* Live WDT pending at capture with IRQ18 disabled: exempt at capture
         * and pre-clear gates, cleared via NVIC ICPR, strict zero after. */
        pending[0]=0x40000u; wdt_at_capture=1;
        CHECK(enabled[0]==0 && active[0]==0);
        checked_success(launch_invoke());
        CHECK(target.pending[0]==0x40000u && pending[0]==0u);
    }
    else if(!strcmp(name,"target-other-pending-reject")) {
        /* Non-WDT bank0 pending must stay fail-closed: no writes, no exclude. */
        pending[0]=0x20000u;
        boot_sequence_result_t r=launch_invoke();
        CHECK(target.pending[0]==0x20000u);
        CHECK(!adapter_writes && target.next==0 && operation_count==1 && !terminal_count);
        for(unsigned j=0;j<an;++j) CHECK(adapter_trace[j].op!='B');
        checked_failure(r,0);
    }
    else {
        if(!strcmp(name,"target-w1c-irq43")) { fault_kind=1; enabled[1]=pending[1]=active[1]=0x800; }
        else if(!strcmp(name,"target-hib-gate-failclosed")) fault_kind=2;
        else if(!strcmp(name,"target-io-failure-poison")) fault_kind=3;
        else { CHECK(!strcmp(name,"target-terminal-reentry-poison")); fault_kind=4; }
        boot_sequence_result_t r=launch_invoke();
        if(fault_kind==1) {
            CHECK(target.enables[1]==0x800 && target.pending[1]==0x800 && target.active[1]==0x800);
            CHECK(!adapter_writes && target.next==0 && operation_count==1 && !terminal_count);
            for(unsigned j=0;j<an;++j) CHECK(adapter_trace[j].op!='B');
        }
        if(fault_kind==2) {
            CHECK(an && adapter_trace[an-1].op=='R' && adapter_trace[an-1].a==0x400fe100u);
            CHECK(!adapter_writes && operation_count==1 && !terminal_count);
            for(unsigned j=0;j<an;++j) CHECK(adapter_trace[j].a<0x400fc000u || adapter_trace[j].a>=0x400fd000u);
        }
        if(fault_kind==3) {
            CHECK(vtor==BOOT_SLOT_A_BASE && target.next==BOOT_HANDOFF_SET_VTOR);
            CHECK(an && adapter_trace[an-1].op=='W' && adapter_trace[an-1].a==0xe000ed08u);
        }
        if(fault_kind==4) CHECK(an && adapter_trace[an-1].op=='O');
        if(fault_kind>=3) CHECK(an==fault_end_an && n==fault_end_n);
        checked_failure(r,fault_kind==4);
    }
    return 1;
}
