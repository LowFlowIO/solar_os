#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SOLAR_OS_TASK_ROLE_BACKGROUND,
} solar_os_task_role_t;

typedef struct {
    bool admitted;
} solar_os_task_managed_admission_t;

static inline bool solar_os_task_admit_managed(
    const char *name,
    size_t stack_bytes,
    solar_os_task_role_t role,
    bool wait,
    solar_os_task_managed_admission_t *admission)
{
    (void)name;
    (void)stack_bytes;
    (void)role;
    (void)wait;
    admission->admitted = true;
    return true;
}

static inline void solar_os_task_note_managed_result(
    const char *name,
    size_t stack_bytes,
    solar_os_task_role_t role,
    const solar_os_task_managed_admission_t *admission,
    bool success)
{
    (void)name;
    (void)stack_bytes;
    (void)role;
    (void)admission;
    (void)success;
}
