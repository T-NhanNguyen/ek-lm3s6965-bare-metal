#include "lm3s6965/boot_target_io.h"

#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/boot_flash.h"
#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_reset_handler.h"
#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_sequence.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

#define TARGET_RESC UINT32_C(0x400fe05c)
#define TARGET_MHZ UINT32_C(1000000)

static int aligned(const void *pointer, size_t alignment)
{
    return pointer != NULL && (uintptr_t)pointer % alignment == 0u;
}

static int span(uint32_t address, size_t size, uint32_t first, uint32_t end)
{
    return address >= first && address < end && size != 0u &&
           size <= (size_t)(end - address);
}

static int reset_read(void *context, uint32_t *flags)
{
    (void)context;
    if (!aligned(flags, _Alignof(uint32_t))) return -1;
    *flags = *(volatile const uint32_t *)(uintptr_t)TARGET_RESC;
    return 0;
}

static int reset_write(void *context, uint32_t flags)
{
    (void)context;
    *(volatile uint32_t *)(uintptr_t)TARGET_RESC = flags;
    return 0;
}

boot_reset_register_adapter_t boot_target_reset_adapter(void)
{
    const boot_reset_register_adapter_t adapter = { NULL, reset_read, reset_write };
    return adapter;
}

static int register_read(void *context, uint32_t address, uint32_t *value)
{
    (void)context;
    if (!aligned(value, _Alignof(uint32_t))) return -1;
    switch (address)
    {
        case BOOT_FLASH_FMA:
        case BOOT_FLASH_FMD:
        case BOOT_FLASH_FMC:
        case BOOT_FLASH_FCRIS:
        case BOOT_FLASH_FCMISC:
        case BOOT_FLASH_USECRL:
            *value = *(volatile const uint32_t *)(uintptr_t)address;
            return 0;
        default:
            return -1;
    }
}

static int register_write(void *context, uint32_t address, uint32_t value)
{
    (void)context;
    switch (address)
    {
        case BOOT_FLASH_FMA:
        case BOOT_FLASH_FMD:
        case BOOT_FLASH_FMC:
        case BOOT_FLASH_FCMISC:
            *(volatile uint32_t *)(uintptr_t)address = value;
            return 0;
        default:
            return -1;
    }
}

static int flash_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    size_t i;
    (void)context;
    if (bytes == NULL ||
        !span(address, size, BOOT_METADATA_PAGE0, BOOT_SLOT_B_END)) return -1;
    for (i = 0; i < size; ++i)
        bytes[i] = *(volatile const uint8_t *)(uintptr_t)(address + (uint32_t)i);
    return 0;
}

boot_flash_io_t boot_target_flash_io(void)
{
    const boot_flash_io_t io = { NULL, register_read, register_write, flash_read };
    return io;
}

boot_flash_result_t boot_target_flash_init(boot_flash_t *flash,
    uint32_t actual_hz, uint32_t poll_limit)
{
    uint32_t timing, mhz;
    boot_flash_io_t io;
    if (!aligned(flash, _Alignof(boot_flash_t)) || poll_limit == 0u ||
        actual_hz < TARGET_MHZ || actual_hz > 50u * TARGET_MHZ ||
        actual_hz % TARGET_MHZ != 0u) return BOOT_FLASH_ARGUMENT;
    mhz = actual_hz / TARGET_MHZ;
    io = boot_target_flash_io();
    if (io.read_register(io.context, BOOT_FLASH_USECRL, &timing) != 0)
        return BOOT_FLASH_IO;
    if ((timing & UINT32_C(255)) != mhz - 1u) return BOOT_FLASH_CLOCK;
    return boot_flash_init(flash, &io, mhz, poll_limit);
}

boot_flash_result_t boot_target_images_read(boot_flash_t *flash,
    boot_select_images_t *out)
{
    boot_select_images_t images;
    boot_flash_result_t result;
    if (!aligned(flash, _Alignof(boot_flash_t)) ||
        !aligned(out, _Alignof(boot_select_images_t))) return BOOT_FLASH_ARGUMENT;
    result = boot_flash_read(flash, BOOT_SLOT_A_MANIFEST,
                             images.wire[BOOT_SLOT_A], BOOT_MANIFEST_BYTES);
    if (result != BOOT_FLASH_OK) return result;
    result = boot_flash_read(flash, BOOT_SLOT_B_MANIFEST,
                             images.wire[BOOT_SLOT_B], BOOT_MANIFEST_BYTES);
    if (result != BOOT_FLASH_OK) return result;
    images.size[BOOT_SLOT_A] = BOOT_MANIFEST_BYTES;
    images.size[BOOT_SLOT_B] = BOOT_MANIFEST_BYTES;
    *out = images;
    return BOOT_FLASH_OK;
}

size_t boot_target_verify_read(void *context, uint32_t address,
    uint8_t *buffer, size_t requested)
{
    if (!aligned(context, _Alignof(boot_flash_t)) || buffer == NULL ||
        requested > BOOT_VERIFY_CHUNK_BYTES ||
        (!span(address, requested, BOOT_SLOT_A_BASE, BOOT_SLOT_A_MANIFEST) &&
         !span(address, requested, BOOT_SLOT_B_BASE, BOOT_SLOT_B_MANIFEST))) return 0u;
    return boot_flash_read(context, address, buffer, requested) == BOOT_FLASH_OK ?
           requested : 0u;
}

int boot_target_sequence_bind(boot_sequence_parameters_t *parameters,
    boot_flash_t *flash, const boot_select_images_t *images,
    boot_reset_register_adapter_t *registers, boot_store_adapter_t *store)
{
    if (!aligned(parameters, _Alignof(boot_sequence_parameters_t)) ||
        !aligned(flash, _Alignof(boot_flash_t)) ||
        !aligned(images, _Alignof(boot_select_images_t)) ||
        !aligned(registers, _Alignof(boot_reset_register_adapter_t)) ||
        !aligned(store, _Alignof(boot_store_adapter_t))) return -1;
    *registers = boot_target_reset_adapter();
    *store = boot_flash_store_adapter(flash);
    parameters->registers = registers;
    parameters->store = store;
    parameters->images = images;
    parameters->reader = boot_target_verify_read;
    parameters->reader_context = flash;
    return 0;
}
