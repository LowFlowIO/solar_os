#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_config.h"
typedef struct { int unused; } solar_os_context_t;
typedef struct { int type; } solar_os_event_t;
#define SOLAR_OS_EVENT_TICK 1
#define SOLAR_OS_JOB_KIND_BACKGROUND 1
#define SOLAR_OS_JOB_RESOURCE_NAME_MAX 64
typedef enum {
    SOLAR_OS_JOB_RESOURCE_CUSTOM,
    SOLAR_OS_JOB_RESOURCE_STREAM,
    SOLAR_OS_JOB_RESOURCE_NET,
} solar_os_job_resource_type_t;
typedef struct {
    const char *name;
    const char *summary;
    int kind;
    esp_err_t (*start)(solar_os_context_t *, int, char **);
    void (*stop)(solar_os_context_t *);
    bool (*event)(solar_os_context_t *, const solar_os_event_t *);
    uint32_t worker_stack_bytes;
    bool worker_stack_external;
    void (*detail)(solar_os_context_t *);
} solar_os_job_t;
esp_err_t solar_os_jobs_note_resource(const char *, solar_os_job_resource_type_t,
                                      const char *, const char *);
esp_err_t solar_os_jobs_get_generation(const char *, uint32_t *);
esp_err_t solar_os_jobs_mark_stopped(const char *, uint32_t, esp_err_t);
