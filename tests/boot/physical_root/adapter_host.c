/* Only the exact generated leaf-access copy is linked here. No physical mmap,
 * signal trap, arbitrary pointer translation, or raw target object is allowed. */
#include "lm3s6965/boot_target_io.h"
#include "safe_access.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static uint32_t regs[7];
static const uint32_t addresses[]={0x400fe05c,0x400fd000,0x400fd004,0x400fd008,0x400fd00c,0x400fd014,0x400fe140};
static uint8_t memory[0x37800]; /* exact [8000,3f800), static sanitizer-safe aggregate */
static struct { uint32_t address; unsigned writing; } events[1024];
static unsigned calls, reads, fail_at;
static uint32_t seen_address[4]; static size_t seen_size[4];
volatile uint32_t *fixture_word(uint32_t a,int w) {
    CHECK(calls<1024); events[calls].address=a; events[calls++].writing=(unsigned)w;
    for(unsigned j=0;j<7;++j) if(a==addresses[j]) {
        CHECK(!w || (j!=4 && j!=6)); return &regs[j];
    }
    CHECK(0); return NULL;
}
volatile const uint8_t *fixture_byte(uint32_t a) {
    CHECK(a>=0x8000 && a<0x3f800); CHECK(calls<1024);
    events[calls].address=a; events[calls++].writing=0;
    return &memory[a-0x8000];
}
static boot_startup_t continuity;
static uint32_t diagnostic[16];
static boot_watchdog_io_t producer_io;
static void no_delay(void *p) { (void)p; CHECK(0); }
static boot_flash_t flash, saved_flash;
static boot_select_images_t images, saved_images;
static boot_sequence_parameters_t params, saved_params;
static boot_reset_register_adapter_t reset, saved_reset;
static boot_store_adapter_t store, saved_store;
static void unchanged(void) { CHECK(!memcmp(&flash,&saved_flash,sizeof flash)); CHECK(calls==0); }
static void fresh(void) {
    memset(regs,0,sizeof regs); regs[6]=7; calls=reads=fail_at=0;
    memset(&flash,0xa5,sizeof flash); saved_flash=flash;
    memset(&images,0xa5,sizeof images); saved_images=images;
}
static int model_register(void *p,uint32_t a,uint32_t *v) {
    CHECK(p==&reads && a==BOOT_FLASH_FMC); *v=0; return 0;
}
static int forbidden_write(void *p,uint32_t a,uint32_t v) { (void)p;(void)a;(void)v; CHECK(0); return -1; }
static int model_read(void *p,uint32_t a,uint8_t *b,size_t z) {
    CHECK(p==&reads && reads<4); seen_address[reads]=a; seen_size[reads]=z; ++reads;
    memset(b,reads==1?0x31:0x72,z);
    return reads==fail_at?-1:0; /* possibly dirtied local output before IO failure */
}
static void model(void) {
    boot_flash_io_t io={&reads,model_register,forbidden_write,model_read};
    CHECK(boot_flash_init(&flash,&io,8,3)==BOOT_FLASH_OK);
}
static void safe_args(void) {
    uint32_t v=0xfeedbeef;
    boot_flash_io_t io=boot_target_flash_io(); reset=boot_target_reset_adapter();
    CHECK(reset.context==NULL && io.context==NULL);
    CHECK(reset.read(NULL,NULL)==-1);
    CHECK(reset.read(NULL,(void *)((uint8_t *)&v+1))==-1);
    CHECK(io.read_register(NULL,BOOT_FLASH_FMA,NULL)==-1);
    CHECK(io.read_register(NULL,BOOT_FLASH_FMA,(void *)((uint8_t *)&v+1))==-1);
    CHECK(v==0xfeedbeef && calls==0);
    CHECK(boot_target_flash_init(NULL,8000000,1)==BOOT_FLASH_ARGUMENT);
    CHECK(boot_target_flash_init((void *)((uint8_t *)&flash+1),8000000,1)==BOOT_FLASH_ARGUMENT);
    const uint32_t hz[]={0,999999,8000001,51000000};
    for(unsigned j=0;j<4;++j) { CHECK(boot_target_flash_init(&flash,hz[j],1)==BOOT_FLASH_ARGUMENT); unchanged(); }
    CHECK(boot_target_flash_init(&flash,8000000,0)==BOOT_FLASH_ARGUMENT); unchanged();
    CHECK(boot_target_images_read(NULL,&images)==BOOT_FLASH_ARGUMENT);
    CHECK(boot_target_images_read(&flash,NULL)==BOOT_FLASH_ARGUMENT);
    CHECK(boot_target_images_read((void *)((uint8_t *)&flash+1),&images)==BOOT_FLASH_ARGUMENT);
    CHECK(boot_target_images_read(&flash,(void *)((uint8_t *)&images+1))==BOOT_FLASH_ARGUMENT);
    CHECK(!memcmp(&images,&saved_images,sizeof images)); unchanged();
}
static void physical_map(void) {
    boot_flash_io_t io=boot_target_flash_io(); reset=boot_target_reset_adapter(); uint32_t v;
    CHECK(BOOT_FLASH_FMA==0x400fd000 && BOOT_FLASH_FMD==0x400fd004 && BOOT_FLASH_FMC==0x400fd008);
    CHECK(BOOT_FLASH_FCRIS==0x400fd00c && BOOT_FLASH_FCMISC==0x400fd014 && BOOT_FLASH_USECRL==0x400fe140);
    regs[0]=0x80000012; CHECK(reset.read(NULL,&v)==0 && v==0x80000012);
    regs[0]=0xc000001a; CHECK(reset.read(NULL,&v)==0 && v==0xc000001a);
    CHECK(reset.write(NULL,0xc0000018)==0 && regs[0]==0xc0000018);
    CHECK(calls==3 && events[0].address==0x400fe05c && !events[0].writing && events[1].address==0x400fe05c && !events[1].writing && events[2].address==0x400fe05c && events[2].writing);
    for(unsigned j=1;j<7;++j) { regs[j]=0xf000+j; CHECK(io.read_register(NULL,addresses[j],&v)==0 && v==regs[j]); CHECK(events[calls-1].address==addresses[j] && !events[calls-1].writing); }
    const unsigned writable[]={1,2,3,5};
    for(unsigned j=0;j<4;++j) { unsigned k=writable[j]; CHECK(io.write_register(NULL,addresses[k],0x12340000+k)==0); CHECK(regs[k]==0x12340000+k && events[calls-1].address==addresses[k] && events[calls-1].writing); }
    unsigned old=calls; uint32_t status=regs[4],timing=regs[6];
    CHECK(io.write_register(NULL,BOOT_FLASH_FCRIS,0)==-1 && io.write_register(NULL,BOOT_FLASH_USECRL,0)==-1);
    CHECK(io.write_register(NULL,0x400fd010,1)==-1 && io.read_register(NULL,0x400fd010,&v)==-1);
    CHECK(calls==old && status==regs[4] && timing==regs[6]);
    uint8_t b[4]={0}; memory[0]=0x38; memory[sizeof memory-1]=0x91;
    CHECK(io.read_flash(NULL,0x8000,b,1)==0 && b[0]==0x38);
    CHECK(io.read_flash(NULL,0x3f7ff,b,1)==0 && b[0]==0x91);
    memory[100]=0x18; memory[101]=0x27; memory[102]=0x36; memory[103]=0x45;
    CHECK(io.read_flash(NULL,0x8064,b,4)==0 && b[0]==0x18 && b[1]==0x27 && b[2]==0x36 && b[3]==0x45);
    for(unsigned j=0;j<4;++j) CHECK(events[calls-4+j].address==0x8064+j && !events[calls-4+j].writing);
    old=calls;
    const uint32_t bad[]={0x7fff,0x3f800,UINT32_MAX};
    for(unsigned j=0;j<3;++j) CHECK(io.read_flash(NULL,bad[j],b,1)==-1);
    CHECK(io.read_flash(NULL,0x3f7ff,b,2)==-1 && io.read_flash(NULL,0x8000,b,SIZE_MAX)==-1);
    CHECK(io.read_flash(NULL,0x8000,b,0)==-1 && io.read_flash(NULL,0x8000,NULL,1)==-1 && calls==old);
}
static void init_clock(void) {
    CHECK(boot_target_flash_init(&flash,9000000,3)==BOOT_FLASH_CLOCK);
    CHECK(!memcmp(&flash,&saved_flash,sizeof flash) && calls==1 && !events[0].writing && events[0].address==0x400fe140);
    fresh(); regs[6]=0xabc00007;
    CHECK(boot_target_flash_init(&flash,8000000,3)==BOOT_FLASH_OK);
    CHECK(flash.mhz==8 && flash.polls==3 && !flash.blocked && flash.blocked_result==BOOT_FLASH_OK);
    CHECK(flash.io.context==NULL && calls==1 && !events[0].writing && regs[6]==0xabc00007);
    /* Real controller still rechecks timing before any modification: no command issued. */
    regs[6]=8; unsigned old=calls;
    CHECK(boot_flash_program(&flash,BOOT_METADATA_PAGE0,0)==BOOT_FLASH_CLOCK);
    CHECK(calls==old+2 && events[old].address==BOOT_FLASH_FMC && events[old+1].address==BOOT_FLASH_USECRL);
    CHECK(!events[old].writing && !events[old+1].writing);
}
static void images_publication(void) {
    CHECK(BOOT_SLOT_A_MANIFEST==0x23c00 && BOOT_SLOT_B_MANIFEST==0x3f400 && BOOT_MANIFEST_BYTES==160);
    model(); CHECK(boot_target_images_read(&flash,&images)==BOOT_FLASH_OK);
    CHECK(reads==2 && seen_address[0]==0x23c00 && seen_address[1]==0x3f400 && seen_size[0]==160 && seen_size[1]==160);
    CHECK(images.size[0]==160 && images.size[1]==160);
    for(unsigned j=0;j<160;++j) CHECK(images.wire[0][j]==0x31 && images.wire[1][j]==0x72);
    fresh(); model(); fail_at=2;
    CHECK(boot_target_images_read(&flash,&images)==BOOT_FLASH_IO && reads==2);
    CHECK(!memcmp(&images,&saved_images,sizeof images));
    fresh(); model(); fail_at=1;
    CHECK(boot_target_images_read(&flash,&images)==BOOT_FLASH_IO && reads==1);
    CHECK(!memcmp(&images,&saved_images,sizeof images));
}
static void bridge_bind(void) {
    uint8_t b[256]; model();
    CHECK(boot_target_verify_read(&flash,0x23b00,b,256)==256 && reads==1 && seen_address[0]==0x23b00 && seen_size[0]==256);
    CHECK(boot_target_verify_read(&flash,0x3f3ff,b,1)==1 && reads==2 && seen_address[1]==0x3f3ff && seen_size[1]==1);
    const uint32_t bad[]={0x8000,0x23c00,0x23fff,0x3f400,UINT32_MAX};
    for(unsigned j=0;j<5;++j) CHECK(boot_target_verify_read(&flash,bad[j],b,1)==0);
    CHECK(!boot_target_verify_read(&flash,0x23bff,b,2));
    CHECK(!boot_target_verify_read(&flash,0x8800,b,257) && !boot_target_verify_read(&flash,0x8800,b,0));
    CHECK(!boot_target_verify_read(NULL,0x8800,b,1) && !boot_target_verify_read((void *)((uint8_t *)&flash+1),0x8800,b,1));
    CHECK(!boot_target_verify_read(&flash,0x8800,NULL,1) && reads==2);
    fail_at=3; CHECK(!boot_target_verify_read(&flash,0x8800,b,1) && reads==3);
    memset(&params,0,sizeof params); params.actual_hz=12345678;
    params.continuity=&continuity; params.diagnostic=diagnostic;
    params.producer_io=&producer_io; params.gate_delay=no_delay; saved_params=params;
    memset(&reset,0xa5,sizeof reset); saved_reset=reset;
    memset(&store,0xa5,sizeof store); saved_store=store;
    for(unsigned j=0;j<10;++j) {
        void *bad=j<5?NULL:(void *)((uint8_t *)(j%5==0?(void *)&params:j%5==1?(void *)&flash:j%5==2?(void *)&images:j%5==3?(void *)&reset:(void *)&store)+1);
        CHECK(boot_target_sequence_bind(j%5==0?bad:&params,j%5==1?bad:&flash,j%5==2?bad:&images,j%5==3?bad:&reset,j%5==4?bad:&store)==-1);
        CHECK(!memcmp(&params,&saved_params,sizeof params) && !memcmp(&reset,&saved_reset,sizeof reset) && !memcmp(&store,&saved_store,sizeof store) && calls==0);
    }
    CHECK(boot_target_sequence_bind(&params,&flash,&images,&reset,&store)==0);
    boot_sequence_parameters_t expected=saved_params;
    expected.registers=&reset; expected.store=&store; expected.images=&images;
    expected.reader=boot_target_verify_read; expected.reader_context=&flash;
    CHECK(!memcmp(&params,&expected,sizeof params));
    boot_reset_register_adapter_t r=boot_target_reset_adapter(); boot_store_adapter_t s=boot_flash_store_adapter(&flash);
    CHECK(reset.context==NULL && reset.read==r.read && reset.write==r.write);
    CHECK(store.context==&flash && store.read==s.read && store.erase_page==s.erase_page && store.program_word==s.program_word && calls==0);
}
int main(int argc,char **argv) {
    CHECK(argc==2); fresh();
    if(!strcmp(argv[1],"safe-args")) safe_args();
    else if(!strcmp(argv[1],"physical-map")) physical_map();
    else if(!strcmp(argv[1],"init-clock")) init_clock();
    else if(!strcmp(argv[1],"images-publication")) images_publication();
    else { CHECK(!strcmp(argv[1],"bridge-bind")); bridge_bind(); }
    printf("ADAPTER_CASE_OK %s\n",argv[1]); return 0;
}
