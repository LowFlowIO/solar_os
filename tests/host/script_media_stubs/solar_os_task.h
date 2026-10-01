#pragma once
#include "freertos/task.h"
#define SOLAR_OS_TASK_STOP_WAIT_MS 2000U
typedef enum { SOLAR_OS_TASK_ROLE_FOREGROUND } solar_os_task_role_t;
BaseType_t solar_os_task_create_pinned_external(TaskFunction_t, const char *, uint32_t,
    void *, UBaseType_t, TaskHandle_t *, BaseType_t, solar_os_task_role_t);
void solar_os_task_delete_external(TaskHandle_t);
bool solar_os_task_wait_done(TaskHandle_t, volatile bool *, uint32_t);
