#pragma once
#include "FreeRTOS.h"
typedef struct test_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
void vTaskDelay(TickType_t ticks);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
