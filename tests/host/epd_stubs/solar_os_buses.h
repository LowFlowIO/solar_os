#pragma once
#include "driver/spi_master.h"
esp_err_t solar_os_bus_spi_add_device(const char *name,
                                     const spi_device_interface_config_t *config,
                                     spi_device_handle_t *device);
