#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_JPEG_GRAY8 = 1,
    SOLAR_OS_JPEG_RGB565 = 2, /* high byte first */
    SOLAR_OS_JPEG_RGB888 = 3,
} solar_os_jpeg_format_t;

/* NOT_SUPPORTED selects the stb fallback; output is owned by heap_caps_free.
 * max_width/height of zero preserve source dimensions. No persistent state. */
esp_err_t solar_os_jpeg_fast_decode(const uint8_t *data, size_t length,
    uint32_t max_pixels, uint32_t max_width, uint32_t max_height,
    solar_os_jpeg_format_t format, uint8_t **pixels, uint32_t *width, uint32_t *height);
