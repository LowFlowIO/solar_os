/*
 * Remote commands: ping, reboot, NVS, console, files and OTA.
 */
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"
#include "hd_internal.h"

static const char *TAG = "heapdesk";

#define ERR(...) (snprintf(err, errlen, __VA_ARGS__), (cJSON *)NULL)

/* ---------- console registry ---------- */
#define HD_MAX_CMDS 24
typedef struct {
    char name[24];
    const char *help;
    heapdesk_cmd_fn fn;
    void *ctx;
} reg_cmd_t;
static reg_cmd_t s_cmds[HD_MAX_CMDS];
static portMUX_TYPE s_cmd_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t heapdesk_register_command(const char *name, const char *help, heapdesk_cmd_fn fn, void *ctx)
{
    if (!name || !fn || !name[0] || strlen(name) >= sizeof(s_cmds[0].name) || strchr(name, ' ')) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_cmd_lock);
    for (int i = 0; i < HD_MAX_CMDS; i++) {
        if (!s_cmds[i].fn || strcmp(s_cmds[i].name, name) == 0) {
            strlcpy(s_cmds[i].name, name, sizeof(s_cmds[i].name));
            s_cmds[i].help = help ? help : "";
            s_cmds[i].fn = fn;
            s_cmds[i].ctx = ctx;
            ret = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_cmd_lock);
    return ret;
}

int heapdesk_printf(heapdesk_out_t *out, const char *fmt, ...)
{
    if (!out || out->len >= sizeof(out->buf) - 1) {
        if (out) {
            out->truncated = true;
        }
        return 0;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out->buf + out->len, sizeof(out->buf) - out->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return n;
    }
    if ((size_t)n >= sizeof(out->buf) - out->len) {
        out->len = sizeof(out->buf) - 1;
        out->truncated = true;
    } else {
        out->len += n;
    }
    return n;
}

