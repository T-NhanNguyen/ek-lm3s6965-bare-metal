#include "flash_model.h"
#include <stdlib.h>
#include <string.h>

static size_t offset(uint32_t address, size_t size)
{
    if (address < BOOT_METADATA_PAGE0 || address >= BOOT_METADATA_END ||
        size > (size_t)(BOOT_METADATA_END - address)) abort();
    return (size_t)(address - BOOT_METADATA_PAGE0);
}
static int cut(flash_model_t *m)
{
    ++m->calls;
    return m->stopped || m->calls == m->cut_call;
}
static int read_bytes(void *context, uint32_t address, uint8_t *bytes, size_t size)
{
    flash_model_t *m = context;
    size_t start = offset(address, size);
    int fault = cut(m);
    memcpy(bytes, m->bytes + start, size);
    if (fault && m->corrupt_read) { bytes[0] ^= 1u; return 0; }
    if (fault) { m->stopped = 1; return -1; }
    return 0;
}
static int erase(void *context, uint32_t address)
{
    flash_model_t *m = context;
    size_t start = offset(address, BOOT_ERASE_BYTES), bit;
    int fault = cut(m);
    if (address != BOOT_METADATA_PAGE0 && address != BOOT_METADATA_PAGE1) abort();
    ++m->erases;
    for (bit = 0; bit < BOOT_ERASE_BYTES * 8u; ++bit)
    {
        if (fault && !m->fail_after && bit >= m->cut_bits) break;
        m->bytes[start + bit / 8u] |= (uint8_t)(1u << (bit % 8u));
    }
    if (fault) { m->stopped = 1; return -1; }
    return 0;
}
static int program(void *context, uint32_t address, uint32_t word)
{
    flash_model_t *m = context;
    size_t start = offset(address, 4u), bit;
    int fault = cut(m);
    if (address % 4u != 0u) abort();
    ++m->writes;
    for (bit = 0; bit < 32u; ++bit)
    {
        uint8_t mask = (uint8_t)(1u << (bit % 8u));
        if (((word >> bit) & 1u) != 0u && (m->bytes[start + bit / 8u] & mask) == 0u) abort();
        if (fault && !m->fail_after && bit >= m->cut_bits) break;
        if (((word >> bit) & 1u) == 0u && !(fault && m->silent_program))
            m->bytes[start + bit / 8u] &= (uint8_t)~mask;
    }
    if (fault && !m->silent_program) { m->stopped = 1; return -1; }
    return 0;
}
void flash_model_init(flash_model_t *m)
{
    memset(m, 0, sizeof *m);
    memset(m->bytes, 0xff, sizeof m->bytes);
    m->cut_call = SIZE_MAX;
}
boot_store_adapter_t flash_model_adapter(flash_model_t *m)
{
    boot_store_adapter_t a = {m, read_bytes, erase, program};
    return a;
}
