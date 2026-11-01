/*
 * Core dump capture: on the first connection after boot, upload any core
 * dump stored in flash to POST {api}/device/v1/crashes (multipart), then
 * erase it so it is reported exactly once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "sdkconfig.h"
#include "hd_internal.h"

static const char *TAG = "heapdesk";

#ifdef CONFIG_HEAPDESK_UPLOAD_COREDUMP

#define BOUNDARY "----heapdesk7c1f0e2a"

static int append_field(char *dst, size_t cap, const char *name, const char *value)
{
    return snprintf(dst, cap, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", name, value);
}

static esp_err_t upload(size_t addr, size_t size)
{
    char *head = calloc(1, 2048);
    if (!head) {
        return ESP_ERR_NO_MEM;
    }
    char tmp[32], bt[640] = {0};
    const char *reason = "panic";
    const char *task = "";
    char pc[16] = "";
    char elf[APP_ELF_SHA256_SZ + 1] = "";
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
        task = sum->exc_task;
        snprintf(pc, sizeof(pc), "0x%08lx", (unsigned long)sum->exc_pc);
        strlcpy(elf, (const char *)sum->app_elf_sha256, sizeof(elf));
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        size_t o = 0;
        for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16 && o < sizeof(bt) - 12; i++) {
            o += snprintf(bt + o, sizeof(bt) - o, "%s0x%08lx", i ? " " : "", (unsigned long)sum->exc_bt_info.bt[i]);
        }
        if (sum->exc_bt_info.corrupted) {
            strlcat(bt, " |<-CORRUPTED", sizeof(bt));
        }
#else
        snprintf(bt, sizeof(bt), "0x%08lx 0x%08lx", (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.ra);
        snprintf(tmp, sizeof(tmp), "mcause 0x%lx", (unsigned long)sum->ex_info.mcause);
        reason = tmp;
#endif
    }
#endif
    int n = 0;
    n += append_field(head + n, 2048 - n, "fw", esp_app_get_description()->version);
    n += append_field(head + n, 2048 - n, "elf_sha256", elf);
    n += append_field(head + n, 2048 - n, "reason", reason);
    n += append_field(head + n, 2048 - n, "exc_task", task);
    n += append_field(head + n, 2048 - n, "exc_pc", pc);
    n += append_field(head + n, 2048 - n, "backtrace", bt);
    n += snprintf(head + n, 2048 - n, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"coredump\"; filename=\"coredump.elf\"\r\n"
                                      "Content-Type: application/octet-stream\r\n\r\n");
    static const char tail[] = "\r\n--" BOUNDARY "--\r\n";
    if (n >= 2048) {
        free(head);
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
        free(sum);
#endif
        return ESP_ERR_INVALID_SIZE;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/device/v1/crashes", hd_cfg.api_url);
    esp_http_client_config_t c;
    hd_http_config(&c, url);
    c.method = HTTP_METHOD_POST;
    c.timeout_ms = 30000;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t r = h ? ESP_OK : ESP_ERR_NO_MEM;
    if (r == ESP_OK) {
        esp_http_client_set_header(h, "Content-Type", "multipart/form-data; boundary=" BOUNDARY);
        r = hd_basic_auth(h);
    }
    if (r == ESP_OK) {
        r = esp_http_client_open(h, n + size + sizeof(tail) - 1);
    }
    if (r == ESP_OK && esp_http_client_write(h, head, n) != n) {
        r = ESP_FAIL;
    }
    uint8_t *buf = malloc(1024);
    for (size_t off = 0; r == ESP_OK && off < size; off += 1024) {
        size_t chunk = size - off < 1024 ? size - off : 1024;
        if (!buf || esp_flash_read(NULL, buf, addr + off, chunk) != ESP_OK ||
            esp_http_client_write(h, (char *)buf, chunk) != (int)chunk) {
            r = ESP_FAIL;
        }
    }
    free(buf);
    if (r == ESP_OK && esp_http_client_write(h, tail, sizeof(tail) - 1) != (int)sizeof(tail) - 1) {
        r = ESP_FAIL;
    }
    int status = 0;
    if (r == ESP_OK && esp_http_client_fetch_headers(h) >= 0) {
        status = esp_http_client_get_status_code(h);
    }
    if (h) {
        esp_http_client_cleanup(h);
    }
    free(head);
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    free(sum);
#endif
    if (r != ESP_OK) {
        return r;
    }
    if (status == 401 || status == 402) {
        ESP_LOGW(TAG, "core dump rejected (HTTP %d); keeping it in flash", status);
        return ESP_ERR_INVALID_STATE;
    }
    return status / 100 == 2 ? ESP_OK : ESP_FAIL;
}

void hd_coredump_upload_pending(void)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        return;
    }
    if (size > 8 * 1024 * 1024) {
        esp_core_dump_image_erase();
        return;
    }
    ESP_LOGI(TAG, "uploading core dump (%u bytes)", (unsigned)size);
    for (int attempt = 0; attempt < 4; attempt++) {
        esp_err_t r = upload(addr, size);
        if (r == ESP_OK) {
            esp_core_dump_image_erase();
            ESP_LOGI(TAG, "core dump uploaded and erased");
            return;
        }
        if (r == ESP_ERR_INVALID_STATE) {
            return;
        }
        ESP_LOGW(TAG, "core dump upload failed (%s), retrying", esp_err_to_name(r));
        vTaskDelay(pdMS_TO_TICKS(3000 << attempt));
    }
}
#else
void hd_coredump_upload_pending(void) {}
#endif
