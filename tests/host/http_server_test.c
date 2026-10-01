#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/task.h"
#include "solar_os_http_server.h"
#include "solar_os_log.h"

static httpd_uri_handler_t registered_get;
static httpd_uri_handler_t registered_post;
static unsigned server_starts;
static unsigned server_stops;
static unsigned async_begins;
static unsigned async_completes;
static unsigned service_unavailable;
static unsigned sync_calls;
static httpd_req_t *accepted_async[2];
static size_t accepted_async_count;

esp_err_t solar_os_log_write(solar_os_log_level_t level,
                             const char *tag,
                             const char *format,
                             ...)
{
    (void)level;
    (void)tag;
    (void)format;
    return ESP_OK;
}

uint32_t esp_random(void)
{
    return 123456U;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    assert(handle != NULL);
    assert(config != NULL);
    *handle = (void *)(uintptr_t)(server_starts + 1U);
    server_starts++;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    assert(handle != NULL);
    server_stops++;
    registered_get = NULL;
    registered_post = NULL;
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle,
                                     const httpd_uri_t *uri)
{
    assert(handle != NULL);
    assert(uri != NULL);
    if (uri->method == HTTP_GET) {
        registered_get = uri->handler;
    } else if (uri->method == HTTP_POST) {
        registered_post = uri->handler;
    }
    return ESP_OK;
}

bool httpd_uri_match_wildcard(const char *template,
                              const char *uri,
                              size_t len)
{
    (void)template;
    (void)uri;
    (void)len;
    return true;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field)
{
    (void)req;
    (void)field;
    return 0U;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req,
                                      const char *field,
                                      char *value,
                                      size_t value_len)
{
    (void)req;
    (void)field;
    (void)value;
    (void)value_len;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_send_404(httpd_req_t *req)
{
    (void)req;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *req,
                             const char *field,
                             const char *value)
{
    (void)req;
    (void)field;
    (void)value;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req,
                              httpd_err_code_t error,
                              const char *message)
{
    (void)req;
    (void)error;
    (void)message;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    if (strcmp(status, "503 Service Unavailable") == 0) {
        service_unavailable++;
    }
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *message)
{
    (void)req;
    (void)message;
    return ESP_OK;
}

esp_err_t httpd_req_async_handler_begin(httpd_req_t *req,
                                        httpd_req_t **out)
{
    assert(req != NULL);
    assert(out != NULL);
    *out = malloc(sizeof(**out));
    assert(*out != NULL);
    **out = *req;
    async_begins++;
    return ESP_OK;
}

esp_err_t httpd_req_async_handler_complete(httpd_req_t *req)
{
    assert(req != NULL);
    async_completes++;
    free(req);
    return ESP_OK;
}

static esp_err_t sync_handler(httpd_req_t *req, void *user)
{
    assert(req != NULL);
    assert(user == (void *)0x1234);
    sync_calls++;
    return ESP_OK;
}

static esp_err_t async_handler(httpd_req_t *req, void *user)
{
    (void)user;
    assert(req != NULL);
    assert(accepted_async_count < 2U);
    accepted_async[accepted_async_count++] = req;
    return ESP_OK;
}

int main(void)
{
    const solar_os_http_route_t sync_route = {
        .owner = "sync",
        .uri = "/sync",
        .method = HTTP_GET,
        .auth = SOLAR_OS_HTTP_AUTH_PUBLIC,
        .handler = sync_handler,
        .user = (void *)0x1234,
    };
    assert(solar_os_http_server_register_route(&sync_route) == ESP_OK);
    assert(server_starts == 1U);
    assert(registered_get != NULL && registered_post != NULL);
    httpd_req_t request = {.uri = "/sync", .method = HTTP_GET};
    assert(registered_get(&request) == ESP_OK);
    assert(sync_calls == 1U);
    assert(solar_os_http_server_unregister_owner("sync") == ESP_OK);
    assert(server_stops == 1U);

    const solar_os_http_route_t async_route = {
        .owner = "async",
        .uri = "/stream",
        .method = HTTP_GET,
        .asynchronous = true,
        .auth = SOLAR_OS_HTTP_AUTH_PUBLIC,
        .handler = async_handler,
    };
    assert(solar_os_http_server_register_route(&async_route) == ESP_OK);
    assert(server_starts == 2U);

    request = (httpd_req_t) {.uri = "/stream", .method = HTTP_GET};
    assert(registered_get(&request) == ESP_OK);
    assert(registered_get(&request) == ESP_OK);
    assert(accepted_async_count == 2U);
    assert(async_begins == 2U);

    /* The fixed async capacity rejects a third request without calling the job. */
    assert(registered_get(&request) == ESP_OK);
    assert(accepted_async_count == 2U);
    assert(async_begins == 3U);
    assert(async_completes == 1U);
    assert(service_unavailable == 1U);

    /* In-flight references prevent route/server teardown. */
    assert(solar_os_http_server_unregister_owner("async") == ESP_ERR_TIMEOUT);
    assert(server_stops == 1U);
    assert(solar_os_http_server_complete_async(accepted_async[0]) == ESP_OK);
    assert(server_stops == 1U);
    assert(solar_os_http_server_complete_async(accepted_async[1]) == ESP_OK);
    assert(server_stops == 2U);
    assert(async_completes == 3U);
    assert(solar_os_http_server_complete_async(accepted_async[1]) ==
           ESP_ERR_INVALID_STATE);

    puts("http server tests: ok");
    return 0;
}
