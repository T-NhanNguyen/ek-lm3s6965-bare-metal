/* Caller-owned double-buffered RAM file; no heap, filesystem, or flash. */
#ifndef LM3S6965_RAM_FILE_H
#define LM3S6965_RAM_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lm3s6965/ftp_storage.h"

/* Caller allocates this object; fields are private to the service despite
 * being visible for static/stack allocation. Do not copy an initialized object,
 * edit its fields, or access its buffers directly while it is in use. */
typedef struct
{
    ftp_storage_t storage;
    uint8_t *buffers[2];
    const char *basename;
    size_t committed_length;
    size_t staging_length;
    unsigned committed;
    bool exists;
    bool uploading;
} ram_file_t;

/* State: successful init establishes an absent file and no active upload.
 * Timing: synchronous, bounded, foreground/serialized, no allocation.
 * Effects: validates everything before changing file; never writes buffers.
 * file, buffer_a, buffer_b, and basename must be non-NULL. Each buffer is a
 * distinct writable contiguous region of at least capacity bytes. Their
 * capacity-byte ranges must not overlap each other, the sizeof(*file)-byte
 * object range, or the borrowed basename bytes including NUL. The basename
 * range must also not overlap the object range; reject every such pairing.
 * capacity is 1..min(UINT_MAX, UINT32_MAX), representable in size_t. Validate
 * ranges using uintptr_t addresses with overflow checks before forming range
 * ends; do not order/subtract unrelated C pointers. Reject overlap or address
 * wrap before mutation. Invalid configuration returns false, leaving file and
 * buffers unchanged. Basename is NUL-terminated within
 * FTP_STORAGE_NAME_MAX + 1 bytes, follows ftp_storage.h's character rules,
 * and is borrowed, not copied. Valid allocation extents, file alignment and
 * writability, buffer writability, and basename readability through NUL (or
 * the scan bound if no NUL) are caller preconditions, not pointer-checkable.
 * Keep file, buffers, and immutable basename alive until unbound and every
 * transport/read-view reference is released. Never reinitialize while bound
 * or with outstanding references. A successful reinit logically discards the
 * old file. Init is not a secure erase. See ftp-relocation-contract.md. */
bool ram_file_init(ram_file_t *file, uint8_t *buffer_a, uint8_t *buffer_b,
                   size_t capacity, const char *basename);

/* Borrowed immutable descriptor valid for file's initialized lifetime.
 * NULL file returns NULL; any non-NULL file must have been initialized.
 * Invoke operations through this descriptor; no additional RAM singleton. */
const ftp_storage_t *ram_file_storage(const ram_file_t *file);

#endif
