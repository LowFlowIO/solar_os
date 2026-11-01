/*
 * Heapdesk connector for ESP-IDF.
 *
 * Streams heap, CPU and per-task stack telemetry to a Heapdesk server over
 * MQTT, uploads core dumps after a crash, and executes remote commands
 * (NVS, console, file transfer, OTA, reboot) issued from the dashboard.
 *
 * Call heapdesk_start() once the network interface has an IP address.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *mqtt_uri;      /*!< NULL: CONFIG_HEAPDESK_MQTT_URI */
    const char *api_url;       /*!< NULL: CONFIG_HEAPDESK_API_URL */
    const char *device_id;     /*!< NULL: CONFIG_HEAPDESK_DEVICE_ID */
    const char *device_secret; /*!< NULL: CONFIG_HEAPDESK_DEVICE_SECRET */
    const char *ca_cert_pem;   /*!< NULL: use the ESP-IDF certificate bundle (if enabled) */
} heapdesk_config_t;

/** Output sink passed to console command handlers. */
typedef struct heapdesk_out heapdesk_out_t;

/**
 * Console command handler. Write output with heapdesk_printf().
 * Return 0 for success; any other value is shown as the exit code.
 */
typedef int (*heapdesk_cmd_fn)(int argc, char **argv, heapdesk_out_t *out, void *ctx);

/** Start the connector (copies all strings). Safe to call once. */
esp_err_t heapdesk_start(const heapdesk_config_t *cfg);

/** Stop the connector and free its resources. */
esp_err_t heapdesk_stop(void);

/** True while the MQTT session is up. */
bool heapdesk_connected(void);

/**
 * Register a console command callable from the dashboard (run_cmd).
 * name: 1-23 chars, no spaces. Up to 24 commands. Thread-safe.
 */
esp_err_t heapdesk_register_command(const char *name, const char *help, heapdesk_cmd_fn fn, void *ctx);

/** printf into a command's output buffer (truncated at 2 KiB). */
int heapdesk_printf(heapdesk_out_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * Tell Heapdesk the stack size (bytes) of a task so the dashboard can show
 * headroom as a percentage. FreeRTOS does not expose stack sizes at runtime.
 */
esp_err_t heapdesk_set_task_stack_size(const char *task_name, uint32_t stack_bytes);

#ifdef __cplusplus
}
#endif
