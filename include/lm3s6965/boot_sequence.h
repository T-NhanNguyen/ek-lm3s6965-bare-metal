/* Shared boot-only orchestration; terminal selection, never execution. */
#ifndef LM3S6965_BOOT_SEQUENCE_H
#define LM3S6965_BOOT_SEQUENCE_H

#include "lm3s6965/boot_startup_producer.h"
#include "lm3s6965/boot_runtime_watchdog.h"

typedef struct
{
    atomic_flag entered;
    unsigned consumed;
    boot_runtime_watchdog_t bridge;
} boot_sequence_t;

typedef struct
{
    boot_startup_t *continuity;
    volatile uint32_t *diagnostic;
    uint32_t actual_hz;
    const boot_watchdog_io_t *producer_io;
    void (*gate_delay)(void *);
    const boot_reset_register_adapter_t *registers;
    const boot_store_adapter_t *store;
    const boot_select_images_t *images;
    boot_verify_read_fn reader;
    void *reader_context;
} boot_sequence_parameters_t;

typedef enum
{
    BOOT_SEQUENCE_OK = 0, BOOT_SEQUENCE_ARGUMENT, BOOT_SEQUENCE_BUSY,
    BOOT_SEQUENCE_CONSUMED, BOOT_SEQUENCE_FAILED
} boot_sequence_status_t;
typedef enum
{
    BOOT_SEQUENCE_NONE = 0, BOOT_SEQUENCE_EARLY_READ,
    BOOT_SEQUENCE_PRODUCER, BOOT_SEQUENCE_RUNTIME
} boot_sequence_stage_t;
typedef struct
{
    boot_sequence_status_t status;
    boot_sequence_stage_t stage;
    /* Valid ONLY at stage RUNTIME; nested valid bits still govern evidence.
     * Zero/default OK fields never prove that a nested operation executed. */
    boot_runtime_watchdog_result_t runtime;
} boot_sequence_result_t;

/* Fresh unpublished storage ONCE per genuine/model boot. Initializes only the
 * sequence/embedded bridge, NOT continuity or its watchdog. Never copy, recreate
 * or reinitialize either object to escape failure. Boot entry owns global once-
 * per-boot uniqueness; this local guard cannot establish genuine reset. */
void boot_sequence_init(boot_sequence_t *sequence);

/* Trusted boot-only caller: privileged masked Thread mode, genuine reset and
 * exclusive reset/default WDT, Timer2, gates, storage and stable images. Supply
 * truthful unchanged actual_hz and full RCC/RCC2 provenance before entry.
 * All producer/runtime contracts apply. Sequence, continuity, diagnostic64 and
 * out are mutually disjoint and disjoint from all input descriptors/parameters.
 * Inputs may alias inputs. Opaque register/store/reader context spans must be
 * disjoint from those objects, inputs and output; caller supplies their sizes.
 * producer_io->context MUST equal continuity->context, with no extra wrapper
 * scratch there (GPTM uses ARM40/HOST64). Descriptors are snapshotted before IO;
 * no callback may mutate inputs, objects, output or ownership, or bypass this
 * sequence via raw runtime/reset/select/watchdog calls.
 * Sequence/bridge and private checked-reset context live outside continuity
 * until synchronous return; boot-local stack is sufficient. Retained callbacks
 * still require immutable boot-FLASH transitive closure and retained-only state.
 * Known spans/alignment/callbacks/Hz are checked before IO. Only ARGUMENT/NONE
 * permits correction. Consume before first callback; BUSY/CONSUMED do no IO.
 * Early read is read-only; runtime capture reads afresh and compares the FULL
 * word once before clear/store. All later reads are fresh without comparison.
 * Every admitted result is terminal: stop on any failure, no cleanup/retry,
 * watchdog copy/reinit/reconfigure/feed or budget restart. Out is unchanged on
 * all errors. Runtime result is valid only at RUNTIME, with its own valid bits.
 * OK is solely a RUN decision: continuity remains EMPTY, not dispatch-ready.
 * Never chain launch, handoff, prepare, dispatch or an actuator from this API. */
boot_sequence_result_t boot_sequence_run(boot_sequence_t *sequence,
    const boot_sequence_parameters_t *parameters, boot_launch_decision_t *out);

#endif
