#ifndef FTP_BOOT_FIXTURE_H
#define FTP_BOOT_FIXTURE_H
#include "lm3s6965/ftp_boot.h"
/* Real signed fixtures/verifier/controller/store, modeled registers only. */
ftp_boot_t *ftp_boot_fixture_init(bool trial);
size_t ftp_boot_fixture_package(uint8_t *out);
size_t ftp_boot_fixture_confirm(char *out);
void ftp_boot_fixture_fail(void);
boot_store_snapshot_t ftp_boot_fixture_metadata(void);
unsigned ftp_boot_fixture_commands(void);
#endif
