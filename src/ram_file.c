/* Caller-owned synchronous RAM storage; publication swaps buffer roles. */
#include "lm3s6965/ram_file.h"

#include <limits.h>
#include <string.h>

typedef struct
{
    uintptr_t start;
    uintptr_t end;
} ram_range_t;

/* Half-open integer ranges: never compare unrelated C pointers. */
static bool range_make(const void *pointer, size_t length, ram_range_t *range)
{
    uintptr_t start = (uintptr_t)pointer;
    if ((uintmax_t)length > (uintmax_t)(UINTPTR_MAX - start))
    {
        return false;
    }
    range->start = start;
    range->end = start + (uintptr_t)length;
    return true;
}

static bool ranges_overlap(ram_range_t a, ram_range_t b)
{
    return a.start < b.end && b.start < a.end;
}

static bool name_character(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') ||
           byte == '.' || byte == '_' || byte == '-';
}

static bool matches_name(void *context, const char *name, size_t length)
{
    const ram_file_t *file = context;
    size_t basename_length;
    if (name == NULL || length == 0u || length > FTP_STORAGE_NAME_MAX + 1u)
    {
        return false;
    }
    if (name[0] == '/')
    {
        ++name;
        --length;
    }
    for (basename_length = 0u; basename_length < FTP_STORAGE_NAME_MAX;
         ++basename_length)
    {
        if (file->basename[basename_length] == '\0')
        {
            break;
        }
    }
    return length == basename_length &&
           memcmp(name, file->basename, length) == 0;
}

static void clear(void *context)
{
    ram_file_t *file = context;
    file->committed_length = 0u;
    file->staging_length = 0u;
    file->exists = false;
    file->uploading = false;
}

static void inspect(void *context, ftp_storage_view_t *view)
{
    const ram_file_t *file = context;
    view->data = file->exists ? file->buffers[file->committed] : NULL;
    view->length = file->committed_length;
    view->exists = file->exists;
}

static ftp_storage_result_t begin(void *context)
{
    ram_file_t *file = context;
    if (file->uploading)
    {
        return FTP_STORAGE_INVALID_STATE;
    }
    file->staging_length = 0u;
    file->uploading = true;
    return FTP_STORAGE_OK;
}

static ftp_storage_result_t append(void *context, const uint8_t *bytes,
                                   size_t length)
{
    ram_file_t *file = context;
    ram_range_t source;
    ram_range_t owned;
    unsigned i;
    if (!file->uploading)
    {
        return FTP_STORAGE_INVALID_STATE;
    }
    if (length == 0u)
    {
        return FTP_STORAGE_OK;
    }
    if (bytes == NULL || !range_make(bytes, length, &source))
    {
        return FTP_STORAGE_INVALID_ARGUMENT;
    }
    /* Even the old committed view is forbidden by the frozen input contract.
     * Check whole buffers, not only the destination slice, before memcpy. */
    for (i = 0u; i < 2u; ++i)
    {
        if (!range_make(file->buffers[i], file->storage.capacity, &owned) ||
            ranges_overlap(source, owned))
        {
            return FTP_STORAGE_INVALID_ARGUMENT;
        }
    }
    if (!range_make(file, sizeof(*file), &owned) ||
        ranges_overlap(source, owned))
    {
        return FTP_STORAGE_INVALID_ARGUMENT;
    }
    if (length > file->storage.capacity - file->staging_length)
    {
        return FTP_STORAGE_CAPACITY;
    }
    memcpy(file->buffers[file->committed ^ 1u] + file->staging_length,
           bytes, length);
    file->staging_length += length;
    return FTP_STORAGE_OK;
}

static void abort_upload(void *context)
{
    ram_file_t *file = context;
    file->staging_length = 0u;
    file->uploading = false;
}

static ftp_storage_result_t commit(void *context)
{
    ram_file_t *file = context;
    if (!file->uploading)
    {
        return FTP_STORAGE_INVALID_STATE;
    }
    file->committed ^= 1u;
    file->committed_length = file->staging_length;
    file->exists = true;
    file->staging_length = 0u;
    file->uploading = false;
    return FTP_STORAGE_OK;
}

static const ftp_storage_ops_t ram_ops = {
    matches_name, clear, inspect, begin, append, abort_upload, commit
};

bool ram_file_init(ram_file_t *file, uint8_t *buffer_a, uint8_t *buffer_b,
                   size_t capacity, const char *basename)
{
    ram_range_t ranges[4];
    size_t name_length;
    unsigned i;
    unsigned j;
    if (file == NULL || buffer_a == NULL || buffer_b == NULL ||
        basename == NULL || capacity == 0u)
    {
        return false;
    }
#if SIZE_MAX > UINT_MAX
    if (capacity > UINT_MAX)
    {
        return false;
    }
#endif
#if SIZE_MAX > UINT32_MAX
    if (capacity > UINT32_MAX)
    {
        return false;
    }
#endif
    if (!range_make(file, sizeof(*file), &ranges[0]) ||
        !range_make(buffer_a, capacity, &ranges[1]) ||
        !range_make(buffer_b, capacity, &ranges[2]))
    {
        return false;
    }
    for (name_length = 0u; name_length <= FTP_STORAGE_NAME_MAX; ++name_length)
    {
        /* Check before reading; readability itself is a caller precondition. */
        if (!range_make(basename, name_length + 1u, &ranges[3]))
        {
            return false;
        }
        if (basename[name_length] == '\0')
        {
            break;
        }
        if (!name_character((unsigned char)basename[name_length]))
        {
            return false;
        }
    }
    if (name_length == 0u || name_length > FTP_STORAGE_NAME_MAX ||
        (name_length == 1u && basename[0] == '.') ||
        (name_length == 2u && basename[0] == '.' && basename[1] == '.'))
    {
        return false;
    }
    for (i = 0u; i < 4u; ++i)
    {
        for (j = i + 1u; j < 4u; ++j)
        {
            if (ranges_overlap(ranges[i], ranges[j]))
            {
                return false;
            }
        }
    }
    /* No object or buffer writes until every validation has succeeded. */
    file->storage.ops = &ram_ops;
    file->storage.context = file;
    file->storage.capacity = capacity;
    file->buffers[0] = buffer_a;
    file->buffers[1] = buffer_b;
    file->basename = basename;
    file->committed_length = 0u;
    file->staging_length = 0u;
    file->committed = 0u;
    file->exists = false;
    file->uploading = false;
    return true;
}

const ftp_storage_t *ram_file_storage(const ram_file_t *file)
{
    return file == NULL ? NULL : &file->storage;
}
