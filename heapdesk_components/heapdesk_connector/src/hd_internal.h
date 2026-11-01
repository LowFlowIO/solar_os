#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "heapdesk.h"

#define HD_TOPIC_MAX 64
#define HD_OUT_MAX 2048

typedef struct {
    char *mqtt_uri;
    char *api_url;
    char *device_id;
    char *device_secret;
    char *ca_cert_pem;
} hd_settings_t;

extern hd_settings_t hd_cfg;

struct heapdesk_out {
    char buf[HD_OUT_MAX];
    size_t len;
    bool truncated;
};

/* heapdesk.c */
void hd_topic(char *dst, size_t n, const char *kind);
esp_err_t hd_publish(const char *kind, const char *payload, int len, int qos, bool retain);
/* Publish a non-final progress update for command id. */
void hd_progress(const char *id, int pct);

/* hd_metrics.c */
char *hd_metrics_json(void);
const char *hd_reset_reason_str(void);
uint32_t hd_task_stack_size(const char *name);

/* hd_commands.c: execute one command; returns result (caller owns) or NULL with err set. */
cJSON *hd_exec(const char *id, const char *kind, const cJSON *args, char *err, size_t errlen);
void hd_ota_mark_valid(void);

/* hd_coredump.c */
void hd_coredump_upload_pending(void);

/* hd_http.c */
void hd_http_config(esp_http_client_config_t *c, const char *url);
bool hd_url_allowed(const char *url);
void hd_hex(const uint8_t *in, size_t n, char *out);
esp_err_t hd_basic_auth(esp_http_client_handle_t h);