static int run_console(char *line, heapdesk_out_t *out)
{
    char *argv[16];
    int argc = 0;
    for (char *save = NULL, *tok = strtok_r(line, " \t", &save); tok && argc < 16; tok = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        heapdesk_printf(out, "help      this list\nheap      heap summary\nversion   firmware info\nuptime    seconds since boot\n");
        for (int i = 0; i < HD_MAX_CMDS && s_cmds[i].fn; i++) {
            heapdesk_printf(out, "%-9s %s\n", s_cmds[i].name, s_cmds[i].help);
        }
        return 0;
    }
    if (strcmp(argv[0], "heap") == 0) {
        heapdesk_printf(out, "internal free %u, largest %u, min %u\npsram free %u, largest %u\n",
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return 0;
    }
    if (strcmp(argv[0], "version") == 0) {
        const esp_app_desc_t *d = esp_app_get_description();
        heapdesk_printf(out, "%s %s (IDF %s, %s)\n", d->project_name, d->version, d->idf_ver, CONFIG_IDF_TARGET);
        return 0;
    }
    if (strcmp(argv[0], "uptime") == 0) {
        heapdesk_printf(out, "%" PRId64 "\n", esp_timer_get_time() / 1000000);
        return 0;
    }
    heapdesk_cmd_fn fn = NULL;
    void *ctx = NULL;
    portENTER_CRITICAL(&s_cmd_lock);
    for (int i = 0; i < HD_MAX_CMDS && s_cmds[i].fn; i++) {
        if (strcmp(s_cmds[i].name, argv[0]) == 0) {
            fn = s_cmds[i].fn;
            ctx = s_cmds[i].ctx;
            break;
        }
    }
    portEXIT_CRITICAL(&s_cmd_lock);
    if (!fn) {
        heapdesk_printf(out, "Unrecognized command '%s' (try help)\n", argv[0]);
        return 127;
    }
    return fn(argc, argv, out, ctx);
}

/* ---------- NVS ---------- */
static const struct {
    const char *name;
    nvs_type_t type;
} s_types[] = {
    {"u8", NVS_TYPE_U8}, {"i8", NVS_TYPE_I8}, {"u16", NVS_TYPE_U16}, {"i16", NVS_TYPE_I16},
    {"u32", NVS_TYPE_U32}, {"i32", NVS_TYPE_I32}, {"u64", NVS_TYPE_U64}, {"i64", NVS_TYPE_I64},
    {"str", NVS_TYPE_STR}, {"blob", NVS_TYPE_BLOB},
};

static const char *type_name(nvs_type_t t)
{
    for (size_t i = 0; i < sizeof(s_types) / sizeof(s_types[0]); i++) {
        if (s_types[i].type == t) {
            return s_types[i].name;
        }
    }
    return "unknown";
}

static void add_value(cJSON *e, nvs_handle_t h, const char *key, nvs_type_t t)
{
    char num[24];
    union { uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; int32_t i32; uint64_t u64; int64_t i64; } v;
    esp_err_t r = ESP_FAIL;
    switch (t) {
    case NVS_TYPE_U8: if ((r = nvs_get_u8(h, key, &v.u8)) == ESP_OK) cJSON_AddNumberToObject(e, "value", v.u8); break;
    case NVS_TYPE_I8: if ((r = nvs_get_i8(h, key, &v.i8)) == ESP_OK) cJSON_AddNumberToObject(e, "value", v.i8); break;
    case NVS_TYPE_U16: if ((r = nvs_get_u16(h, key, &v.u16)) == ESP_OK) cJSON_AddNumberToObject(e, "value", v.u16); break;
    case NVS_TYPE_I16: if ((r = nvs_get_i16(h, key, &v.i16)) == ESP_OK) cJSON_AddNumberToObject(e, "value", v.i16); break;
    case NVS_TYPE_U32: if ((r = nvs_get_u32(h, key, &v.u32)) == ESP_OK) cJSON_AddNumberToObject(e, "value", v.u32); break;
    case NVS_TYPE_I32: if ((r = nvs_get_i32(h, key, &v.i32)) == ESP_OK) cJSON_AddNumberToObject(e, "value", v.i32); break;
    case NVS_TYPE_U64:
        if ((r = nvs_get_u64(h, key, &v.u64)) == ESP_OK) { snprintf(num, sizeof(num), "%" PRIu64, v.u64); cJSON_AddStringToObject(e, "value", num); }
        break;
    case NVS_TYPE_I64:
        if ((r = nvs_get_i64(h, key, &v.i64)) == ESP_OK) { snprintf(num, sizeof(num), "%" PRId64, v.i64); cJSON_AddStringToObject(e, "value", num); }
        break;
    case NVS_TYPE_STR: {
        size_t len = 0;
        if (nvs_get_str(h, key, NULL, &len) == ESP_OK && len <= 4001) {
            char *s = malloc(len);
            if (s && (r = nvs_get_str(h, key, s, &len)) == ESP_OK) {
                cJSON_AddStringToObject(e, "value", s);
            }
            free(s);
        }
        break;
    }
    case NVS_TYPE_BLOB: {
        size_t len = 0;
        if ((r = nvs_get_blob(h, key, NULL, &len)) == ESP_OK) {
            snprintf(num, sizeof(num), "<%u bytes>", (unsigned)len);
            cJSON_AddStringToObject(e, "value", num);
        }
        break;
    }
    default: break;
    }
    if (r != ESP_OK) {
        cJSON_AddNullToObject(e, "value");
    }
}

static const char *str_arg(const cJSON *args, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static cJSON *nvs_get_cmd(const cJSON *args, char *err, size_t errlen)
{
    const char *ns = str_arg(args, "namespace"), *key = str_arg(args, "key");
    nvs_handle_t h;
    if (!ns) return ERR("namespace required");
    esp_err_t r = nvs_open(ns, NVS_READONLY, &h);
    if (r != ESP_OK) return ERR("nvs_open %s: %s", ns, esp_err_to_name(r));
    cJSON *res = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(res, "entries");
    cJSON_AddStringToObject(res, "namespace", ns);
    if (key && key[0]) {
        nvs_type_t t;
        r = nvs_find_key(h, key, &t);
        if (r != ESP_OK) {
            nvs_close(h);
            cJSON_Delete(res);
            return ERR("%s: %s", key, esp_err_to_name(r));
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "key", key);
        cJSON_AddStringToObject(e, "type", type_name(t));
        add_value(e, h, key, t);
        cJSON_AddItemToArray(arr, e);
    } else {
        nvs_iterator_t it = NULL;
        for (r = nvs_entry_find(NVS_DEFAULT_PART_NAME, ns, NVS_TYPE_ANY, &it); r == ESP_OK && cJSON_GetArraySize(arr) < 64;
             r = nvs_entry_next(&it)) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "key", info.key);
            cJSON_AddStringToObject(e, "type", type_name(info.type));
            add_value(e, h, info.key, info.type);
            cJSON_AddItemToArray(arr, e);
        }
        nvs_release_iterator(it);
    }
    nvs_close(h);
    return res;
}

