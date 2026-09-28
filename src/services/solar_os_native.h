#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_elf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_ELF_MAX_BYTES (2U * 1024U * 1024U)

typedef esp_err_t (*solar_os_native_write_fn)(const char *text,
                                              size_t text_len,
                                              void *user);

typedef struct {
    solar_os_native_write_fn write;
    void *write_user;
} solar_os_native_run_options_t;

typedef struct {
    solar_os_native_elf_info_t elf;
} solar_os_native_run_result_t;

esp_err_t solar_os_native_run(const char *path,
                              int argc,
                              char **argv,
                              const solar_os_native_run_options_t *options,
                              solar_os_native_run_result_t *result,
                              char *detail,
                              size_t detail_len);

#ifdef __cplusplus
}
#endif
