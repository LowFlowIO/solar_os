#pragma once
#include "freertos/FreeRTOS.h"
#include <stdlib.h>
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffU
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return calloc(1, sizeof(int));
}
static inline SemaphoreHandle_t
xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {
    return s;
}
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) {
    (void)s;
    (void)t;
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    (void)s;
    return 1;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
