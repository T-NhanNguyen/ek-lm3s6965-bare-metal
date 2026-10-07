/* Synchronous single-file storage contract. See ftp-relocation-contract.md. */
#ifndef LM3S6965_FTP_STORAGE_H
#define LM3S6965_FTP_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FTP_STORAGE_NAME_MAX 63u

typedef enum
{
    FTP_STORAGE_OK = 0,
    FTP_STORAGE_CAPACITY,
    FTP_STORAGE_ERROR,
    FTP_STORAGE_INVALID_ARGUMENT,
    FTP_STORAGE_INVALID_STATE
} ftp_storage_result_t;

typedef struct
{
    const uint8_t *data;
    size_t length;
    bool exists;
} ftp_storage_view_t;

/* State: one committed file and at most one active upload per context.
 * Timing: synchronous, bounded, serialized, non-reentrant; no pending results.
 * Effects: only clear and successful commit invalidate a committed read view.
 * No allocation, filesystem, flash, transport, or asynchronous storage support.
 * Every callback is required; context must be non-NULL and initialized.
 * Configuration is immutable while bound. Basename is 1..63 ASCII bytes
 * (FTP_STORAGE_NAME_MAX), only A-Z, a-z, 0-9, '.', '_', '-'; neither "." nor
 * ".." is allowed. */
typedef struct
{
    /* Exact, case-sensitive basename or '/' + basename. No trimming/decoding.
     * Input is a span, not necessarily NUL-terminated; NULL is never a match.
     * Empty, oversized, embedded-NUL, and other path spans return false. */
    bool (*matches_name)(void *context, const char *name, size_t length);
    /* Boot/explicit reset only. Infallible; removes file and active upload.
     * Logical clear, not secure erasure; retains backend configuration. */
    void (*clear)(void *context);
    /* Infallible for required non-NULL view. Absent: {NULL, 0, false}.
     * Existing empty file: non-NULL data, zero length, exists=true.
     * Otherwise data covers length contiguous readable bytes. Stable through
     * begin, append, abort, and failed operations; never write through data. */
    void (*inspect)(void *context, ftp_storage_view_t *view);
    /* Starts empty staging, without changing committed data/existence/view.
     * Already active: INVALID_STATE, no change. ERROR is recoverable. */
    ftp_storage_result_t (*begin)(void *context);
    /* Active upload required. Copy the whole chunk or none; never retain it.
     * NULL bytes allowed only for length=0 (successful no-op when active).
     * CAPACITY means insufficient remaining space; ERROR is another failure.
     * Any non-OK result leaves both staging and committed state unchanged.
     * Input must not overlap backend-owned buffers or mutable state. */
    ftp_storage_result_t (*append)(void *context, const uint8_t *bytes,
                                   size_t length);
    /* Infallible, idempotent; ends upload and discards staging, not committed.
     * Bytes need not be erased. Safe after a failed begin/append/commit. */
    void (*abort)(void *context);
    /* Active valid upload required, including empty. OK atomically replaces
     * file, sets exists=true, and ends upload. Failure preserves committed
     * state/view and leaves staging active for abort; no partial publication.
     * Protocol alone decides when EOF + accepted graceful close permit this. */
    ftp_storage_result_t (*commit)(void *context);
} ftp_storage_ops_t;

typedef struct
{
    const ftp_storage_ops_t *ops;
    void *context;
    /* 1..min(UINT_MAX, UINT32_MAX), also representable in size_t.
     * inspect.length and staging length must never exceed capacity.
     * This bounds the core's unsigned SIZE value to at most ten digits without
     * assuming unsigned int width. The binding validates before any callback. */
    size_t capacity;
} ftp_storage_t;

#endif
