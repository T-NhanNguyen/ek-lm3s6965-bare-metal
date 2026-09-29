#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "led.h"
#include "system_control.h"
#include "trace.h"
#include "uart.h"

#define UART0_BAUD_RATE 115200u
#define SWO_BAUD_RATE   1000000u

#define HEARTBEAT_TASK_STACK_WORDS 256u
#define CONSOLE_TASK_STACK_WORDS   256u
#define HEARTBEAT_TASK_PRIORITY    2u
#define CONSOLE_TASK_PRIORITY      1u

#define HEARTBEAT_HALF_PERIOD_MS 500u
#define CONSOLE_PERIOD_MS        1000u

static void heartbeat_task(void *parameters)
{
    (void)parameters;

    for (;;)
    {
        user_led_write(true);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_HALF_PERIOD_MS));
        user_led_write(false);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_HALF_PERIOD_MS));
    }
}

static void console_task(void *parameters)
{
    (void)parameters;

    uint32_t message_count = 0u;

    for (;;)
    {
        printf("freertos: tick %lu message %lu\n",
               (unsigned long)xTaskGetTickCount(),
               (unsigned long)message_count);
        message_count++;
        vTaskDelay(pdMS_TO_TICKS(CONSOLE_PERIOD_MS));
    }
}

static void create_demo_tasks(void)
{
    const BaseType_t heartbeat_result =
        xTaskCreate(heartbeat_task, "heartbeat", HEARTBEAT_TASK_STACK_WORDS, NULL,
                    HEARTBEAT_TASK_PRIORITY, NULL);
    const BaseType_t console_result =
        xTaskCreate(console_task, "console", CONSOLE_TASK_STACK_WORDS, NULL,
                    CONSOLE_TASK_PRIORITY, NULL);

    if ((heartbeat_result != pdPASS) || (console_result != pdPASS))
    {
        printf("freertos: task creation failed\n");

        for (;;)
        {
        }
    }
}

int main(void)
{
    const bool pll_lock_acquired = system_control_configure_pll();
    const uint32_t system_clock_hz =
        pll_lock_acquired ? SYSTEM_CLOCK_FREQUENCY_HZ : EXTERNAL_CRYSTAL_FREQUENCY_HZ;

    uart0_initialize(system_clock_hz, UART0_BAUD_RATE);
    trace_initialize(system_clock_hz, SWO_BAUD_RATE);
    user_led_initialize();
    user_led_write(false);

    printf("\n");
    printf("LM3S6965 FreeRTOS bring-up\n");
    printf("sysclk:%u Hz\n", (unsigned)system_clock_hz);
    printf("rcc:   0x%08X\n", (unsigned)system_control_read_rcc());
    printf("tick:  %u Hz\n", (unsigned)configTICK_RATE_HZ);
    printf("heap:  %u bytes\n", (unsigned)configTOTAL_HEAP_SIZE);
    printf("scheduler starting\n");

    create_demo_tasks();
    vTaskStartScheduler();

    for (;;)
    {
    }
}