static cJSON *nvs_set_cmd(const cJSON *args, bool del, char *err, size_t errlen)
{
#ifndef CONFIG_HEAPDESK_ALLOW_NVS_WRITE
    return ERR("remote NVS writes are disabled in this firmware");
#else
    const char *ns = str_arg(args, "namespace"), *key = str_arg(args, "key"), *ty = str_arg(args, "type");
    if (!ns || !key) return ERR("namespace and key required");
    nvs_handle_t h;
    esp_err_t r = nvs_open(ns, NVS_READWRITE, &h);
    if (r != ESP_OK) return ERR("nvs_open %s: %s", ns, esp_err_to_name(r));
    if (del) {
        r = nvs_erase_key(h, key);
    } else {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "value");
        if (!ty) {
            r = ESP_ERR_INVALID_ARG;
        } else if (strcmp(ty, "str") == 0) {
            r = cJSON_IsString(v) ? nvs_set_str(h, key, v->valuestring) : ESP_ERR_INVALID_ARG;
        } else {
            /* 64-bit values arrive as strings, the rest as numbers. */
            char *end = NULL;
            int64_t sv = 0;
            uint64_t uv = 0;
            if (cJSON_IsString(v)) {
                errno = 0;
                if (ty[0] == 'u') uv = strtoull(v->valuestring, &end, 10); else sv = strtoll(v->valuestring, &end, 10);
                if (errno || !end || *end) r = ESP_ERR_INVALID_ARG;
            } else if (cJSON_IsNumber(v)) {
                sv = (int64_t)v->valuedouble;
                uv = (uint64_t)v->valuedouble;
            } else {
                r = ESP_ERR_INVALID_ARG;
            }
            if (r != ESP_ERR_INVALID_ARG) {
                if (!strcmp(ty, "u8")) r = nvs_set_u8(h, key, (uint8_t)uv);
                else if (!strcmp(ty, "i8")) r = nvs_set_i8(h, key, (int8_t)sv);
                else if (!strcmp(ty, "u16")) r = nvs_set_u16(h, key, (uint16_t)uv);
                else if (!strcmp(ty, "i16")) r = nvs_set_i16(h, key, (int16_t)sv);
                else if (!strcmp(ty, "u32")) r = nvs_set_u32(h, key, (uint32_t)uv);
                else if (!strcmp(ty, "i32")) r = nvs_set_i32(h, key, (int32_t)sv);
                else if (!strcmp(ty, "u64")) r = nvs_set_u64(h, key, uv);
                else if (!strcmp(ty, "i64")) r = nvs_set_i64(h, key, sv);
                else r = ESP_ERR_NOT_SUPPORTED;
            }
        }
    }
    if (r == ESP_OK) {
        r = nvs_commit(h);
    }
    nvs_close(h);
    if (r != ESP_OK) return ERR("%s", esp_err_to_name(r));
    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, del ? "deleted" : "written", true);
    return res;
#endif
}

/* ---------- files ---------- */
#ifdef CONFIG_HEAPDESK_ALLOW_FILES
static bool path_ok(const char *p)
{
    return p && p[0] == '/' && strlen(p) <= 128 && !strstr(p, "..");
}

