#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

/* Hook callbacks and assertion handler referenced by FreeRTOSConfig.h. */

void vApplicationMallocFailedHook(void)
{
    printf("freertos: malloc failed\n");

    taskDISABLE_INTERRUPTS();

    for (;;)
    {
    }
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;

    printf("freertos: stack overflow in %s\n", task_name);

    taskDISABLE_INTERRUPTS();

    for (;;)
    {
    }
}

void freertos_assert_failed(const char *file, unsigned long line)
{
    printf("freertos: assert failed at %s:%lu\n", file, line);

    taskDISABLE_INTERRUPTS();

    for (;;)
    {
    }
}
