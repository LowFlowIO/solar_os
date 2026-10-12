#pragma once
#include "FreeRTOS.h"
typedef struct test_scan_task *TaskHandle_t;
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
void vTaskDelete(TaskHandle_t task);