static cJSON *files_list(const cJSON *args, char *err, size_t errlen)
{
    const char *path = str_arg(args, "path");
    if (!path_ok(path)) return ERR("invalid path");
    DIR *d = opendir(path);
    if (!d) return ERR("opendir %s: %s", path, strerror(errno));
    cJSON *res = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(res, "entries");
    cJSON_AddStringToObject(res, "path", path);
    char full[300];
    struct dirent *de;
    while ((de = readdir(d)) != NULL && cJSON_GetArraySize(arr) < 200) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", de->d_name);
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        bool dir = de->d_type == DT_DIR;
        if (stat(full, &st) == 0) {
            dir = S_ISDIR(st.st_mode);
            if (!dir) cJSON_AddNumberToObject(e, "size", st.st_size);
        }
        cJSON_AddBoolToObject(e, "dir", dir);
        cJSON_AddItemToArray(arr, e);
    }
    closedir(d);
    return res;
}

static cJSON *files_get(const cJSON *args, char *err, size_t errlen)
{
    const char *path = str_arg(args, "path"), *url = str_arg(args, "upload_url");
    if (!path_ok(path) || !url || !hd_url_allowed(url)) return ERR("invalid arguments");
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) return ERR("%s: no such file", path);
    if (st.st_size > CONFIG_HEAPDESK_FILE_MAX_BYTES) return ERR("file too large");
    FILE *f = fopen(path, "rb");
    if (!f) return ERR("open %s: %s", path, strerror(errno));
    esp_http_client_config_t c;
    hd_http_config(&c, url);
    c.method = HTTP_METHOD_PUT;
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_http_client_set_header(h, "Content-Type", "application/octet-stream");
    esp_err_t r = esp_http_client_open(h, st.st_size);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    char buf[1024];
    size_t n, total = 0;
    while (r == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mbedtls_sha256_update(&sha, (unsigned char *)buf, n);
        if (esp_http_client_write(h, buf, n) != (int)n) r = ESP_FAIL;
        total += n;
    }
    fclose(f);
    int status = 0;
    if (r == ESP_OK && esp_http_client_fetch_headers(h) >= 0) status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    uint8_t dig[32];
    char hex[65];
    mbedtls_sha256_finish(&sha, dig);
    mbedtls_sha256_free(&sha);
    if (r != ESP_OK || status / 100 != 2) return ERR("upload failed (%s, HTTP %d)", esp_err_to_name(r), status);
    hd_hex(dig, 32, hex);
    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "size", total);
    cJSON_AddStringToObject(res, "sha256", hex);
    return res;
}

static cJSON *files_put(const cJSON *args, char *err, size_t errlen)
{
    const char *path = str_arg(args, "path"), *url = str_arg(args, "url"), *want = str_arg(args, "sha256");
    if (!path_ok(path) || !url || !hd_url_allowed(url)) return ERR("invalid arguments");
    char tmp[140];
    snprintf(tmp, sizeof(tmp), "%s.hdtmp", path);
    esp_http_client_config_t c;
    hd_http_config(&c, url);
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t r = esp_http_client_open(h, 0);
    int64_t len = r == ESP_OK ? esp_http_client_fetch_headers(h) : -1;
    int status = esp_http_client_get_status_code(h);
    if (r != ESP_OK || status != 200 || len > CONFIG_HEAPDESK_FILE_MAX_BYTES) {
        esp_http_client_cleanup(h);
        return ERR("download failed (HTTP %d)", status);
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        esp_http_client_cleanup(h);
        return ERR("open %s: %s", tmp, strerror(errno));
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    char buf[1024];
    int n;
    size_t total = 0;
    bool ok = true;
    while ((n = esp_http_client_read(h, buf, sizeof(buf))) > 0) {
        total += n;
        if (total > CONFIG_HEAPDESK_FILE_MAX_BYTES || fwrite(buf, 1, n, f) != (size_t)n) { ok = false; break; }
        mbedtls_sha256_update(&sha, (unsigned char *)buf, n);
    }
    ok = ok && n == 0;
    fclose(f);
    esp_http_client_cleanup(h);
    uint8_t dig[32];
    char hex[65];
    mbedtls_sha256_finish(&sha, dig);
    mbedtls_sha256_free(&sha);
    hd_hex(dig, 32, hex);
    if (!ok || (want && strcmp(want, hex) != 0)) {
        unlink(tmp);
        return ERR(ok ? "sha256 mismatch" : "write failed");
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return ERR("rename: %s", strerror(errno));
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "written", total);
    return res;
}
#endif

/* ---------- OTA ---------- */
void hd_ota_mark_valid(void)
{
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed (rollback cancelled)");
    }
#endif
}

