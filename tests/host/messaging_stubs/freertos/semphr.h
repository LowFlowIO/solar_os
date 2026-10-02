#pragma once
#include <stdlib.h>
#include "../../freertos/semphr.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return calloc(1, sizeof(StaticSemaphore_t));
}
static inline void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    free(semaphore);
}
