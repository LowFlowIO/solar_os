#include "solar_os_module_packages.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "solar_os_board_caps.h"
#include "solar_os_crypto.h"
#include "solar_os_http_client.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_native_elf.h"
#include "solar_os_native_abi.h"
#include "solar_os_ota_key.h"
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#ifndef SOLAR_OS_MODULE_REPOSITORY_URL
#define SOLAR_OS_MODULE_REPOSITORY_URL "https://solar-os.eu/ota/modules"
#endif

#define MODULE_CATALOG_MAX_BYTES (32U * 1024U)
#define MODULE_SIGNATURE_MAX_BYTES 512U
#define MODULE_HTTP_TIMEOUT_MS 15000U
#define MODULE_HTTP_DEADLINE_MS 60000U

static atomic_bool module_operation_running;

typedef struct {
    FILE *file;
    solar_os_crypto_sha256_t sha256;
    const solar_os_module_install_options_t *options;
    uint32_t expected_size;
    uint32_t bytes;
} module_download_t;

static void module_set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", text != NULL ? text : "module operation failed");
    }
}

static bool module_name_valid(const char *text)
{
    if (text == NULL || text[0] == '\0' || strlen(text) >= SOLAR_OS_MODULE_PACKAGE_ID_MAX) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') {
            return false;
        }
    }
    return strcmp(text, ".") != 0 && strcmp(text, "..") != 0;
}

static bool module_relative_path_valid(const char *path)
{
    if (path == NULL || path[0] == '\0' || path[0] == '/' || strstr(path, "..") != NULL) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.' && *p != '/') {
            return false;
        }
    }
    return true;
}

