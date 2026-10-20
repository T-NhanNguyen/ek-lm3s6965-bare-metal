#include "boot_clock_setup.h"

#include <stdint.h>

#include "lm3s6965/boot_target_io.h"
#include "lm3s6965/system_control.h"

void Boot_Terminal_Handler(void) __attribute__((noreturn));

/* Ordinary boot-only RAM; each API object/input/context occupies disjoint
 * stable storage. No retained object is touched until sequence admission. */
static boot_sequence_t sequence;
static boot_sequence_parameters_t parameters;
static boot_flash_t flash;
static boot_select_images_t images;
static boot_reset_register_adapter_t registers;
static boot_store_adapter_t store;
static boot_watchdog_io_t producer;
static boot_launch_decision_t decision;

/* Debugger publication protocol: state is written LAST. Result/decision are
 * meaningful only in their corresponding terminal states, never on reset. */
enum { BOOT_ENTRY_STARTING = 1, BOOT_ENTRY_CLOCK_FAILED,
       BOOT_ENTRY_FLASH_FAILED, BOOT_ENTRY_IMAGES_FAILED,
       BOOT_ENTRY_BIND_FAILED, BOOT_ENTRY_SEQUENCE_FAILED, BOOT_ENTRY_DECISION };
volatile uint32_t boot_entry_state;
volatile boot_flash_result_t boot_entry_flash_result;
volatile boot_sequence_result_t boot_entry_sequence_result;
volatile boot_launch_decision_t boot_entry_decision;

static uint32_t clock_read(uint32_t address)
{
    return *(volatile const uint32_t *)(uintptr_t)address;
}
static void clock_write(uint32_t address, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)address = value;
}
static void clock_delay(uint32_t iterations)
{
    for (volatile uint32_t remaining = iterations; remaining != 0u; --remaining) {
        __asm__ volatile("nop");
    }
}
static void stop(uint32_t state) __attribute__((noreturn));
static void stop(uint32_t state)
{
    __asm__ volatile("dmb" ::: "memory");
    boot_entry_state = state;
    Boot_Terminal_Handler();
}

void boot_decision_entry(void) __attribute__((noreturn));
void boot_decision_entry(void)
{
    const boot_clock_io_t clock = { clock_read, clock_write, clock_delay };
    boot_entry_state = BOOT_ENTRY_STARTING;
    if (!boot_clock_setup(&clock)) stop(BOOT_ENTRY_CLOCK_FAILED);
    boot_sequence_init(&sequence);
    boot_entry_flash_result = boot_target_flash_init(&flash,
        SYSTEM_CLOCK_FREQUENCY_HZ, 32768u);
    if (boot_entry_flash_result != BOOT_FLASH_OK) stop(BOOT_ENTRY_FLASH_FAILED);
    boot_entry_flash_result = boot_target_images_read(&flash, &images);
    if (boot_entry_flash_result != BOOT_FLASH_OK) stop(BOOT_ENTRY_IMAGES_FAILED);
    parameters.continuity = &boot_startup_continuity;
    parameters.diagnostic = (volatile uint32_t *)(uintptr_t)0x2000dfc0u;
    parameters.actual_hz = SYSTEM_CLOCK_FREQUENCY_HZ;
    producer.context = boot_startup_continuity.context;
    producer.read_register = boot_startup_gptm_direct_read;
    producer.write_register = boot_startup_gptm_direct_write;
    parameters.producer_io = &producer;
    parameters.gate_delay = boot_startup_gptm_direct_delay;
    if (boot_target_sequence_bind(&parameters, &flash, &images,
                                  &registers, &store) != 0)
        stop(BOOT_ENTRY_BIND_FAILED);
    /* The sole run owns producer/WDT and accounting. No retry or dispatch. */
    boot_sequence_result_t result = boot_sequence_run(&sequence, &parameters,
                                                     &decision);
    boot_entry_sequence_result = result;
    if (result.status != BOOT_SEQUENCE_OK) stop(BOOT_ENTRY_SEQUENCE_FAILED);
    boot_entry_decision = decision;
    stop(BOOT_ENTRY_DECISION);
}
