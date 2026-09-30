#pragma once
#include "solar_os_jobs.h"
typedef struct { int unused; } solar_os_shell_io_t;
solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *);
int solar_os_shell_io_printf(solar_os_shell_io_t *, const char *, ...);