static esp_err_t module_join_url(const char *base,
                                 const char *path,
                                 char *out,
                                 size_t out_len)
{
    if (base == NULL || path == NULL || out == NULL || out_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t base_len = strlen(base);
    const int written = snprintf(out,
                                 out_len,
                                 "%s%s%s",
                                 base,
                                 base_len > 0U && base[base_len - 1U] == '/' ? "" : "/",
                                 path[0] == '/' ? path + 1 : path);
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t module_release_url(char *out, size_t out_len)
{
    return module_join_url(SOLAR_OS_MODULE_REPOSITORY_URL,
                           SOLAR_OS_VERSION,
                           out,
                           out_len);
}

static esp_err_t module_fetch_body(const char *url,
                                   size_t max_bytes,
                                   solar_os_http_buffered_response_t *response)
{
    const solar_os_http_request_options_t request = {
        .url = url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .user_agent = "SolarOS-pkg/" SOLAR_OS_VERSION,
        .follow_redirects = true,
        .timeout_ms = MODULE_HTTP_TIMEOUT_MS,
        .deadline_ms = MODULE_HTTP_DEADLINE_MS,
        .receive_buffer_size = 2048U,
        .transmit_buffer_size = 1024U,
    };
    esp_err_t err = solar_os_http_perform_buffered(&request, max_bytes + 1U, response);
    if (err != ESP_OK) {
        return err;
    }
    if (response->response.status_code != 200) {
        return ESP_ERR_NOT_FOUND;
    }
    if (response->body_truncated || response->body_len > max_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    response->body[response->body_len] = '\0';
    return ESP_OK;
}

static esp_err_t module_verify_catalog_signature(const uint8_t *catalog,
                                                 size_t catalog_len,
                                                 const char *signature)
{
    uint8_t der[SOLAR_OS_CRYPTO_ECDSA_P256_DER_SIGNATURE_MAX];
    size_t der_len = 0U;
    esp_err_t err = solar_os_crypto_base64_decode(signature,
                                                  der,
                                                  sizeof(der),
                                                  &der_len);
    if (err == ESP_OK) {
        err = solar_os_crypto_ecdsa_p256_sha256_verify_pem(
            SOLAR_OS_OTA_PUBLIC_KEY_PEM,
            catalog,
            catalog_len,
            der,
            der_len);
    }
    return err;
}

static bool module_capability_available(const char *required)
{
    char available[SOLAR_OS_BOARD_CAPABILITIES_TEXT_MAX];
    if (required == NULL ||
        !solar_os_board_capabilities_format(available, sizeof(available))) {
        return false;
    }

    const size_t required_len = strlen(required);
    const char *cursor = available;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            cursor++;
        }
        const char *end = strchr(cursor, ' ');
        const size_t len = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (len == required_len && memcmp(cursor, required, len) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

static esp_err_t module_parse_package(const solar_os_json_value_t *value,
                                      const char *release_url,
                                      solar_os_module_package_t *package)
{
    if (!solar_os_json_is_object(value) || package == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memset(package, 0, sizeof(*package));

    char artifact_path[192];
    esp_err_t err = solar_os_json_get_path_string(value, "id",
                                                   package->id, sizeof(package->id));
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "name",
                                            package->name, sizeof(package->name));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "version",
                                            package->version, sizeof(package->version));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "description",
                                            package->description,
                                            sizeof(package->description));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(value,
                                            "minimum_host_api_size",
                                            &package->minimum_host_api_size);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "artifact.path",
                                            artifact_path, sizeof(artifact_path));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(value, "artifact.size",
                                            &package->artifact_size);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(value, "artifact.sha256",
                                            package->artifact_sha256,
                                            sizeof(package->artifact_sha256));
    }
    if (err != ESP_OK || !module_name_valid(package->id) ||
        package->name[0] == '\0' || package->version[0] == '\0' ||
        package->minimum_host_api_size == 0U ||
        !module_relative_path_valid(artifact_path) ||
        package->artifact_size < 52U ||
        package->artifact_size > SOLAR_OS_NATIVE_ELF_MAX_BYTES ||
        !solar_os_crypto_sha256_hex_is_valid(package->artifact_sha256) ||
        module_join_url(release_url,
                        artifact_path,
                        package->artifact_url,
                        sizeof(package->artifact_url)) != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    package->compatible = true;
    if (package->minimum_host_api_size > sizeof(solar_os_native_host_api_v1_t)) {
        package->compatible = false;
        snprintf(package->incompatibility,
                 sizeof(package->incompatibility),
                 "needs host API size %u",
                 (unsigned)package->minimum_host_api_size);
    }

    const solar_os_json_value_t *capabilities =
        solar_os_json_path_get(value, "required_capabilities");
    if (!solar_os_json_is_array(capabilities)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t capability_count = solar_os_json_array_size(capabilities);
    for (size_t i = 0U; i < capability_count; i++) {
        char capability[40];
        if (solar_os_json_get_string(solar_os_json_array_get(capabilities, i),
                                     capability,
                                     sizeof(capability)) != ESP_OK ||
            capability[0] == '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (package->compatible && !module_capability_available(capability)) {
            package->compatible = false;
            snprintf(package->incompatibility,
                     sizeof(package->incompatibility),
                     "needs %s capability",
                     capability);
        }
    }

    char installed_path[SOLAR_OS_STORAGE_PATH_MAX];
    bool installed = false;
    if (solar_os_module_package_path(package->id,
                                     installed_path,
                                     sizeof(installed_path)) == ESP_OK) {
        (void)solar_os_storage_exists(installed_path, &installed);
    }
    package->installed = installed;
    return ESP_OK;
}

static esp_err_t module_parse_catalog(const uint8_t *body,
                                      size_t body_len,
                                      const char *release_url,
                                      solar_os_module_catalog_t *catalog)
{
    solar_os_json_doc_t *document = NULL;
    esp_err_t err = solar_os_json_parse((const char *)body, body_len, &document);
    if (err != ESP_OK) {
        return err;
    }

    const solar_os_json_value_t *root = solar_os_json_root(document);
    char schema[40];
    char project[16];
    uint32_t schema_version = 0U;
    err = solar_os_json_get_path_string(root, "schema", schema, sizeof(schema));
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(root, "schema_version", &schema_version);
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "project", project, sizeof(project));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "host.version",
                                            catalog->host_version,
                                            sizeof(catalog->host_version));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "host.source_commit",
                                            catalog->source_commit,
                                            sizeof(catalog->source_commit));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_string(root, "host.target",
                                            catalog->target,
                                            sizeof(catalog->target));
    }
    if (err == ESP_OK) {
        err = solar_os_json_get_path_uint32(root, "host.native_abi",
                                            &catalog->native_abi);
    }
    if (err != ESP_OK || strcmp(schema, "solaros.module_catalog") != 0 ||
        schema_version != 1U || strcmp(project, "SolarOS") != 0 ||
        catalog->host_version[0] == '\0' || catalog->source_commit[0] == '\0' ||
        catalog->target[0] == '\0' ||
        strcmp(catalog->host_version, SOLAR_OS_VERSION) != 0 ||
        strcmp(catalog->target, CONFIG_IDF_TARGET) != 0 ||
        catalog->native_abi != SOLAR_OS_NATIVE_ABI_VERSION) {
        solar_os_json_free(document);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const solar_os_json_value_t *modules = solar_os_json_path_get(root, "modules");
    const size_t count = solar_os_json_array_size(modules);
    if (!solar_os_json_is_array(modules) || count > SOLAR_OS_MODULE_PACKAGE_COUNT_MAX) {
        solar_os_json_free(document);
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0U; i < count; i++) {
        err = module_parse_package(solar_os_json_array_get(modules, i),
                                   release_url,
                                   &catalog->packages[i]);
        if (err != ESP_OK) {
            solar_os_json_free(document);
            return err;
        }
        for (size_t previous = 0U; previous < i; previous++) {
            if (strcmp(catalog->packages[previous].id,
                       catalog->packages[i].id) == 0) {
                solar_os_json_free(document);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    catalog->count = count;
    solar_os_json_free(document);
    return ESP_OK;
}

esp_err_t solar_os_module_catalog_fetch(solar_os_module_catalog_t **out_catalog,
                                        char *detail,
                                        size_t detail_len)
{
    if (out_catalog == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_catalog = NULL;
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }

    char release_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    char catalog_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    char signature_url[SOLAR_OS_MODULE_PACKAGE_URL_MAX];
    if (module_release_url(release_url, sizeof(release_url)) != ESP_OK ||
        module_join_url(release_url, "catalog.json",
                        catalog_url, sizeof(catalog_url)) != ESP_OK ||
        module_join_url(release_url, "catalog.sig",
                        signature_url, sizeof(signature_url)) != ESP_OK) {
        module_set_detail(detail, detail_len, "module repository URL is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    solar_os_http_buffered_response_t catalog_response;
    solar_os_http_buffered_response_t signature_response;
    memset(&catalog_response, 0, sizeof(catalog_response));
    memset(&signature_response, 0, sizeof(signature_response));
    esp_err_t err = module_fetch_body(catalog_url,
                                      MODULE_CATALOG_MAX_BYTES,
                                      &catalog_response);
    if (err == ESP_OK) {
        err = module_fetch_body(signature_url,
                                MODULE_SIGNATURE_MAX_BYTES,
                                &signature_response);
    }
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "could not download the module catalog");
        goto done;
    }

    err = module_verify_catalog_signature(catalog_response.body,
                                          catalog_response.body_len,
                                          (const char *)signature_response.body);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "module catalog signature is invalid");
        goto done;
    }

    solar_os_module_catalog_t *catalog = solar_os_memory_calloc(
        1U,
        sizeof(*catalog),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
        "modules.catalog");
    if (catalog == NULL) {
        err = ESP_ERR_NO_MEM;
        module_set_detail(detail, detail_len, "not enough external memory for the catalog");
        goto done;
    }
    err = module_parse_catalog(catalog_response.body,
                               catalog_response.body_len,
                               release_url,
                               catalog);
    if (err != ESP_OK) {
        solar_os_memory_free(catalog);
        module_set_detail(detail, detail_len,
                          err == ESP_ERR_NOT_SUPPORTED ?
                              "catalog does not match this firmware, target, or ABI" :
                              "module catalog is invalid");
        goto done;
    }
    catalog->signature_verified = true;
    *out_catalog = catalog;

done:
    solar_os_http_buffered_response_clear(&signature_response);
    solar_os_http_buffered_response_clear(&catalog_response);
    return err;
}

void solar_os_module_catalog_free(solar_os_module_catalog_t *catalog)
{
    solar_os_memory_free(catalog);
}

esp_err_t solar_os_module_package_path(const char *id,
                                       char *path,
                                       size_t path_len)
{
    if (!module_name_valid(id) || path == NULL || path_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    char relative[64];
    const int written = snprintf(relative, sizeof(relative), "modules/%s.app.elf", id);
    if (written < 0 || (size_t)written >= sizeof(relative)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return solar_os_storage_default_path(relative, path, path_len);
}

esp_err_t solar_os_module_package_foreach_installed(
    solar_os_module_package_visit_fn visit,
    void *user)
{
    static const char suffix[] = ".app.elf";
    char modules_path[SOLAR_OS_STORAGE_PATH_MAX];

    if (visit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_storage_default_path("modules",
                                                  modules_path,
                                                  sizeof(modules_path));
    if (err != ESP_OK) {
        return err;
    }

    errno = 0;
    DIR *dir = opendir(modules_path);
    if (dir == NULL) {
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const size_t name_len = strlen(entry->d_name);
        const size_t suffix_len = sizeof(suffix) - 1U;
        if (name_len <= suffix_len ||
            strcmp(&entry->d_name[name_len - suffix_len], suffix) != 0) {
            continue;
        }

        const size_t id_len = name_len - suffix_len;
        if (id_len >= SOLAR_OS_MODULE_PACKAGE_ID_MAX) {
            continue;
        }
        char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
        memcpy(id, entry->d_name, id_len);
        id[id_len] = '\0';
        if (!module_name_valid(id)) {
            continue;
        }

        char path[SOLAR_OS_STORAGE_PATH_MAX];
        solar_os_storage_metadata_t metadata;
        if (solar_os_module_package_path(id, path, sizeof(path)) != ESP_OK ||
            solar_os_storage_stat(path, &metadata) != ESP_OK ||
            metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
            continue;
        }
        if (!visit(id, user)) {
            break;
        }
    }

    closedir(dir);
    return ESP_OK;
}

static bool module_download_cancelled(void *user)
{
    module_download_t *download = (module_download_t *)user;
    return download != NULL && download->options != NULL &&
        download->options->should_cancel != NULL &&
        download->options->should_cancel(download->options->user);
}

static esp_err_t module_download_event(const solar_os_http_event_t *event, void *user)
{
    module_download_t *download = (module_download_t *)user;
    if (event == NULL || download == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type == SOLAR_OS_HTTP_EVENT_RESPONSE) {
        if (event->status_code != 200 ||
            (event->content_length >= 0 &&
             (uint64_t)event->content_length != download->expected_size)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        return ESP_OK;
    }
    if (event->type != SOLAR_OS_HTTP_EVENT_DATA || event->data_len == 0U) {
        return ESP_OK;
    }
    if (download->bytes > download->expected_size ||
        event->data_len > download->expected_size - download->bytes ||
        fwrite(event->data, 1U, event->data_len, download->file) != event->data_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = solar_os_crypto_sha256_update(&download->sha256,
                                                  event->data,
                                                  event->data_len);
    if (err != ESP_OK) {
        return err;
    }
    download->bytes += (uint32_t)event->data_len;
    if (download->options != NULL && download->options->progress != NULL) {
        download->options->progress(download->bytes,
                                    download->expected_size,
                                    download->options->user);
    }
    return ESP_OK;
}

static esp_err_t module_validate_download(const char *path,
                                          uint32_t size,
                                          char *detail,
                                          size_t detail_len)
{
    uint8_t *data = solar_os_memory_alloc(size,
                                          SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                          "modules.validate");
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t read_len = 0U;
    esp_err_t err = solar_os_storage_read_file(path, data, size, &read_len);
    if (err == ESP_OK && read_len != size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = solar_os_native_elf_validate(data,
                                           size,
                                           SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                           NULL,
                                           detail,
                                           detail_len);
    }
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_module_package_install(const char *id,
                                          const solar_os_module_install_options_t *options,
                                          solar_os_module_install_result_t *result,
                                          char *detail,
                                          size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    if (!module_name_valid(id)) {
        module_set_detail(detail, detail_len, "invalid module name");
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&module_operation_running, true)) {
        module_set_detail(detail, detail_len, "another module operation is running");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    solar_os_module_catalog_t *catalog = NULL;
    err = solar_os_module_catalog_fetch(&catalog, detail, detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    const solar_os_module_package_t *package = NULL;
    for (size_t i = 0U; i < catalog->count; i++) {
        if (strcmp(catalog->packages[i].id, id) == 0) {
            package = &catalog->packages[i];
            break;
        }
    }
    if (package == NULL) {
        module_set_detail(detail, detail_len, "module is not present in the catalog");
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (!package->compatible) {
        module_set_detail(detail, detail_len, package->incompatibility);
        err = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }

    char module_dir[SOLAR_OS_STORAGE_PATH_MAX];
    char active_path[SOLAR_OS_STORAGE_PATH_MAX];
    char staged_path[SOLAR_OS_STORAGE_PATH_MAX];
    char backup_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path("modules", module_dir, sizeof(module_dir)) != ESP_OK ||
        solar_os_module_package_path(id, active_path, sizeof(active_path)) != ESP_OK ||
        solar_os_storage_sibling_path(active_path, ".tmp",
                                      staged_path, sizeof(staged_path)) != ESP_OK ||
        solar_os_storage_sibling_path(active_path, ".bak",
                                      backup_path, sizeof(backup_path)) != ESP_OK) {
        module_set_detail(detail, detail_len, "module storage path is too long");
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    err = solar_os_storage_makedirs(module_dir, true);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "could not create the module directory");
        goto done;
    }
    (void)solar_os_storage_remove(staged_path);

    FILE *file = fopen(staged_path, "wb");
    if (file == NULL) {
        module_set_detail(detail, detail_len, "could not create the staged module file");
        err = ESP_FAIL;
        goto done;
    }
    module_download_t download = {
        .file = file,
        .options = options,
        .expected_size = package->artifact_size,
    };
    solar_os_crypto_sha256_init(&download.sha256);
    err = solar_os_crypto_sha256_start(&download.sha256);

    const solar_os_http_request_options_t request = {
        .url = package->artifact_url,
        .method = SOLAR_OS_HTTP_METHOD_GET,
        .user_agent = "SolarOS-pkg/" SOLAR_OS_VERSION,
        .follow_redirects = true,
        .timeout_ms = MODULE_HTTP_TIMEOUT_MS,
        .read_poll_ms = 250U,
        .deadline_ms = MODULE_HTTP_DEADLINE_MS,
        .should_cancel = module_download_cancelled,
        .cancel_user_data = &download,
        .receive_buffer_size = 4096U,
        .transmit_buffer_size = 1024U,
        .event_handler = module_download_event,
        .user_data = &download,
    };
    solar_os_http_request_t *http = NULL;
    solar_os_http_response_t response;
    if (err == ESP_OK) {
        err = solar_os_http_request_create(&request, &http);
    }
    if (err == ESP_OK) {
        err = solar_os_http_request_perform(http, &response);
    }
    if (http != NULL) {
        const esp_err_t destroy_err = solar_os_http_request_destroy(http);
        if (err == ESP_OK && destroy_err != ESP_OK) {
            err = destroy_err;
        }
    }
    uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    if (err == ESP_OK && (response.status_code != 200 ||
                          download.bytes != package->artifact_size)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = solar_os_crypto_sha256_finish(&download.sha256, digest);
    }
    if (err == ESP_OK &&
        !solar_os_crypto_sha256_matches_hex(digest, package->artifact_sha256)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) {
        err = solar_os_storage_sync_file(file);
    }
    if (fclose(file) != 0 && err == ESP_OK) {
        err = ESP_FAIL;
    }
    solar_os_crypto_sha256_free(&download.sha256);
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(staged_path);
        module_set_detail(detail, detail_len,
                          err == ESP_ERR_INVALID_CRC ?
                              "downloaded module hash does not match the signed catalog" :
                              "module download failed or was cancelled");
        goto done;
    }

    err = module_validate_download(staged_path,
                                   package->artifact_size,
                                   detail,
                                   detail_len);
    if (err == ESP_OK) {
        err = solar_os_storage_replace_file(staged_path,
                                            active_path,
                                            backup_path);
    }
    if (err != ESP_OK) {
        (void)solar_os_storage_remove(staged_path);
        if (detail == NULL || detail[0] == '\0') {
            module_set_detail(detail, detail_len, "module validation or activation failed");
        }
        goto done;
    }

    if (result != NULL) {
        snprintf(result->id, sizeof(result->id), "%s", package->id);
        snprintf(result->version, sizeof(result->version), "%s", package->version);
        snprintf(result->path, sizeof(result->path), "%s", active_path);
        result->bytes = package->artifact_size;
    }

done:
    solar_os_module_catalog_free(catalog);
    atomic_store(&module_operation_running, false);
    return err;
}

esp_err_t solar_os_module_package_remove(const char *id,
                                         char *detail,
                                         size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (atomic_exchange(&module_operation_running, true)) {
        module_set_detail(detail, detail_len, "another module operation is running");
        return ESP_ERR_INVALID_STATE;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_module_package_path(id, path, sizeof(path));
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "invalid module name or storage path");
        goto done;
    }
    bool exists = false;
    err = solar_os_storage_exists(path, &exists);
    if (err != ESP_OK || !exists) {
        module_set_detail(detail, detail_len, "module is not installed");
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    err = solar_os_storage_remove(path);
    if (err != ESP_OK) {
        module_set_detail(detail, detail_len, "could not remove the installed module");
    }

done:
    atomic_store(&module_operation_running, false);
    return err;
}
