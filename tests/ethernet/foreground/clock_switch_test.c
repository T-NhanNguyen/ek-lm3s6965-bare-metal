/* Actual separately compiled net_clock.c + startup/GPTM; HOST admission only.
 * Callback backing storage has process lifetime, NOT signed boot provenance. */
#include "check.h"
#include "net_clock.h"
#include "net_clock_port.h"
#include "lm3s6965/boot_startup_gptm.h"
#include <string.h>
boot_startup_t boot_startup_continuity;
static uint32_t diagnostics[16], timer_regs[4][19], gate, rcc, rcc2;
static unsigned ios, tar_reads, ctl_writes, icr_writes, icr_reads, delays;
static unsigned barriers, unmasks, starts, started_at, final_at, unmask_at, second_barrier_at;
static unsigned sequence, wdt_reads, wdt_writes, poison_at;
static unsigned ris_reads, rcc2_reads;
static int fault, nested_result;
static bool masked = true, inject_pending;
static uint32_t ipsr, basepri, faultmask, control;
static uint32_t systick[3], icsr, icsr_cell, shcsr, enabled[2], pending[2], disable[2], clear[2];
static uint32_t *last;
static uint32_t last_address;
static boot_startup_gptm_t *g(void) { return (void *)boot_startup_continuity.context; }
static uint32_t base(void) { return 0x40030000u + g()->timer * 0x1000u; }
static void flush(void)
{
    if (!last) return;
    if (last_address == 0xe000ed04u) {
        if (icsr_cell & (1u<<27)) icsr &= ~(1u<<28);
        if (icsr_cell & (1u<<25)) icsr &= ~(1u<<26);
    }
    for (unsigned i=0; i<2; ++i) {
        enabled[i] &= ~disable[i]; pending[i] &= ~clear[i];
        disable[i]=clear[i]=0;
    }
    if (last_address == 0xe000e010u && systick[0] == 7u && !starts) {
        ++starts; started_at=++sequence;
    }
    last=NULL;
}
volatile uint32_t *clock_test_register(uint32_t a)
{
    flush(); ++sequence; last_address=a;
    if (a>=0xe000e010u && a<=0xe000e018u && (a&3u)==0) last=&systick[(a-0xe000e010u)/4u];
    else if (a==0xe000ed04u) { icsr_cell=icsr; last=&icsr_cell; }
    else if (a==0xe000ed24u) last=&shcsr;
    else for (unsigned i=0; i<2; ++i) {
        if (a==0xe000e100u+4*i) last=&enabled[i];
        if (a==0xe000e180u+4*i) last=&disable[i];
        if (a==0xe000e200u+4*i) last=&pending[i];
        if (a==0xe000e280u+4*i) last=&clear[i];
    }
    CHECK(last != NULL); return last;
}
bool net_clock_port_masked_thread(void) { flush(); return masked && ipsr==0 && basepri==0 && faultmask==0 && control==0; }
void net_clock_port_barrier(void)
{
    flush(); ++barriers; ++sequence;
    if (starts) second_barrier_at=sequence;
    if (starts && inject_pending) pending[1]=4;
}
void net_clock_port_unmask(void)
{
    flush(); CHECK(masked && starts==1 && final_at>started_at);
    CHECK(enabled[0]==0 && enabled[1]==0 && pending[0]==0 && pending[1]==0);
    uint32_t offset=0; CHECK(net_clock_startup_elapsed(&offset) && offset==2);
    masked=false; ++unmasks; unmask_at=++sequence;
}
_Noreturn void boot_startup_stop(void) { CHECK(0); abort(); }
static void nested(void)
{
    /* Nested return is deliberately ignored by the injected callback. */
    nested_result=boot_startup_gptm_release(&boot_startup_continuity);
    poison_at=ios;
}
static int rd(void *p, uint32_t a, uint32_t *v)
{
    (void)p; ++ios; ++sequence;
    if (a==0x400fe104u) { *v=gate; if (fault==1 && ios==3) return -1; return 0; }
    if (a==0x400fe060u) { *v=rcc; return 0; }
    if (a==0x400fe070u) {
        *v=rcc2; ++rcc2_reads;
        if (fault==5 && rcc2_reads==2) *v ^= 1;
        if (fault==7 && rcc2_reads==2) nested();
        return 0;
    }
    CHECK(a>=base() && a<=base()+72u && ((a-base())&3u)==0);
    unsigned offset=(a-base())/4u;
    if (offset==9) ++icr_reads;
    if (offset==7) {
        ++ris_reads;
        if (fault==3 || (fault==4 && ris_reads==2)) { *v=1; return 0; }
        if (fault==8) nested();
    }
    if (offset==18) { ++tar_reads; if (starts) final_at=sequence; }
    *v=timer_regs[g()->timer][offset]; return 0;
}
static int wr(void *p, uint32_t a, uint32_t v)
{
    (void)p; ++ios; ++sequence;
    if (a==0x400fe104u) { gate=v; return 0; }
    CHECK(a>=base() && a<=base()+72u && ((a-base())&3u)==0);
    unsigned offset=(a-base())/4u;
    if (offset==3) { ++ctl_writes; if (fault==9 && v==0) nested(); }
    if (offset==9) { ++icr_writes; CHECK(v==0x70fu); timer_regs[g()->timer][7]=0; }
    timer_regs[g()->timer][offset]=v; return 0;
}
static boot_watchdog_io_t io;
static void delay(void *p)
{
    CHECK(p==NULL && gate & (1u<<(16u+g()->timer)));
    CHECK(ios==3 && ctl_writes==0); ++delays;
    if (fault==6) {
        nested_result=boot_startup_gptm_claim(g(),g()->timer,50000000,&io,delay);
        poison_at=ios;
    }
}
static int wdread(void *p, uint32_t a, uint32_t *v)
{
    CHECK(p==boot_startup_continuity.context); ++wdt_reads;
    switch(a) {
    case BOOT_WATCHDOG_RCGC0: *v=8; break;
    case BOOT_WATCHDOG_CTL: *v=3; break;
    case BOOT_WATCHDOG_LOAD: *v=250000000; break;
    case BOOT_WATCHDOG_LOCK: *v=1; break;
    case BOOT_WATCHDOG_RIS: *v=0; break;
    default: CHECK(0);
    } return 0;
}
static int wdwrite(void *p,uint32_t a,uint32_t v)
{ (void)p; (void)a; (void)v; ++wdt_writes; CHECK(0); return -1; }
static void reset(void)
{
    memset(&boot_startup_continuity,0,sizeof boot_startup_continuity);
    memset(timer_regs,0,sizeof timer_regs); gate=rcc=rcc2=0;
    ios=tar_reads=ctl_writes=icr_writes=icr_reads=delays=0;
    ris_reads=rcc2_reads=poison_at=0; fault=0; nested_result=-2;
    io=(boot_watchdog_io_t){NULL,rd,wr};
}
static void claim(unsigned timer)
{
    CHECK(boot_startup_gptm_claim(g(),timer,50000000,&io,delay));
    timer_regs[timer][18]=UINT32_MAX-50000u;
}
static void bind(void)
{
    boot_startup_t *c=&boot_startup_continuity;
    c->state=BOOT_STARTUP_C_READY; c->hz=50000000;
    c->clock=(boot_startup_clock_t){boot_startup_gptm_read,c->context,0,0,
        UINT32_MAX,UINT32_MAX,50000000,BOOT_STARTUP_GPTM_SOURCE};
    CHECK(boot_startup_gptm_supported(c));
}
static void counters(void)
{ ios=tar_reads=ctl_writes=icr_writes=icr_reads=ris_reads=rcc2_reads=0; }
static void terminal(void)
{
    unsigned n=ios; boot_startup_sample_t s, saved;
    memset(&s,0xa5,sizeof s); saved=s;
    CHECK(!boot_startup_gptm_claim(g(),0,50000000,&io,delay));
    CHECK(boot_startup_gptm_read(g(),&s)!=0);
    CHECK(!boot_startup_gptm_release(&boot_startup_continuity));
    CHECK(ios==n && !memcmp(&s,&saved,sizeof s));
}
static void entered(void)
{
    reset(); boot_startup_t *c=&boot_startup_continuity;
    boot_watchdog_io_t w={c->context,wdread,wdwrite};
    boot_startup_clock_t clock={boot_startup_gptm_read,c->context,0,0,
        UINT32_MAX,UINT32_MAX,50000000,BOOT_STARTUP_GPTM_SOURCE};
    CHECK(boot_startup_reset(c,diagnostics,0,&w,&clock)); claim(2);
    c->watchdog.configured=1; c->watchdog.owner=BOOT_WATCHDOG_APPLICATION;
    c->watchdog.epoch=9; c->watchdog.reload=250000000;
    boot_runtime_watchdog_launch_t l={0};
    boot_runtime_watchdog_result_t result={.status=BOOT_RUNTIME_WATCHDOG_OK,
        .valid=BOOT_RUNTIME_WATCHDOG_HANDOFF,.handoff=BOOT_HANDOFF_OK};
    l.decision.image.manifest.execution_base=0x8800;
    l.decision.image.manifest.payload_length=4096;
    l.decision.image.initial_msp=0x20010000;
    l.decision.image.reset_vector=0x8901;
    l.intent=(boot_handoff_intent_t){0x8800,0x20010000,0x8901,0,0,0,0,9};
    CHECK(boot_startup_prepare(c,&result,&l));
    CHECK(boot_startup_begin_dispatch(c));
    c->observed=(boot_startup_observations_t){.msp=0x20010000,.vtor=0x8800,
        .pc=0x8920,.stub=0x8901};
    CHECK(boot_startup_early(c,0x8901,0x8920));
    CHECK(c->entry_us==1000 && wdt_writes==0);
}
int main(int argc,char **argv)
{
    CHECK(argc==2); const char *name=argv[1];
    if (!strcmp(name,"c_ready_validated_before_app")) {
        for (unsigned variant=0; variant<4; ++variant) {
            entered(); boot_startup_t *c=&boot_startup_continuity;
            if (variant==1) c->watchdog.owner=BOOT_WATCHDOG_BOOT;
            if (variant==2) timer_regs[2][18]=UINT32_MAX-49950u;
            if (variant==3) timer_regs[2][7]=1;
            unsigned feeds=wdt_writes;
            CHECK(boot_startup_c_ready(c)==(variant==0));
            CHECK(c->state==(variant==0 ? BOOT_STARTUP_C_READY : BOOT_STARTUP_FAILED));
            CHECK(c->observed.initialized==(variant==0)); CHECK(wdt_writes==feeds);
            boot_diag_t diag; boot_diag_context_t context;
            CHECK(boot_diag_publication_snapshot(&c->publication,&diag,&context)==BOOT_DIAG_PUBLICATION_OK);
            CHECK((diag.health & BOOT_DIAG_HEALTH_APP)==(variant==0 ? BOOT_DIAG_HEALTH_APP : 0));
            printf("FOREGROUND_CONFIG_OK %s readiness-%u\n",name,variant);
        }
    } else if (!strncmp(name,"systick_",8)) {
        entered(); CHECK(boot_startup_c_ready(&boot_startup_continuity));
        enabled[0]=0xa5; enabled[1]=0x7ff; pending[0]=3; pending[1]=4;
        icsr=(1u<<28)|(1u<<26); /* stale PendSV/SysTick clear commands */
        timer_regs[2][18]=UINT32_MAX-100000u;
        if (!strcmp(name,"systick_admission_rejects_unsafe_core")) {
            unsigned n=ios;
            masked=false; CHECK(!net_clock_initialize(50000000)); masked=true;
            CHECK(ios==n && starts==0);
            printf("FOREGROUND_CONFIG_OK %s mask\n",name);
            uint32_t source=boot_startup_continuity.clock.source;
            boot_startup_continuity.clock.source=1;
            CHECK(!net_clock_initialize(50000000));
            CHECK(ios==n && ctl_writes==1 && starts==0);
            printf("FOREGROUND_CONFIG_OK %s unknown-source\n",name);
            boot_startup_continuity.clock.source=source;
            shcsr=1u<<16; CHECK(!net_clock_initialize(50000000));
            CHECK(starts==0 && unmasks==0 && wdt_writes==0);
            printf("FOREGROUND_CONFIG_OK %s shcsr\n",name);
        } else if (!strcmp(name,"systick_poststart_pending_fails_closed")) {
            inject_pending=true; CHECK(!net_clock_initialize(50000000));
            CHECK(starts==1 && unmasks==0 && pending[1]==4 && masked);
        } else {
            CHECK(!strcmp(name,"systick_offset_no_retained_io"));
            boot_watchdog_t watchdog=boot_startup_continuity.watchdog;
            CHECK(net_clock_initialize(50000000));
            uint32_t offset=0; CHECK(net_clock_startup_elapsed(&offset) && offset==2);
            CHECK(started_at<second_barrier_at && second_barrier_at<final_at && final_at<unmask_at && barriers==2 && unmasks==1);
            CHECK(systick[0]==7 && systick[1]==49999 && systick[2]==0 && sys_now()==0);
            CHECK(boot_startup_gptm_release(&boot_startup_continuity));
            CHECK(!memcmp(&watchdog,&boot_startup_continuity.watchdog,sizeof watchdog));
            CHECK(boot_startup_continuity.entry_us==1000 && boot_startup_continuity.entry_epoch==9);
            unsigned n=ios, reads=wdt_reads;
            for (unsigned i=0;i<3;++i) SysTick_Handler();
            CHECK(sys_now()==3 && ios==n && wdt_reads==reads && wdt_writes==0);
            CHECK(net_clock_startup_elapsed(&offset) && offset==2);
            CHECK(!net_clock_initialize(50000000) && ios==n);
        }
    } else {
        reset();
        if (!strcmp(name,"gptm_explicit_timer_claim_configuration")) {
            for(unsigned t=0;t<4;++t) {
                reset(); gate=0x80; claim(t);
                CHECK(gate==(0x80u|(1u<<(16+t))) && delays==1 && icr_writes==1 && icr_reads==0);
                CHECK(timer_regs[t][0]==0 && timer_regs[t][1]==2 && timer_regs[t][3]==1);
                CHECK(timer_regs[t][6]==0 && timer_regs[t][10]==UINT32_MAX);
                for(unsigned other=0;other<4;++other) if(other!=t)
                    for(unsigned r=0;r<19;++r) CHECK(timer_regs[other][r]==0);
                boot_startup_sample_t s; CHECK(boot_startup_gptm_read(g(),&s)==0);
                CHECK(s.us==1000 && s.hz==50000000 && s.source==BOOT_STARTUP_GPTM_SOURCE);
                printf("FOREGROUND_CONFIG_OK %s timer-%u\n",name,t);
            }
        } else if (!strcmp(name,"gptm_unknown_or_enabled_never_stops_timer")) {
            CHECK(!boot_startup_gptm_release(&boot_startup_continuity) && ios==0);
            timer_regs[3][3]=1;
            CHECK(!boot_startup_gptm_claim(g(),3,50000000,&io,delay));
            CHECK(ctl_writes==0 && timer_regs[3][3]==1); terminal();
        } else if (!strcmp(name,"gptm_claim_io_failure_consumed")) {
            fault=1; CHECK(!boot_startup_gptm_claim(g(),0,50000000,&io,delay));
            CHECK(ios==3 && delays==0 && ctl_writes==0); terminal();
        } else if (!strcmp(name,"gptm_claim_reentry_terminal")) {
            fault=6; CHECK(!boot_startup_gptm_claim(g(),0,50000000,&io,delay));
            CHECK(nested_result==0 && poison_at==ios && ctl_writes==0); terminal();
        } else {
            claim(1); bind(); counters();
            boot_startup_sample_t s, saved; memset(&s,0xa5,sizeof s); saved=s;
            if (!strcmp(name,"gptm_expiry_before_tar")) fault=3;
            else if (!strcmp(name,"gptm_expiry_after_tar")) fault=4;
            else if (!strcmp(name,"gptm_drift_rejects_unpublished_sample")) fault=5;
            else if (!strcmp(name,"gptm_read_reentry_terminal")) fault=7;
            else if (!strcmp(name,"gptm_release_consumed_no_doublewrite")) {
                CHECK(boot_startup_gptm_release(&boot_startup_continuity));
                CHECK(ctl_writes==1 && tar_reads==0); terminal();
                printf("FOREGROUND_CONFIG_OK %s normal\n",name);
                reset(); claim(1); bind(); counters(); fault=8;
                CHECK(!boot_startup_gptm_release(&boot_startup_continuity));
                CHECK(nested_result==0 && poison_at==ios && ctl_writes==0 && tar_reads==0);
                terminal();
                printf("FOREGROUND_CONFIG_OK %s ris-reentry\n",name);
                reset(); claim(1); bind(); counters(); fault=9;
            } else CHECK(0);
            if (fault==8 || fault==9) {
                CHECK(!boot_startup_gptm_release(&boot_startup_continuity));
                CHECK(ctl_writes==(fault==9 ? 1u:0u));
            } else {
                CHECK(boot_startup_gptm_read(g(),&s)!=0 && !memcmp(&s,&saved,sizeof s));
                CHECK(tar_reads==(fault==3 ? 0u:1u)); CHECK(ctl_writes==0);
            }
            CHECK(icr_reads==0 && icr_writes==0);
            if (fault>=7) CHECK(nested_result==0 && poison_at==ios);
            terminal();
            if (fault==9) printf("FOREGROUND_CONFIG_OK %s ctl-reentry\n",name);
        }
    }
    printf("FOREGROUND_CASE_OK %s\n",name); return 0;
}
