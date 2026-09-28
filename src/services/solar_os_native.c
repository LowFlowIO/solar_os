#include "solar_os_native.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_elf.h"
#include "sdkconfig.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

static atomic_bool native_running;
static atomic_bool native_symbols_registered;
static solar_os_native_run_options_t native_active_options;

static esp_err_t native_write_utf8(const char *text, size_t text_len)
{
    if (text == NULL || (text_len > 0U && native_active_options.write == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (text_len == 0U) {
        return ESP_OK;
    }
    return native_active_options.write(text,
                                       text_len,
                                       native_active_options.write_user);
}

static const solar_os_native_host_api_v1_t native_host_v1 = {
    .abi_version = SOLAR_OS_NATIVE_ABI_VERSION,
    .struct_size = sizeof(solar_os_native_host_api_v1_t),
    .target = CONFIG_IDF_TARGET,
    .firmware_version = SOLAR_OS_VERSION,
    .write_utf8 = native_write_utf8,
};

const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void)
{
    return &native_host_v1;
}

static const struct esp_elfsym native_symbols[] = {
    ESP_ELFSYM_EXPORT(solar_os_native_host_v1),
    ESP_ELFSYM_END,
};

static void native_set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", text != NULL ? text : "native module failed");
    }
}

static esp_err_t native_register_symbols(char *detail, size_t detail_len)
{
    if (atomic_load(&native_symbols_registered)) {
        return ESP_OK;
    }
    const int ret = esp_elf_register_symbol(native_symbols);
    if (ret != 0) {
        native_set_detail(detail, detail_len, "could not register the SolarOS native ABI");
        return ESP_FAIL;
    }
    atomic_store(&native_symbols_registered, true);
    return ESP_OK;
}

esp_err_t solar_os_native_run(const char *path,
                              int argc,
                              char **argv,
                              const solar_os_native_run_options_t *options,
                              solar_os_native_run_result_t *result,
                              char *detail,
                              size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || argc < 1 || argv == NULL ||
        options == NULL || options->write == NULL) {
        native_set_detail(detail, detail_len, "missing path, arguments, or output callback");
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&native_running, true)) {
        native_set_detail(detail, detail_len, "another native module is already running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    uint8_t *data = NULL;
    esp_elf_t elf;
    bool elf_initialized = false;
    solar_os_storage_metadata_t metadata;
    solar_os_native_elf_info_t elf_info;

    err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK || metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE) {
        native_set_detail(detail, detail_len, "ELF file was not found");
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }
    if (metadata.size_bytes < 52U ||
        metadata.size_bytes > SOLAR_OS_NATIVE_ELF_MAX_BYTES ||
        metadata.size_bytes > SIZE_MAX) {
        native_set_detail(detail, detail_len, "ELF file size is outside the supported range");
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    const size_t file_size = (size_t)metadata.size_bytes;
    data = solar_os_memory_alloc(file_size,
                                 SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                 "native.elf");
    if (data == NULL) {
        native_set_detail(detail, detail_len, "not enough external memory for the ELF file");
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    size_t read_len = 0U;
    err = solar_os_storage_read_file(path, data, file_size, &read_len);
    if (err != ESP_OK || read_len != file_size) {
        native_set_detail(detail, detail_len, "could not read the complete ELF file");
        err = ESP_FAIL;
        goto done;
    }

    err = solar_os_native_elf_validate(data,
                                       file_size,
                                       SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                       &elf_info,
                                       detail,
                                       detail_len);
    if (err != ESP_OK) {
        goto done;
    }
    err = native_register_symbols(detail, detail_len);
    if (err != ESP_OK) {
        goto done;
    }

    int loader_ret = esp_elf_init(&elf);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "Espressif ELF loader initialization failed");
        err = ESP_FAIL;
        goto done;
    }
    elf_initialized = true;

    loader_ret = esp_elf_relocate(&elf, data);
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "ELF relocation or symbol resolution failed");
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    native_active_options = *options;
    loader_ret = esp_elf_request(&elf, 0, argc, argv);
    memset(&native_active_options, 0, sizeof(native_active_options));
    if (loader_ret != 0) {
        native_set_detail(detail, detail_len, "ELF entry point failed to run");
        err = ESP_FAIL;
        goto done;
    }

    if (result != NULL) {
        *result = (solar_os_native_run_result_t){.elf = elf_info};
    }

done:
    memset(&native_active_options, 0, sizeof(native_active_options));
    if (elf_initialized) {
        esp_elf_deinit(&elf);
    }
    solar_os_memory_free(data);
    atomic_store(&native_running, false);
    return err;
}
