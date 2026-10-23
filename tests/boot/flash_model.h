#ifndef BOOT_FLASH_MODEL_H
#define BOOT_FLASH_MODEL_H
#include "lm3s6965/boot_store.h"
typedef struct
{
    uint8_t bytes[2u * BOOT_ERASE_BYTES];
    size_t calls, cut_call, cut_bits;
    unsigned writes, erases;
    int stopped, corrupt_read, silent_program, fail_after;
} flash_model_t;
void flash_model_init(flash_model_t *model);
boot_store_adapter_t flash_model_adapter(flash_model_t *model);
#endif
