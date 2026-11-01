#pragma once
/* Heapdesk side-project glue (opt-in via SOLAR_OS_ENABLE_HEAPDESK=1). */
#include <stddef.h>
#include "esp_err.h"

#define SOLAR_OS_HEAPDESK_NVS_NS "heapdesk"

/* Registers an IP_EVENT handler; starts the connector on first IP. */
void solar_os_heapdesk_init(void);
/* Starts now if an IP is already up (used by `heapdesk start`). */
esp_err_t solar_os_heapdesk_try_start(void);
/* Reads key from NVS namespace "heapdesk"; empty string when unset. */
esp_err_t solar_os_heapdesk_get(const char *key, char *out, size_t out_len);
esp_err_t solar_os_heapdesk_set(const char *key, const char *value);
esp_err_t solar_os_heapdesk_erase(const char *key);
