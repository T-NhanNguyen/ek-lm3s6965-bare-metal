#ifndef LM3S6965_BOOT_DIAG_PUBLICATION_H
#define LM3S6965_BOOT_DIAG_PUBLICATION_H

#include <stdbool.h>
#include <stdint.h>

#include "lm3s6965/boot_diag.h"

/* One zero-initialized owner per block, privileged serialized foreground only.
 * Owner (including its sidecar) must not overlap the 64-byte port. Caller
 * inputs/outputs must be disjoint from owner/port and each other. Host callback
 * is bounded to indices 0..15; false may follow an effect.
 * NULL callback selects aligned volatile uint32 SRAM access (infallible).
 * Callbacks must be truthful and cannot write outside the requested word.
 * No retries. No live SWD atomicity, no validity-double-check ABA guarantee. */
typedef bool (*boot_diag_word_fn)(void *, unsigned, bool, uint32_t *);
typedef void (*boot_diag_barrier_fn)(void *);
typedef struct {
    volatile uint32_t *words;
    boot_diag_word_fn word; /* host model only; target must use NULL */
    boot_diag_barrier_fn barrier; /* host trace only */
    void *user;
    boot_diag_context_t context;
    bool busy, associated;
} boot_diag_publication_t;
typedef enum {
    BOOT_DIAG_PUBLICATION_OK, BOOT_DIAG_PUBLICATION_ARGUMENT,
    BOOT_DIAG_PUBLICATION_ISR, BOOT_DIAG_PUBLICATION_BUSY,
    BOOT_DIAG_PUBLICATION_INVALID, BOOT_DIAG_PUBLICATION_UNASSOCIATED,
    BOOT_DIAG_PUBLICATION_READ, BOOT_DIAG_PUBLICATION_UNCERTAIN,
    BOOT_DIAG_PUBLICATION_CAPTURE
} boot_diag_publication_result_t;
/* Boot deliberately replaces even a valid old block; flags are raw capture. */
boot_diag_publication_result_t boot_diag_publication_boot(
    boot_diag_publication_t *, uint32_t captured_flags);
/* Adopt NEVER writes RAM. Context MUST be trustworthy same-record associated
 * metadata, not guessed from SRAM or replayed from a previous boot. False
 * trusted rejects before reading. Availability is not serialized. */
boot_diag_publication_result_t boot_diag_publication_adopt(
    boot_diag_publication_t *, const boot_diag_context_t *, bool trusted);
/* Only stage/health change; every other word/evidence availability retained. */
boot_diag_publication_result_t boot_diag_publication_progress(
    boot_diag_publication_t *, uint32_t stage, uint32_t health);
/* Serialized foreground read; outputs unchanged on all errors. */
boot_diag_publication_result_t boot_diag_publication_snapshot(
    boot_diag_publication_t *, boot_diag_t *, boot_diag_context_t *);
/* Read-only extension: invoked once with owned decoded values while the same
 * foreground owner is busy and interrupts remain masked. Must be bounded,
 * nonblocking, allocation-free; no erase/program/crypto/format or mutation of
 * owner, port, associated context or sources. Reentry returns BUSY. Callback
 * pointers are temporary and must not escape. False reports CAPTURE. Caller
 * owns callback context exclusively; it must be disjoint from owner/port. */
typedef bool (*boot_diag_capture_fn)(void *, const boot_diag_t *,
    const boot_diag_context_t *);
boot_diag_publication_result_t boot_diag_publication_capture(
    boot_diag_publication_t *, boot_diag_capture_fn, void *);
#endif
