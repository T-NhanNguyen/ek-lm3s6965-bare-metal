#include "lm3s6965/boot_target_io.h"
/* Never executed. Volatile function-pointer table roots all actual entry points
 * without manufacturing runtime objects or alternate policy. */
void fixture_entry(void);
void fixture_entry(void) {
    boot_reset_register_adapter_t (*volatile r)(void)=boot_target_reset_adapter;
    boot_flash_io_t (*volatile f)(void)=boot_target_flash_io;
    boot_flash_result_t (*volatile i)(boot_flash_t *,uint32_t,uint32_t)=boot_target_flash_init;
    boot_flash_result_t (*volatile m)(boot_flash_t *,boot_select_images_t *)=boot_target_images_read;
    boot_verify_read_fn volatile v=boot_target_verify_read;
    int (*volatile b)(boot_sequence_parameters_t *,boot_flash_t *,const boot_select_images_t *,boot_reset_register_adapter_t *,boot_store_adapter_t *)=boot_target_sequence_bind;
    boot_sequence_result_t (*volatile s)(boot_sequence_t *,const boot_sequence_parameters_t *,boot_launch_decision_t *)=boot_sequence_run;
    (void)r;(void)f;(void)i;(void)m;(void)v;(void)b;(void)s;
}
