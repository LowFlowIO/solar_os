#pragma once

#include <stdint.h>
#include "esp_err.h"

/* SolarOS-owned stable IDs; these are not the OEM screenType values. */
typedef enum {
    EPD_XTEINK_UNKNOWN = 0,
    EPD_XTEINK_SSD1677 = 1,
    EPD_XTEINK_UC8179 = 2,
    EPD_XTEINK_UC8279 = 3,
} epd_xteink_controller_t;

epd_xteink_controller_t epd_xteink_classify(const uint8_t version[5], uint8_t status);
esp_err_t epd_xteink_identify(const char *spi_bus, int cs, int dc, int reset,
                             int mosi, epd_xteink_controller_t *controller);
