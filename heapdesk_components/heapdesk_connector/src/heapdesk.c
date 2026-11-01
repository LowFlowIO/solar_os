/*
 * Heapdesk connector core: MQTT session, command queue and telemetry loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "hd_internal.h"

static const char *TAG = "heapdesk";

#define HD_CMD_MAX_BYTES 8192
#define HD_QUEUE_LEN 4

hd_settings_t hd_cfg;

static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static volatile bool s_connected;
static volatile bool s_running;
static bool s_first_connect_done;

/* Reassembly buffer for MQTT messages split across several events. */
static char *s_rx;
static int s_rx_len, s_rx_total;

static char *dupstr(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

void hd_topic(char *dst, size_t n, const char *kind)
{
    snprintf(dst, n, "hd/v1/%s/%s", hd_cfg.device_id, kind);
}

esp_err_t hd_publish(const char *kind, const char *payload, int len, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    char topic[HD_TOPIC_MAX];
    hd_topic(topic, sizeof(topic), kind);
    int id = esp_mqtt_client_publish(s_client, topic, payload, len, qos, retain);
    return id < 0 ? ESP_FAIL : ESP_OK;
}

static void publish_response(const char *id, bool ok, cJSON *result, const char *err)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "id", id);
    cJSON_AddBoolToObject(r, "ok", ok);
    if (result) {
        cJSON_AddItemToObject(r, "result", result);
    }
    if (!ok && err && err[0]) {
        cJSON_AddStringToObject(r, "error", err);
    }
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) {
        if (hd_publish("resp", s, 0, 1, false) != ESP_OK) {
            ESP_LOGW(TAG, "could not publish response for %s", id);
        }
        cJSON_free(s);
    }
}

void hd_progress(const char *id, int pct)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"ok\":true,\"done\":false,\"progress\":%d}", id, pct);
    if (n > 0 && n < (int)sizeof(buf)) {
        hd_publish("resp", buf, n, 0, false);
    }
}

