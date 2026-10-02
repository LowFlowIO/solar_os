#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

esp_err_t solar_os_axp2101_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count);
esp_err_t solar_os_axp2101_detach(const char *name);

/* Hold the PMIC attachment while a panel uses its ALDO3 supply. */
esp_err_t solar_os_axp2101_acquire_aldo3(const char *i2c_bus, uint8_t address);
void solar_os_axp2101_release_aldo3(void);
esp_err_t solar_os_axp2101_set_aldo3(bool enabled);
