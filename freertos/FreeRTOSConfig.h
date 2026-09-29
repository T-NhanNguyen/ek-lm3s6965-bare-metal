#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Configuration for the LM3S6965 FreeRTOS target.
 * The values follow the official FreeRTOS demo CORTEX_LM3S6965_GCC_QEMU.
 * FreeRTOS itself is MIT licensed and lives in third_party/FreeRTOS-Kernel. */

#include "system_control.h"

/* Hardware description. */
#define configCPU_CLOCK_HZ                   ((unsigned long)SYSTEM_CLOCK_FREQUENCY_HZ)
#define configTICK_RATE_HZ                   ((TickType_t)1000)

/* Scheduling behaviour. */
#define configUSE_PREEMPTION                 1
#define configUSE_TIME_SLICING              1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE             0
#define configMAX_PRIORITIES                (5)
#define configMINIMAL_STACK_SIZE            ((unsigned short)128)
#define configMAX_TASK_NAME_LEN             (16)
#define configTICK_TYPE_WIDTH_IN_BITS       TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD             1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1

/* Synchronisation primitives. */
#define configUSE_MUTEXES                   1
#define configUSE_RECURSIVE_MUTEXES         1
#define configUSE_COUNTING_SEMAPHORES       1
#define configUSE_QUEUE_SETS                0
#define configUSE_TASK_NOTIFICATIONS        1
#define configQUEUE_REGISTRY_SIZE           0

/* Software timers. */
#define configUSE_TIMERS                    1
#define configTIMER_TASK_PRIORITY           (2)
#define configTIMER_QUEUE_LENGTH            (10)
#define configTIMER_TASK_STACK_DEPTH        (configMINIMAL_STACK_SIZE)

/* Memory allocation: heap_4 with a static pool in .bss. */
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configSUPPORT_STATIC_ALLOCATION     0
#define configTOTAL_HEAP_SIZE               ((size_t)(32 * 1024))

/* Optional kernel features. */
#define configUSE_EVENT_GROUPS              1
#define configUSE_STREAM_BUFFERS            1
#define configUSE_TRACE_FACILITY            0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configUSE_NEWLIB_REENTRANT          0

/* Hook callbacks implemented in freertos/hooks.c. */
#define configUSE_IDLE_HOOK                 0
#define configUSE_TICK_HOOK                 0
#define configUSE_MALLOC_FAILED_HOOK        1
#define configCHECK_FOR_STACK_OVERFLOW      2

/* Interrupt priorities. QEMU does not model the NVIC priority bits, so the
 * kernel priority uses all eight bits, and the syscall ceiling stays at 0x40.
 * See https://www.freertos.org/RTOS-Cortex-M3-M4.html. */
#define configKERNEL_INTERRUPT_PRIORITY     255
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (0x40)

void freertos_assert_failed(const char *file, unsigned long line);
#define configASSERT(x)                     \
    if ((x) == 0)                           \
    {                                       \
        freertos_assert_failed(__FILE__, __LINE__); \
    }

/* The vector table in src/startup.c names the supervisor call entry
 * SVCall_Handler, not SVC_Handler. Map the port handlers onto the names that
 * the vector table uses, so the direct-routing check passes. */
#define vPortSVCHandler                     SVCall_Handler
#define xPortPendSVHandler                  PendSV_Handler
#define xPortSysTickHandler                 SysTick_Handler
#define configCHECK_HANDLER_INSTALLATION    1

/* API functions kept in the build. */
#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskDelayUntil             1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_xTaskGetIdleTaskHandle      1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_eTaskGetState               1

#endif