static void publish_hello(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char elf[65] = {0};
    esp_app_get_elf_sha256(elf, sizeof(elf));
    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "fw", app->version);
    cJSON_AddStringToObject(h, "chip", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(h, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(h, "reset_reason", hd_reset_reason_str());
    cJSON_AddStringToObject(h, "elf_sha256", elf);
    char *s = cJSON_PrintUnformatted(h);
    cJSON_Delete(h);
    if (s) {
        hd_publish("hello", s, 0, 1, false);
        cJSON_free(s);
    }
}

static void handle_command(char *payload, int len)
{
    /* Ownership of payload passes to the worker task through the queue. */
    if (xQueueSend(s_queue, &payload, 0) != pdTRUE) {
        cJSON *c = cJSON_ParseWithLength(payload, len);
        const cJSON *id = c ? cJSON_GetObjectItemCaseSensitive(c, "id") : NULL;
        if (cJSON_IsString(id)) {
            publish_response(id->valuestring, false, NULL, "device busy: command queue full");
        }
        cJSON_Delete(c);
        free(payload);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        char topic[HD_TOPIC_MAX];
        hd_topic(topic, sizeof(topic), "cmd");
        esp_mqtt_client_subscribe(s_client, topic, 1);
        hd_publish("status", "online", 6, 1, true);
        publish_hello();
        ESP_LOGI(TAG, "connected to %s as %s", hd_cfg.mqtt_uri, hd_cfg.device_id);
        if (!s_first_connect_done) {
            s_first_connect_done = true;
            /* A successful connection proves the new image works. */
            hd_ota_mark_valid();
            xTaskNotify(s_task, 1, eSetBits);
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (ev->current_data_offset == 0) {
            free(s_rx);
            s_rx = NULL;
            s_rx_len = 0;
            s_rx_total = ev->total_data_len;
            if (s_rx_total <= 0 || s_rx_total > HD_CMD_MAX_BYTES) {
                ESP_LOGW(TAG, "ignoring command of %d bytes", s_rx_total);
                s_rx_total = 0;
                break;
            }
            s_rx = malloc(s_rx_total + 1);
            if (!s_rx) {
                s_rx_total = 0;
                break;
            }
        }
        if (!s_rx || ev->current_data_offset != s_rx_len || s_rx_len + ev->data_len > s_rx_total) {
            break;
        }
        memcpy(s_rx + s_rx_len, ev->data, ev->data_len);
        s_rx_len += ev->data_len;
        if (s_rx_len == s_rx_total) {
            s_rx[s_rx_len] = '\0';
            char *p = s_rx;
            int n = s_rx_len;
            s_rx = NULL;
            s_rx_len = s_rx_total = 0;
            handle_command(p, n);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "broker refused connection (code %d): check device id/secret",
                     ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static void run_command(char *payload)
{
    char err[160] = {0};
    cJSON *c = cJSON_Parse(payload);
    free(payload);
    if (!c) {
        return;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(c, "id");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(c, "kind");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(c, "args");
    if (!cJSON_IsString(id) || !cJSON_IsString(kind) || strlen(id->valuestring) > 64) {
        cJSON_Delete(c);
        return;
    }
    ESP_LOGI(TAG, "command %s (%s)", kind->valuestring, id->valuestring);
    cJSON *res = hd_exec(id->valuestring, kind->valuestring, args, err, sizeof(err));
    bool reboot = res && (strcmp(kind->valuestring, "reboot") == 0 || strcmp(kind->valuestring, "ota_start") == 0);
    publish_response(id->valuestring, res != NULL, res, err);
    cJSON_Delete(c);
    if (reboot) {
        ESP_LOGW(TAG, "rebooting on remote request");
        vTaskDelay(pdMS_TO_TICKS(750)); /* let the response leave */
        esp_restart();
    }
}

static void worker(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONFIG_HEAPDESK_METRICS_INTERVAL_MS);
    bool boot_tasks_done = false;
    while (s_running) {
        uint32_t bits = 0;
        if (!boot_tasks_done && xTaskNotifyWait(0, UINT32_MAX, &bits, 0) == pdTRUE && bits) {
            boot_tasks_done = true;
#ifdef CONFIG_HEAPDESK_UPLOAD_COREDUMP
            hd_coredump_upload_pending();
#endif
        }
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (int32_t)(next - now) > 0 ? next - now : 0;
        char *payload = NULL;
        if (xQueueReceive(s_queue, &payload, wait) == pdTRUE) {
            run_command(payload);
            continue;
        }
        next += period;
        if ((int32_t)(xTaskGetTickCount() - next) > (int32_t)period) {
            next = xTaskGetTickCount() + period; /* we fell behind (e.g. long OTA) */
        }
        if (s_connected) {
            char *m = hd_metrics_json();
            if (m) {
                hd_publish("metrics", m, 0, 0, false);
                free(m);
            }
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t heapdesk_start(const heapdesk_config_t *cfg)
{
#ifndef CONFIG_HEAPDESK_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    heapdesk_config_t d = {0};
    if (cfg) {
        d = *cfg;
    }
    hd_cfg.mqtt_uri = dupstr(d.mqtt_uri ? d.mqtt_uri : CONFIG_HEAPDESK_MQTT_URI);
    hd_cfg.api_url = dupstr(d.api_url ? d.api_url : CONFIG_HEAPDESK_API_URL);
    hd_cfg.device_id = dupstr(d.device_id ? d.device_id : CONFIG_HEAPDESK_DEVICE_ID);
    hd_cfg.device_secret = dupstr(d.device_secret ? d.device_secret : CONFIG_HEAPDESK_DEVICE_SECRET);
    hd_cfg.ca_cert_pem = dupstr(d.ca_cert_pem);
    size_t idl = hd_cfg.device_id ? strlen(hd_cfg.device_id) : 0;
    if (idl < 4 || idl > 40 || !hd_cfg.device_secret || strlen(hd_cfg.device_secret) < 20) {
        ESP_LOGE(TAG, "device id/secret not configured (menuconfig > Heapdesk connector)");
        heapdesk_stop();
        return ESP_ERR_INVALID_ARG;
    }
    if (!hd_url_allowed(hd_cfg.mqtt_uri) || !hd_url_allowed(hd_cfg.api_url)) {
        ESP_LOGE(TAG, "plaintext endpoints are disabled (CONFIG_HEAPDESK_INSECURE_TLS_ALLOWED)");
        heapdesk_stop();
        return ESP_ERR_INVALID_ARG;
    }

    static char lwt_topic[HD_TOPIC_MAX];
    hd_topic(lwt_topic, sizeof(lwt_topic), "status");
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = hd_cfg.mqtt_uri,
        .credentials.client_id = hd_cfg.device_id,
        .credentials.username = hd_cfg.device_id,
        .credentials.authentication.password = hd_cfg.device_secret,
        .session.keepalive = 30,
        .session.last_will = {.topic = lwt_topic, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1},
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
    };
    if (hd_cfg.ca_cert_pem) {
        mc.broker.verification.certificate = hd_cfg.ca_cert_pem;
    } else {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }
    s_queue = xQueueCreate(HD_QUEUE_LEN, sizeof(char *));
    s_client = esp_mqtt_client_init(&mc);
    if (!s_queue || !s_client) {
        heapdesk_stop();
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    s_running = true;
    if (xTaskCreate(worker, "heapdesk", CONFIG_HEAPDESK_TASK_STACK_SIZE, NULL, CONFIG_HEAPDESK_TASK_PRIORITY, &s_task) != pdPASS) {
        heapdesk_stop();
        return ESP_ERR_NO_MEM;
    }
    heapdesk_set_task_stack_size("heapdesk", CONFIG_HEAPDESK_TASK_STACK_SIZE);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        heapdesk_stop();
        return err;
    }
    ESP_LOGI(TAG, "started (device %s)", hd_cfg.device_id);
    return ESP_OK;
#endif
}

esp_err_t heapdesk_stop(void)
{
    s_running = false;
    if (s_client) {
        if (s_connected) {
            hd_publish("status", "offline", 7, 1, true);
        }
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    for (int i = 0; i < 50 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_HEAPDESK_METRICS_INTERVAL_MS / 10 + 1));
    }
    if (s_queue) {
        char *p;
        while (xQueueReceive(s_queue, &p, 0) == pdTRUE) {
            free(p);
        }
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    free(s_rx);
    s_rx = NULL;
    free(hd_cfg.mqtt_uri);
    free(hd_cfg.api_url);
    free(hd_cfg.device_id);
    free(hd_cfg.device_secret);
    free(hd_cfg.ca_cert_pem);
    memset(&hd_cfg, 0, sizeof(hd_cfg));
    return ESP_OK;
}

bool heapdesk_connected(void)
{
    return s_connected;
}
