#include "lm3s6965/boot_store.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lm3s6965/boot_format.h"

static int adapter_valid(const boot_store_adapter_t *a)
{
    return a != NULL && a->read != NULL && a->erase_page != NULL &&
           a->program_word != NULL;
}

static uint32_t page_address(unsigned page)
{
    return page == 0u ? BOOT_METADATA_PAGE0 : BOOT_METADATA_PAGE1;
}

boot_store_result_t boot_store_load(const boot_store_adapter_t *a,
                                    boot_store_snapshot_t *out)
{
    uint8_t pages[2][BOOT_METADATA_BYTES];
    boot_store_snapshot_t snapshot;
    boot_result_t result;
    if (!adapter_valid(a) || out == NULL) return BOOT_STORE_ARGUMENT;
    if (a->read(a->context, BOOT_METADATA_PAGE0, pages[0], sizeof pages[0]) != 0 ||
        a->read(a->context, BOOT_METADATA_PAGE1, pages[1], sizeof pages[1]) != 0)
        return BOOT_STORE_IO;
    result = boot_metadata_select(pages[0], sizeof pages[0], pages[1],
                                  sizeof pages[1], &snapshot.record, &snapshot.page);
    if (result == BOOT_ERROR_NO_RECORD) return BOOT_STORE_NO_RECORD;
    if (result == BOOT_ERROR_CONFLICT) return BOOT_STORE_CONFLICT;
    if (result != BOOT_OK) return BOOT_STORE_INVALID_RECORD;
    *out = snapshot;
    return BOOT_STORE_OK;
}

boot_store_result_t boot_store_replace(const boot_store_adapter_t *a,
                                       const boot_store_snapshot_t *expected,
                                       const boot_metadata_t *desired,
                                       boot_store_snapshot_t *out)
{
    uint8_t wanted[BOOT_METADATA_BYTES], old[BOOT_METADATA_BYTES];
    uint8_t current[BOOT_METADATA_BYTES], readback[BOOT_ERASE_BYTES];
    boot_store_snapshot_t snapshot;
    boot_store_result_t result;
    uint32_t next, destination;
    size_t i;
    if (!adapter_valid(a) || expected == NULL || desired == NULL || out == NULL ||
        expected->page > 1u) return BOOT_STORE_ARGUMENT;
    if (boot_metadata_encode(&expected->record, BOOT_COMMIT_VALID, old, sizeof old) != BOOT_OK ||
        boot_metadata_encode(desired, BOOT_COMMIT_ERASED, wanted, sizeof wanted) != BOOT_OK)
        return BOOT_STORE_INVALID_RECORD;
    if (boot_metadata_next_sequence(&expected->record, &next) != BOOT_OK)
        return BOOT_STORE_EXHAUSTED;
    if (desired->sequence != next) return BOOT_STORE_SEQUENCE;
    result = boot_store_load(a, &snapshot);
    if (result != BOOT_STORE_OK) return result;
    if (boot_metadata_encode(&snapshot.record, BOOT_COMMIT_VALID, current, sizeof current) != BOOT_OK)
        return BOOT_STORE_INVALID_RECORD;
    if (snapshot.page != expected->page || memcmp(old, current, sizeof old) != 0)
        return BOOT_STORE_STALE;
    destination = page_address(1u - snapshot.page);
    if (a->erase_page(a->context, destination) != 0) return BOOT_STORE_IO;
    if (a->read(a->context, destination, readback, sizeof readback) != 0) return BOOT_STORE_IO;
    for (i = 0; i < sizeof readback; ++i)
        if (readback[i] != UINT8_MAX) return BOOT_STORE_VERIFY;
    for (i = 0; i < BOOT_METADATA_BYTES - BOOT_PROGRAM_BYTES; i += BOOT_PROGRAM_BYTES)
    {
        uint32_t word = (uint32_t)wanted[i] | ((uint32_t)wanted[i+1u] << 8) |
                        ((uint32_t)wanted[i+2u] << 16) | ((uint32_t)wanted[i+3u] << 24);
        if (a->program_word(a->context, destination + (uint32_t)i, word) != 0)
            return BOOT_STORE_IO;
    }
    if (a->read(a->context, destination, readback, BOOT_METADATA_BYTES) != 0)
        return BOOT_STORE_IO;
    if (memcmp(wanted, readback, sizeof wanted) != 0) return BOOT_STORE_VERIFY;
    /* From this point the old page is physically preserved, but may no longer
     * be authoritative even if the callback reports failure. */
    if (a->program_word(a->context, destination + BOOT_METADATA_BYTES - BOOT_PROGRAM_BYTES,
                        BOOT_COMMIT_VALID) != 0) return BOOT_STORE_UNCERTAIN;
    memset(wanted + BOOT_METADATA_BYTES - BOOT_PROGRAM_BYTES, 0, BOOT_PROGRAM_BYTES);
    if (a->read(a->context, destination, readback, BOOT_METADATA_BYTES) != 0 ||
        memcmp(wanted, readback, sizeof wanted) != 0) return BOOT_STORE_UNCERTAIN;
    result = boot_store_load(a, &snapshot);
    if (result != BOOT_STORE_OK || snapshot.page != 1u - expected->page ||
        boot_metadata_encode(&snapshot.record, BOOT_COMMIT_VALID, current, sizeof current) != BOOT_OK ||
        memcmp(wanted, current, sizeof wanted) != 0) return BOOT_STORE_UNCERTAIN;
    *out = snapshot;
    return BOOT_STORE_OK;
}
