#include "lm3s6965/boot_verify.h"
#include "verify_fixtures.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
int main(void) {
    boot_manifest_t out, old; uint8_t wire[BOOT_MANIFEST_BYTES];
    memset(&out,0xa5,sizeof out); old=out;
#if defined(LM3S6965_BOOT_PHYSICAL_PRODUCTION) && LM3S6965_BOOT_PHYSICAL_PRODUCTION
    CHECK(boot_verify_manifest(fixture_manifest[0],160,0,NULL,NULL,&out)==BOOT_VERIFY_ERROR_SIGNATURE);
    CHECK(!memcmp(&out,&old,sizeof out));
#else
#error "This fixture is production-only; default signature behavior reuses shared sequence cases"
#endif
    memcpy(wire,fixture_manifest[0],sizeof wire); wire[96]^=1;
    CHECK(boot_verify_manifest(wire,160,0,NULL,NULL,&out)==BOOT_VERIFY_ERROR_SIGNATURE);
    CHECK(!memcmp(&out,&old,sizeof out));
    puts("ROOT_BEHAVIOR_OK"); return 0;
}
