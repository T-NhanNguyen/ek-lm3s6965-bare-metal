#include "ftp_firmware_receive.h"

#include <stddef.h>

#include "lm3s6965/ftp_core.h"

/* Binding wrapper only. The receiver, controller, publication, and sources
 * stay caller-owned (see header); only this ftp_boot_t is module-owned, and
 * it is zero-initialized once before publication per ftp_boot_init. */
static ftp_boot_t binding;
static bool bound;

bool ftp_firmware_receive_bind(const ftp_boot_config_t *config)
{
    if (bound) {
        return true;
    }
    if (config == NULL) {
        return false;
    }
    if (!ftp_boot_init(&binding, config)) {
        return false;
    }
    if (!ftp_core_bind_operation(ftp_boot_operation(&binding))) {
        return false;
    }
    bound = true;
    return true;
}

__attribute__((weak)) const ftp_boot_config_t *ftp_firmware_receive_production_config(void)
{
    /* This example ships no production flash controller, running evidence,
     * or metadata sources, so there is nothing truthful to bind. Production
     * overrides this hook; until then the caller keeps RAM-only hello. */
    return NULL;
}
