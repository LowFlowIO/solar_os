/* Heapdesk side-project glue. Uses the stock Heapdesk ESP-IDF connector as-is.
 * Config precedence per field: NVS namespace "heapdesk" (set via the
 * `heapdesk` shell command), then CONFIG_HEAPDESK_* from sdkconfig. */
#include "solar_os_heapdesk.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "heapdesk.h"
#include "nvs.h"

static const char *TAG = "solar_os_heapdesk";
static bool heapdesk_started;
static bool heapdesk_handler_registered;

esp_err_t solar_os_heapdesk_get(const char *key, char *out, size_t out_len)
{
    if (key == NULL || out == NULL || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(SOLAR_OS_HEAPDESK_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK) out[0] = '\0';
    return err;
}

esp_err_t solar_os_heapdesk_set(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SOLAR_OS_HEAPDESK_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t solar_os_heapdesk_erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SOLAR_OS_HEAPDESK_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = key ? nvs_erase_key(h, key) : nvs_erase_all(h);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool sta_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    return netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0;
}

esp_err_t solar_os_heapdesk_try_start(void)
{
    if (heapdesk_started) return ESP_OK;
    if (!sta_has_ip()) return ESP_ERR_INVALID_STATE;

    /* heapdesk_start copies strings, so stack buffers are fine. */
    char mqtt[160], api[160], id[64], secret[96];
    solar_os_heapdesk_get("mqtt_uri", mqtt, sizeof(mqtt));
    solar_os_heapdesk_get("api_url", api, sizeof(api));
    solar_os_heapdesk_get("device_id", id, sizeof(id));
    solar_os_heapdesk_get("secret", secret, sizeof(secret));
    if (id[0] == '\0' && CONFIG_HEAPDESK_DEVICE_ID[0] == '\0') {
        ESP_LOGW(TAG, "no device id; run `heapdesk set device_id hd_...`");
        return ESP_ERR_INVALID_STATE;
    }
    const heapdesk_config_t cfg = {
        .mqtt_uri = mqtt[0] ? mqtt : NULL,
        .api_url = api[0] ? api : NULL,
        .device_id = id[0] ? id : NULL,
        .device_secret = secret[0] ? secret : NULL,
    };
    const esp_err_t err = heapdesk_start(&cfg);
    if (err == ESP_OK) {
        heapdesk_started = true;
        ESP_LOGI(TAG, "connector started (%s)", mqtt[0] ? mqtt : CONFIG_HEAPDESK_MQTT_URI);
    } else {
        ESP_LOGW(TAG, "heapdesk_start failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    (void)solar_os_heapdesk_try_start();
}

void solar_os_heapdesk_init(void)
{
    if (heapdesk_handler_registered) return;
    /* Wi-Fi normally creates the default loop; tolerate either order. */
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IP handler: %s", esp_err_to_name(err));
        return;
    }
    heapdesk_handler_registered = true;
    (void)solar_os_heapdesk_try_start();
}
