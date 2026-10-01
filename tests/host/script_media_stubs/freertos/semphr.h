#pragma once
#include "../../rtsp_stubs/freertos/semphr.h"
typedef pthread_mutex_t StaticSemaphore_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    pthread_mutex_init(storage, NULL);
    return storage;
}
