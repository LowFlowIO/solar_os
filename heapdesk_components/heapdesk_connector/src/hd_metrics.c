/*
 * Telemetry: heap (internal + PSRAM), CPU load and per-task stack headroom.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "sdkconfig.h"
#if CONFIG_ESP_WIFI_ENABLED
#include "esp_wifi.h"
#endif
#include "hd_internal.h"

#define HD_STACK_HINTS 24

typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    uint32_t bytes;
} stack_hint_t;

static stack_hint_t s_hints[HD_STACK_HINTS];
static portMUX_TYPE s_hint_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t heapdesk_set_task_stack_size(const char *task_name, uint32_t stack_bytes)
{
    if (!task_name || !task_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_hint_lock);
    for (int i = 0; i < HD_STACK_HINTS; i++) {
        if (s_hints[i].name[0] == '\0' || strncmp(s_hints[i].name, task_name, sizeof(s_hints[i].name)) == 0) {
            strlcpy(s_hints[i].name, task_name, sizeof(s_hints[i].name));
            s_hints[i].bytes = stack_bytes;
            ret = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_hint_lock);
    return ret;
}

uint32_t hd_task_stack_size(const char *name)
{
    uint32_t v = 0;
    portENTER_CRITICAL(&s_hint_lock);
    for (int i = 0; i < HD_STACK_HINTS && s_hints[i].name[0]; i++) {
        if (strncmp(s_hints[i].name, name, sizeof(s_hints[i].name)) == 0) {
            v = s_hints[i].bytes;
            break;
        }
    }
    portEXIT_CRITICAL(&s_hint_lock);
    if (v == 0) {
        /* Well-known ESP-IDF system tasks. */
        if (strcmp(name, "main") == 0) {
            v = CONFIG_ESP_MAIN_TASK_STACK_SIZE;
        } else if (strncmp(name, "IDLE", 4) == 0) {
            v = CONFIG_FREERTOS_IDLE_TASK_STACKSIZE;
        } else if (strcmp(name, "esp_timer") == 0) {
            v = CONFIG_ESP_TIMER_TASK_STACK_SIZE;
        } else if (strcmp(name, "sys_evt") == 0) {
            v = CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE;
        }
    }
    return v;
}

const char *hd_reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
    }
}

#if configUSE_TRACE_FACILITY
typedef struct {
    TaskHandle_t h;
    uint32_t runtime;
} prev_rt_t;

static prev_rt_t *s_prev;
static int s_prev_n;
static uint32_t s_prev_total;

static char state_char(eTaskState s)
{
    switch (s) {
    case eRunning: return 'X';
    case eReady: return 'R';
    case eBlocked: return 'B';
    case eSuspended: return 'S';
    case eDeleted: return 'D';
    default: return '?';
    }
}
#endif

char *hd_metrics_json(void)
{
    cJSON *m = cJSON_CreateObject();
    if (!m) {
        return NULL;
    }
    cJSON_AddNumberToObject(m, "up", (double)(esp_timer_get_time() / 1000000));

    cJSON *heap = cJSON_AddObjectToObject(m, "heap");
    cJSON_AddNumberToObject(heap, "free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));

    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (ps_total > 0) {
        cJSON *ps = cJSON_AddObjectToObject(m, "psram");
        cJSON_AddNumberToObject(ps, "free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(ps, "largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(ps, "total", ps_total);
    }

#if CONFIG_ESP_WIFI_ENABLED
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(m, "rssi", ap.rssi);
    }
#endif

#if configUSE_TRACE_FACILITY
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    if (n > CONFIG_HEAPDESK_MAX_TASKS + 8) {
        n = CONFIG_HEAPDESK_MAX_TASKS + 8;
    }
    TaskStatus_t *st = malloc(sizeof(TaskStatus_t) * n);
    prev_rt_t *cur = st ? malloc(sizeof(prev_rt_t) * n) : NULL;
    if (st && cur) {
        configRUN_TIME_COUNTER_TYPE total = 0;
        UBaseType_t got = uxTaskGetSystemState(st, n, &total);
        uint32_t dt = (uint32_t)total - s_prev_total;
        double busy = 0;
        cJSON *tasks = cJSON_AddArrayToObject(m, "tasks");
        for (UBaseType_t i = 0; i < got; i++) {
            uint32_t rt = (uint32_t)st[i].ulRunTimeCounter;
            cur[i].h = st[i].xHandle;
            cur[i].runtime = rt;
            double pct = 0;
#if configGENERATE_RUN_TIME_STATS
            for (int j = 0; j < s_prev_n; j++) {
                if (s_prev[j].h == st[i].xHandle) {
                    if (dt > 0) {
                        pct = 100.0 * (double)(rt - s_prev[j].runtime) / (double)dt;
                    }
                    break;
                }
            }
            if (pct > 100) {
                pct = 100;
            }
            if (strncmp(st[i].pcTaskName, "IDLE", 4) != 0) {
                busy += pct;
            }
#endif
            if (i >= CONFIG_HEAPDESK_MAX_TASKS) {
                continue;
            }
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "n", st[i].pcTaskName);
            /* ESP-IDF FreeRTOS measures stack in bytes. */
            cJSON_AddNumberToObject(t, "hwm", st[i].usStackHighWaterMark);
            uint32_t sz = hd_task_stack_size(st[i].pcTaskName);
            if (sz) {
                cJSON_AddNumberToObject(t, "sz", sz);
            }
            cJSON_AddNumberToObject(t, "cpu", (double)((int)(pct * 10 + 0.5)) / 10.0);
            cJSON_AddNumberToObject(t, "p", st[i].uxCurrentPriority);
            char s[2] = {state_char(st[i].eCurrentState), 0};
            cJSON_AddStringToObject(t, "s", s);
#if configTASKLIST_INCLUDE_COREID
            cJSON_AddNumberToObject(t, "c", st[i].xCoreID == tskNO_AFFINITY ? -1 : st[i].xCoreID);
#else
            cJSON_AddNumberToObject(t, "c", 0);
#endif
            cJSON_AddItemToArray(tasks, t);
        }
#if configGENERATE_RUN_TIME_STATS
        if (s_prev_n > 0 && dt > 0) {
            double cpu = busy / (double)portNUM_PROCESSORS;
            cJSON_AddNumberToObject(m, "cpu", cpu > 100 ? 100 : (double)((int)(cpu * 10 + 0.5)) / 10.0);
        }
#endif
        free(s_prev);
        s_prev = cur;
        s_prev_n = got;
        s_prev_total = (uint32_t)total;
        cur = NULL;
    }
    free(cur);
    free(st);
#endif

    char *out = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!out) {
        return NULL;
    }
    /* cJSON_free may differ from free with custom hooks; copy to malloc'd. */
    char *copy = strdup(out);
    cJSON_free(out);
    return copy;
}
