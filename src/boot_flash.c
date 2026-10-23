#include "lm3s6965/boot_flash.h"

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "lm3s6965/boot_format.h"
#include "lm3s6965/boot_store.h"

#define FLASH_KEY UINT32_C(0xa4420000)
#define FLASH_WRITE UINT32_C(1)
#define FLASH_ERASE UINT32_C(2)
#define FLASH_COMMANDS UINT32_C(15)
#define FLASH_STATUS UINT32_C(3)

static int range(uint32_t address, size_t size, uint32_t first, uint32_t end)
{
    return address >= first && address < end && size != 0u &&
           size <= (size_t)(end - address);
}

static boot_flash_result_t enter(boot_flash_t *f)
{
    if (f == NULL) return BOOT_FLASH_ARGUMENT;
    if (atomic_flag_test_and_set_explicit(&f->entered, memory_order_acquire))
        return BOOT_FLASH_BUSY;
    return BOOT_FLASH_OK;
}

static boot_flash_result_t leave(boot_flash_t *f, boot_flash_result_t result)
{
    if (f->blocked && f->blocked_result == BOOT_FLASH_OK && result != BOOT_FLASH_OK)
        f->blocked_result = result;
    atomic_flag_clear_explicit(&f->entered, memory_order_release);
    return result;
}

static int rr(boot_flash_t *f, uint32_t address, uint32_t *v)
{
    return f->io.read_register(f->io.context, address, v);
}

static int wr(boot_flash_t *f, uint32_t address, uint32_t v)
{
    return f->io.write_register(f->io.context, address, v);
}

static boot_flash_result_t idle(boot_flash_t *f, uint32_t polls)
{
    uint32_t i, value;
    for (i = 0; i < polls; ++i)
    {
        if (rr(f, BOOT_FLASH_FMC, &value) != 0) return BOOT_FLASH_IO;
        if ((value & FLASH_COMMANDS) == 0u) return BOOT_FLASH_OK;
    }
    return BOOT_FLASH_TIMEOUT;
}

static boot_flash_result_t clear_status(boot_flash_t *f)
{
    uint32_t value;
    /* STE: clear both stale raw sources via W1C, never write the RO FCRIS. */
    if (wr(f, BOOT_FLASH_FCMISC, FLASH_STATUS) != 0 ||
        rr(f, BOOT_FLASH_FCRIS, &value) != 0) return BOOT_FLASH_IO;
    return (value & FLASH_STATUS) == 0u ? BOOT_FLASH_OK : BOOT_FLASH_STATUS;
}

boot_flash_result_t boot_flash_init(boot_flash_t *f, const boot_flash_io_t *io,
                                   uint32_t mhz, uint32_t polls)
{
    if (f == NULL || io == NULL || io->read_register == NULL ||
        io->write_register == NULL || io->read_flash == NULL ||
        mhz == 0u || mhz > 50u || polls == 0u) return BOOT_FLASH_ARGUMENT;
    f->io = *io;
    f->mhz = mhz;
    f->polls = polls;
    f->blocked = 0u;
    f->blocked_result = BOOT_FLASH_OK;
    f->entered = (atomic_flag)ATOMIC_FLAG_INIT;
    return BOOT_FLASH_OK;
}

boot_flash_result_t boot_flash_recover(boot_flash_t *f)
{
    boot_flash_result_t result = enter(f);
    if (result != BOOT_FLASH_OK) return result;
    f->blocked = 1u;
    result = idle(f, f->polls);
    if (result == BOOT_FLASH_OK) result = clear_status(f);
    if (result == BOOT_FLASH_OK) {
        f->blocked = 0u;
        f->blocked_result = BOOT_FLASH_OK;
    }
    return leave(f, result);
}

