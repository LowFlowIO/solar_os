#pragma once
#include "FreeRTOS.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
typedef struct { pthread_mutex_t lock; pthread_cond_t wake; unsigned count; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t test_semaphore_init(StaticSemaphore_t *s, unsigned count) {
    assert(pthread_mutex_init(&s->lock, NULL) == 0);
    assert(pthread_cond_init(&s->wake, NULL) == 0);
    s->count = count;
    return s;
}
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {
    return test_semaphore_init(s, 1);
}
static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) {
    return test_semaphore_init(s, 0);
}
static inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t timeout) {
    assert(s); pthread_mutex_lock(&s->lock);
    if (timeout == portMAX_DELAY) {
        while (!s->count) pthread_cond_wait(&s->wake, &s->lock);
    } else {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += timeout / 1000;
        until.tv_nsec += (timeout % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { ++until.tv_sec; until.tv_nsec -= 1000000000L; }
        while (!s->count)
            if (pthread_cond_timedwait(&s->wake, &s->lock, &until) == ETIMEDOUT) {
                pthread_mutex_unlock(&s->lock); return 0;
            }
    }
    --s->count; pthread_mutex_unlock(&s->lock); return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    assert(s); pthread_mutex_lock(&s->lock);
    assert(s->count == 0); s->count = 1;
    pthread_cond_broadcast(&s->wake); pthread_mutex_unlock(&s->lock); return 1;
}
