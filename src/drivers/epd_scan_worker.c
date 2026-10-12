#include "epd_scan_worker.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

static void scan_task(void *arg)
{
    epd_scan_worker_t *w = arg;
    bool pending = false;
    int64_t next_at = 0, previous_scan = 0;
    for (;;) {
        xSemaphoreTake(w->lock, portMAX_DELAY);
        if (w->stopping) {
            xSemaphoreGive(w->lock);
            break;
        }
        if (!w->enabled) pending = false;
        bool ready = w->enabled && (pending || w->has_target);
        const int64_t now = esp_timer_get_time();
        if (!ready || now < next_at) {
            xSemaphoreGive(w->lock);
            TickType_t wait = portMAX_DELAY;
            if (ready) {
                wait = pdMS_TO_TICKS((next_at - now + 999) / 1000);
                if (wait == 0) wait = 1;
            }
            ulTaskNotifyTake(pdTRUE, wait);
            continue;
        }
        const bool new_target = w->has_target;
        if (new_target) memcpy(w->snapshot, w->latest, w->frame_size);
        w->has_target = false;
        const unsigned period = w->period_ms;
        const uint32_t generation = w->generation;
        xSemaphoreGive(w->lock);
        bool scanned = false;
        const int64_t started = esp_timer_get_time();
        const esp_err_t err = w->process(w->context, w->snapshot, new_target, &pending, &scanned);
        const int64_t ended = esp_timer_get_time();
        next_at = started + (int64_t)period * 1000;
        xSemaphoreTake(w->lock, portMAX_DELAY);
        if (generation != w->generation) pending = false;
        if (scanned) {
            ++w->stats.scans;
            w->stats.last_scan_us = ended - started;
            w->stats.last_interval_us = previous_scan == 0 ? 0 : started - previous_scan;
            previous_scan = started;
        }
        if (err != ESP_OK && generation == w->generation) {
            w->enabled = false;
            ++w->generation;
            w->has_target = false;
            pending = false;
        }
        xSemaphoreGive(w->lock);
    }
    xSemaphoreGive(w->done);
    vTaskDelete(NULL);
}

esp_err_t epd_scan_worker_init(epd_scan_worker_t *w, size_t size,
                               epd_scan_process_fn process, void *context)
{
    if (w == NULL || size == 0 || process == NULL) return ESP_ERR_INVALID_ARG;
    if (w->task != NULL) return ESP_ERR_INVALID_STATE;
    memset(w, 0, sizeof(*w));
    w->lock = xSemaphoreCreateMutexStatic(&w->lock_storage);
    w->done = xSemaphoreCreateBinaryStatic(&w->done_storage);
    w->latest = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    w->snapshot = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (w->latest == NULL || w->snapshot == NULL) {
        epd_scan_worker_deinit(w);
        return ESP_ERR_NO_MEM;
    }
    w->frame_size = size;
    w->process = process;
    w->context = context;
    w->period_ms = 100;
    if (xTaskCreate(scan_task, "epd-pulse", 4096, w, 4, &w->task) != pdPASS) {
        epd_scan_worker_deinit(w);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void epd_scan_worker_enable(epd_scan_worker_t *w, bool enabled)
{
    if (w == NULL || w->lock == NULL) return;
    xSemaphoreTake(w->lock, portMAX_DELAY);
    if (w->task == NULL || w->stopping) {
        xSemaphoreGive(w->lock);
        return;
    }
    if (w->enabled != enabled) ++w->generation;
    w->enabled = enabled;
    if (!w->enabled) w->has_target = false;
    xTaskNotifyGive(w->task);
    xSemaphoreGive(w->lock);
}

esp_err_t epd_scan_worker_submit(epd_scan_worker_t *w, const uint8_t *frame)
{
    if (w == NULL || frame == NULL) return ESP_ERR_INVALID_ARG;
    if (w->lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(w->lock, portMAX_DELAY);
    if (w->task == NULL || !w->enabled || w->stopping) {
        xSemaphoreGive(w->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (w->has_target) ++w->stats.coalesced;
    memcpy(w->latest, frame, w->frame_size);
    ++w->stats.submitted;
    w->has_target = true;
    xTaskNotifyGive(w->task);
    xSemaphoreGive(w->lock);
    return ESP_OK;
}

esp_err_t epd_scan_worker_set_period(epd_scan_worker_t *w, unsigned ms)
{
    if (w == NULL || w->lock == NULL) return ESP_ERR_INVALID_STATE;
    if (ms < 10 || ms > 1000) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(w->lock, portMAX_DELAY);
    if (w->task == NULL || w->stopping) {
        xSemaphoreGive(w->lock);
        return ESP_ERR_INVALID_STATE;
    }
    w->period_ms = ms;
    xTaskNotifyGive(w->task);
    xSemaphoreGive(w->lock);
    return ESP_OK;
}

epd_scan_stats_t epd_scan_worker_stats(epd_scan_worker_t *w)
{
    epd_scan_stats_t stats = {0};
    if (w == NULL || w->lock == NULL) return stats;
    xSemaphoreTake(w->lock, portMAX_DELAY);
    stats = w->stats;
    xSemaphoreGive(w->lock);
    return stats;
}

void epd_scan_worker_deinit(epd_scan_worker_t *w)
{
    if (w == NULL) return;
    if (w->task != NULL) {
        xSemaphoreTake(w->lock, portMAX_DELAY);
        w->enabled = false;
        w->stopping = true;
        w->has_target = false;
        xTaskNotifyGive(w->task);
        xSemaphoreGive(w->lock);
        xSemaphoreTake(w->done, portMAX_DELAY);
        xSemaphoreTake(w->lock, portMAX_DELAY);
        w->task = NULL;
        xSemaphoreGive(w->lock);
    }
    heap_caps_free(w->latest); w->latest = NULL;
    heap_caps_free(w->snapshot); w->snapshot = NULL;
}
