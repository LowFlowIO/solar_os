#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA 94U
#define SOLAR_OS_NATIVE_ELF_MACHINE_RISCV 243U

typedef struct {
    size_t file_size;
    uint32_t entry;
    uint16_t machine;
    uint16_t program_count;
    uint16_t section_count;
} solar_os_native_elf_info_t;

esp_err_t solar_os_native_elf_validate(const uint8_t *data,
                                       size_t data_len,
                                       uint16_t expected_machine,
                                       solar_os_native_elf_info_t *info,
                                       char *detail,
                                       size_t detail_len);

const char *solar_os_native_elf_machine_name(uint16_t machine);

#ifdef __cplusplus
}
#endif
