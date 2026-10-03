#pragma once

#include "solar_os_expansion.h"

esp_err_t solar_os_x4pro_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t binding_count);
esp_err_t solar_os_x4pro_detach(const char *name);
extern const solar_os_expansion_driver_t solar_os_x4pro_expansion_driver;
