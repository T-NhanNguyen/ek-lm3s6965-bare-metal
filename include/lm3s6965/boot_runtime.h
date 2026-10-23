/* Bounded HOST reset-to-selection decision, not physical boot/handoff. */
#ifndef LM3S6965_BOOT_RUNTIME_H
#define LM3S6965_BOOT_RUNTIME_H

#include <stdatomic.h>
#include <stdint.h>

#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_reset_handler.h"
#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"

typedef struct
{
    atomic_flag entered;
    unsigned consumed;
} boot_runtime_t;

typedef enum
{
    BOOT_RUNTIME_OK = 0,
    BOOT_RUNTIME_ARGUMENT,
    BOOT_RUNTIME_BUSY,
    BOOT_RUNTIME_CONSUMED,
    BOOT_RUNTIME_RESET_FAILED,
    BOOT_RUNTIME_SELECT_FAILED,
    BOOT_RUNTIME_RECONCILE
} boot_runtime_status_t;

#define BOOT_RUNTIME_REPORT_RESET UINT32_C(1)
#define BOOT_RUNTIME_REPORT_SELECT UINT32_C(2)
typedef struct
{
    boot_runtime_status_t status;
    uint32_t valid;
    boot_reset_handler_result_t reset_status;
    boot_reset_handler_report_t reset;
    boot_select_result_t selection;
} boot_runtime_result_t;

/* Initialize fresh unpublished storage ONCE per physical/model boot. Do not
 * reinitialize/recreate/copy this object within that boot, even after failure.
 * New boot requires a fresh object and fresh register capture, not cached flags.
 * No global latch or persistent exactly-once guarantee is provided. */
void boot_runtime_init(boot_runtime_t *runtime);

/* Caller owns adapters, contexts, object, images and authority exclusively and
 * establishes watchdog protection before calling. All boot_select stability,
 * truthful IO and provenance contracts still apply except this call establishes
 * successful reset accounting itself. No external raw reset/select bypass in
 * the same boot. No ISR/concurrent users; atomic guard rejects callback reentry.
 * Structural rejection makes no IO and permits correction on the same object.
 * Admission consumes the object BEFORE any callback; success and EVERY admitted
 * failure are terminal. Duplicate calls make no callbacks; no selection-only
 * retry. Inputs may alias other inputs but not runtime; out must be disjoint
 * from runtime, every input and opaque context storage (opaque sizes are caller
 * obligations). Callbacks must not mutate object, inputs or output. Adapter
 * tables and both manifest prefixes are copied before operations.
 *
 * Actual handler capture -> fresh RMW clear -> authoritative load/classify ->
 * at most one accounting commit. ONLY handler OK's final snapshot enters real
 * boot_select. Uncounted reset has zero ACCOUNTING writes; selector may still
 * commit READY->TRIAL or previous promotion. Never select from partial reports.
 * UNCERTAIN in either stage => RECONCILE/no decision, no automatic reload/replay.
 * Explicit read-only authority reconciliation OUTSIDE this API does not permit
 * rerunning this boot or turn a failed reset into OK. New boot must recapture.
 * All non-OK results leave out unchanged. OK publishes an owned decision only
 * after both stages succeed, not execution permission. Result valid bits mark
 * attempted stages; nested reset validity and selector evidence_mask govern
 * diagnostics. Neutral enum OK defaults are NOT proof of work or verification.
 */
boot_runtime_result_t boot_runtime_run(boot_runtime_t *runtime,
    const boot_reset_register_adapter_t *registers,
    const boot_store_adapter_t *store, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    boot_verify_progress_fn progress, void *progress_context,
    boot_launch_decision_t *out);

#endif
