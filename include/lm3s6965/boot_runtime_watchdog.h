/* Bounded HOST genuine-progress bridge; not startup, timing or MMIO. */
#ifndef LM3S6965_BOOT_RUNTIME_WATCHDOG_H
#define LM3S6965_BOOT_RUNTIME_WATCHDOG_H

#include <stdatomic.h>
#include <stdint.h>

#include "lm3s6965/boot_launch.h"
#include "lm3s6965/boot_reset_handler.h"
#include "lm3s6965/boot_runtime.h"
#include "lm3s6965/boot_select.h"
#include "lm3s6965/boot_store.h"
#include "lm3s6965/boot_verify.h"
#include "lm3s6965/boot_watchdog.h"

typedef struct
{
    atomic_flag entered;
    unsigned consumed;
    boot_runtime_t runtime;
} boot_runtime_watchdog_t;

typedef enum
{
    BOOT_RUNTIME_WATCHDOG_OK = 0,
    BOOT_RUNTIME_WATCHDOG_ARGUMENT,
    BOOT_RUNTIME_WATCHDOG_BUSY,
    BOOT_RUNTIME_WATCHDOG_CONSUMED,
    BOOT_RUNTIME_WATCHDOG_REJECTED,
    BOOT_RUNTIME_WATCHDOG_RUNTIME_FAILED
} boot_runtime_watchdog_status_t;

typedef enum
{
    BOOT_RUNTIME_WATCHDOG_PREFLIGHT = 0,
    BOOT_RUNTIME_WATCHDOG_REPORT,
    BOOT_RUNTIME_WATCHDOG_SERVICE
} boot_runtime_watchdog_operation_t;

#define BOOT_RUNTIME_WATCHDOG_DIAGNOSTIC UINT32_C(1)
#define BOOT_RUNTIME_WATCHDOG_RUNTIME UINT32_C(2)
#define BOOT_RUNTIME_WATCHDOG_CALLBACK UINT32_C(4)
typedef struct
{
    boot_runtime_watchdog_status_t status;
    uint32_t valid;
    boot_runtime_result_t runtime;
    /* Last attempted watchdog operation, including exact failing cause. Callback
     * fields are valid only with CALLBACK; no callback is inferred from OK. */
    boot_watchdog_result_t watchdog;
    boot_runtime_watchdog_operation_t operation;
    boot_verify_phase_t phase;
    uint32_t epoch, verification, completed, total;
} boot_runtime_watchdog_result_t;

/* Fresh unpublished storage ONCE per boot. No copy/reinit/recreation or raw
 * runtime/reset/select/report/service bypass in the same boot. The embedded
 * runtime is owned by this wrapper, not a separately callable runtime object. */
void boot_runtime_watchdog_init(boot_runtime_watchdog_t *bridge);

/* Caller MUST ALREADY have successfully configured a nonblocked BOOT-owned
 * watchdog, stable actual clock/readiness/reload, and exclusively own controller,
 * bridge, contexts, storage, images and outputs for the entire transaction.
 * Never configures, blindly feeds, retries, rolls back or transfers ownership.
 * Holds watchdog entry across ALL callbacks (external watchdog calls => BUSY).
 * Callback tables/prefixes are snapshotted. All mutable objects/output must be
 * disjoint from each other and all input tables. Opaque context spans must also
 * be disjoint from these objects/output; caller establishes their sizes. Inputs
 * may alias other inputs. No callback mutation of objects, inputs or output.
 * Structural/watchdog preflight rejection has no reset/flash/register effects
 * and permits correction. Admitted run consumed BEFORE callbacks; reentry BUSY,
 * duplicate CONSUMED/no IO. Output frozen until complete owned success.
 * Genuine BEGIN adopts watchdog verification+1, never feeds. No ID wrap. Actual
 * AUTHENTICATED and exact read+hash chunks report then service with current epoch.
 * A report/service failure returns 0 => verifier ABORTED, never invalid fallback.
 * Every admitted failure quarantines watchdog and discards evidence without IO
 * (service's own fault cleanup is unchanged). No later wrapper service/retry.
 * Runtime report meanings/evidence_mask are unchanged. Last good progress is
 * NOT image validity: later hash/vector/read/crypto failures remain possible.
 * Reset/flash before BEGIN, indivisible ECDSA, interattempt/final hash and
 * postverify commit/publication gaps are unmeasured; NO synthetic reports.
 */
boot_runtime_watchdog_result_t boot_runtime_watchdog_run(
    boot_runtime_watchdog_t *bridge, boot_watchdog_t *watchdog,
    const boot_reset_register_adapter_t *registers,
    const boot_store_adapter_t *store, const boot_select_images_t *images,
    boot_verify_read_fn reader, void *reader_context,
    boot_launch_decision_t *out);

#endif
