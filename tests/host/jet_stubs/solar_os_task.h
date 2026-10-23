#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct jet_test_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define SOLAR_OS_TASK_STOP_WAIT_MS 2000
#define SOLAR_OS_TASK_ROLE_FOREGROUND 1
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY -1
#define pdMS_TO_TICKS(ms) (ms)
int solar_os_task_create_pinned_internal(TaskFunction_t function, const char *name,
    uint32_t stack, void *argument, unsigned priority, TaskHandle_t *handle, int core, int role);
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout);
void solar_os_task_delete_internal(TaskHandle_t task);
void vTaskDelay(unsigned ticks);