boot_flash_result_t boot_flash_read(boot_flash_t *f, uint32_t address,
                                   uint8_t *bytes, size_t size)
{
    boot_flash_result_t result;
    if (bytes == NULL || !range(address, size, BOOT_METADATA_PAGE0, BOOT_SLOT_B_END))
        return BOOT_FLASH_ARGUMENT;
    result = enter(f);
    if (result != BOOT_FLASH_OK) return result;
    if (f->blocked != 0u) return leave(f, BOOT_FLASH_BLOCKED);
    result = idle(f, 1u);
    if (result != BOOT_FLASH_OK) f->blocked = 1u;
    else if (f->io.read_flash(f->io.context, address, bytes, size) != 0)
        result = BOOT_FLASH_IO;
    return leave(f, result);
}

static boot_flash_result_t modify(boot_flash_t *f, uint32_t address,
                                   uint32_t word, uint32_t command)
{
    uint8_t bytes[BOOT_ERASE_BYTES];
    uint32_t value;
    size_t i, size = command == FLASH_WRITE ? BOOT_PROGRAM_BYTES : BOOT_ERASE_BYTES;
    boot_flash_result_t result;
    if (!range(address, size, BOOT_METADATA_PAGE0, BOOT_SLOT_B_END) ||
        address % (uint32_t)size != 0u) return BOOT_FLASH_ARGUMENT;
    result = enter(f);
    if (result != BOOT_FLASH_OK) return result;
    if (f->blocked != 0u) return leave(f, BOOT_FLASH_BLOCKED);
    result = idle(f, 1u);
    if (result != BOOT_FLASH_OK)
    {
        f->blocked = 1u;
        return leave(f, result);
    }
    if (rr(f, BOOT_FLASH_USECRL, &value) != 0) return leave(f, BOOT_FLASH_IO);
    if ((value & UINT32_C(255)) != f->mhz - 1u) return leave(f, BOOT_FLASH_CLOCK);
    if (command == FLASH_WRITE)
    {
        if (f->io.read_flash(f->io.context, address, bytes, size) != 0)
            return leave(f, BOOT_FLASH_IO);
        value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
        if ((value & word) != word) return leave(f, BOOT_FLASH_BITS);
    }
    result = clear_status(f);
    if (result != BOOT_FLASH_OK) return leave(f, result);
    /* STE: datasheet p264: data then address, keyed FMC is LAST, one bit only. */
    if ((command == FLASH_WRITE && wr(f, BOOT_FLASH_FMD, word) != 0) ||
        wr(f, BOOT_FLASH_FMA, address) != 0) return leave(f, BOOT_FLASH_IO);
    /* Even a failed command callback may have launched an operation. */
    f->blocked = 1u;
    if (wr(f, BOOT_FLASH_FMC, FLASH_KEY | command) != 0)
        return leave(f, BOOT_FLASH_IO);
    result = idle(f, f->polls);
    if (result != BOOT_FLASH_OK) return leave(f, result);
    if (rr(f, BOOT_FLASH_FCRIS, &value) != 0) return leave(f, BOOT_FLASH_IO);
    if ((value & 1u) != 0u) return leave(f, BOOT_FLASH_PROTECTED);
    if ((value & 2u) == 0u) return leave(f, BOOT_FLASH_STATUS);
    if (f->io.read_flash(f->io.context, address, bytes, size) != 0)
        return leave(f, BOOT_FLASH_IO);
    for (i = 0; i < size; ++i)
    {
        uint8_t expected = command == FLASH_WRITE ?
            (uint8_t)(word >> (8u * (unsigned)i)) : UINT8_MAX;
        if (bytes[i] != expected) return leave(f, BOOT_FLASH_VERIFY);
    }
    f->blocked = 0u;
    return leave(f, BOOT_FLASH_OK);
}

boot_flash_result_t boot_flash_program(boot_flash_t *f, uint32_t address, uint32_t word)
{
    return modify(f, address, word, FLASH_WRITE);
}

boot_flash_result_t boot_flash_erase(boot_flash_t *f, uint32_t address)
{
    return modify(f, address, 0u, FLASH_ERASE);
}