static cJSON *ota_start(const char *id, const cJSON *args, char *err, size_t errlen)
{
#ifndef CONFIG_HEAPDESK_ALLOW_OTA
    return ERR("OTA is disabled in this firmware");
#else
    const char *url = str_arg(args, "url"), *want = str_arg(args, "sha256");
    if (!url || !hd_url_allowed(url)) return ERR("invalid OTA URL");
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) return ERR("no OTA partition (check partition table)");
    esp_http_client_config_t hc;
    hd_http_config(&hc, url);
    hc.keep_alive_enable = true;
    esp_https_ota_config_t oc = {.http_config = &hc};
    esp_https_ota_handle_t ota = NULL;
    esp_err_t r = esp_https_ota_begin(&oc, &ota);
    if (r != ESP_OK) return ERR("ota begin: %s", esp_err_to_name(r));
    int total = esp_https_ota_get_image_size(ota), last = -10;
    while ((r = esp_https_ota_perform(ota)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(ota);
        int pct = total > 0 ? (int)((int64_t)got * 100 / total) : 0;
        if (pct >= last + 10) {
            last = pct;
            hd_progress(id, pct);
        }
    }
    if (r != ESP_OK || !esp_https_ota_is_complete_data_received(ota)) {
        esp_https_ota_abort(ota);
        return ERR("ota download: %s", esp_err_to_name(r));
    }
    r = esp_https_ota_finish(ota); /* validates the image and selects it for boot */
    if (r != ESP_OK) return ERR("ota finish: %s", esp_err_to_name(r));
    if (want && strlen(want) == 64) {
        uint8_t dig[32];
        char hex[65];
        if (esp_partition_get_sha256(target, dig) == ESP_OK) {
            hd_hex(dig, 32, hex);
            if (strcmp(hex, want) != 0) {
                esp_ota_set_boot_partition(esp_ota_get_running_partition());
                return ERR("sha256 mismatch: image rejected");
            }
        }
    }
    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "bytes", total);
    cJSON_AddBoolToObject(res, "rebooting", true);
    return res;
#endif
}

/* ---------- dispatch ---------- */
cJSON *hd_exec(const char *id, const char *kind, const cJSON *args, char *err, size_t errlen)
{
    if (!strcmp(kind, "ping")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "pong", true);
        cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000));
        return r;
    }
    if (!strcmp(kind, "reboot")) {
#ifdef CONFIG_HEAPDESK_ALLOW_REBOOT
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "rebooting", true);
        return r;
#else
        return ERR("remote reboot is disabled in this firmware");
#endif
    }
    if (!strcmp(kind, "get_nvs")) return nvs_get_cmd(args, err, errlen);
    if (!strcmp(kind, "set_nvs")) return nvs_set_cmd(args, false, err, errlen);
    if (!strcmp(kind, "del_nvs")) return nvs_set_cmd(args, true, err, errlen);
    if (!strcmp(kind, "run_cmd")) {
        const char *line = str_arg(args, "line");
        if (!line) return ERR("line required");
        heapdesk_out_t *out = calloc(1, sizeof(*out));
        char *copy = strdup(line);
        if (!out || !copy) {
            free(out);
            free(copy);
            return ERR("out of memory");
        }
        int code = run_console(copy, out);
        out->buf[out->len] = '\0';
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "output", out->buf);
        cJSON_AddNumberToObject(r, "code", code);
        if (out->truncated) cJSON_AddBoolToObject(r, "truncated", true);
        free(copy);
        free(out);
        return r;
    }
#ifdef CONFIG_HEAPDESK_ALLOW_FILES
    if (!strcmp(kind, "list_files")) return files_list(args, err, errlen);
    if (!strcmp(kind, "get_file")) return files_get(args, err, errlen);
    if (!strcmp(kind, "put_file")) return files_put(args, err, errlen);
#endif
    if (!strcmp(kind, "ota_start")) return ota_start(id, args, err, errlen);
    return ERR("unsupported command '%s'", kind);
}
