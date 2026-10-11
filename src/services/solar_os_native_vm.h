#pragma once
#include "solar_os.h"

esp_err_t solar_os_native_vm_launch(solar_os_context_t *ctx, int argc, char **argv);
const void *solar_os_native_vm_get_service(const char *name, uint32_t abi,
    uint32_t minimum_size);