static int store_read(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    if (!range(address, size, BOOT_METADATA_PAGE0, BOOT_SLOT_A_BASE)) return -1;
    return boot_flash_read(context, address, bytes, size) == BOOT_FLASH_OK ? 0 : -1;
}

static int store_erase(void *context, uint32_t address)
{
    if (address != BOOT_METADATA_PAGE0 && address != BOOT_METADATA_PAGE1) return -1;
    return boot_flash_erase(context, address) == BOOT_FLASH_OK ? 0 : -1;
}

static int store_program(void *context, uint32_t address, uint32_t word)
{
    if (!range(address, BOOT_PROGRAM_BYTES, BOOT_METADATA_PAGE0, BOOT_SLOT_A_BASE)) return -1;
    return boot_flash_program(context, address, word) == BOOT_FLASH_OK ? 0 : -1;
}

boot_store_adapter_t boot_flash_store_adapter(boot_flash_t *f)
{
    boot_store_adapter_t adapter = { f, store_read, store_erase, store_program };
    return adapter;
}

boot_flash_result_t boot_flash_erase_begin(boot_flash_erase_plan_t *plan,
    const boot_store_snapshot_t *invalidated, uint32_t running, uint32_t destination)
{
    uint8_t encoded[BOOT_METADATA_BYTES];
    const uint8_t empty[48] = {0};
    const boot_metadata_t *record;
    if (plan == NULL || invalidated == NULL || invalidated->page > 1u ||
        running > BOOT_SLOT_B || destination > BOOT_SLOT_B || running == destination)
        return BOOT_FLASH_ARGUMENT;
    record = &invalidated->record;
    if (boot_metadata_encode(record, BOOT_COMMIT_VALID, encoded, sizeof encoded) != BOOT_OK ||
        record->current_slot != running || record->candidate_slot != BOOT_SLOT_NONE ||
        record->previous_slot == destination || record->intent_slot == destination ||
        (record->confirmed_mask & (UINT32_C(1) << running)) == 0u ||
        (record->confirmed_mask & (UINT32_C(1) << destination)) != 0u ||
        memcmp(encoded + 32u + 48u * destination, empty, sizeof empty) != 0)
        return BOOT_FLASH_ARGUMENT;
    plan->destination = destination;
    plan->page = plan->failed = 0u;
    plan->entered = (atomic_flag)ATOMIC_FLAG_INIT;
    return BOOT_FLASH_OK;
}

boot_flash_result_t boot_flash_erase_step(boot_flash_t *f, boot_flash_erase_plan_t *plan)
{
    boot_flash_result_t result;
    uint32_t base, manifest, address;
    if (f == NULL || plan == NULL) return BOOT_FLASH_ARGUMENT;
    if (atomic_flag_test_and_set_explicit(&plan->entered, memory_order_acquire))
        return BOOT_FLASH_BUSY;
    if (plan->failed != 0u) result = BOOT_FLASH_BLOCKED;
    else if (plan->destination > BOOT_SLOT_B || plan->page > BOOT_SLOT_BYTES / BOOT_ERASE_BYTES)
        result = BOOT_FLASH_ARGUMENT;
    else if (plan->page == BOOT_SLOT_BYTES / BOOT_ERASE_BYTES) result = BOOT_FLASH_DONE;
    else
    {
        base = plan->destination == BOOT_SLOT_A ? BOOT_SLOT_A_BASE : BOOT_SLOT_B_BASE;
        manifest = plan->destination == BOOT_SLOT_A ? BOOT_SLOT_A_MANIFEST : BOOT_SLOT_B_MANIFEST;
        address = plan->page == 0u ? manifest : base + (plan->page - 1u) * BOOT_ERASE_BYTES;
        /* STE: no payload destruction until the old manifest erase verified. */
        result = boot_flash_erase(f, address);
        if (result == BOOT_FLASH_OK) ++plan->page;
    }
    if (result != BOOT_FLASH_OK && result != BOOT_FLASH_DONE) plan->failed = 1u;
    atomic_flag_clear_explicit(&plan->entered, memory_order_release);
    return result;
}
