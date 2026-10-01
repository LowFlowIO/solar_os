#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_display_surface.h"

/* Portable raster primitives; buffers are validated by their callers. RGB565
 * uses high byte first, including on little-endian CPUs. Compact RGB888 in
 * place before scaling: source reads always precede overlapping writes. */
static inline void solar_os_rgb565_from_rgb888(uint8_t *pixels, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const uint8_t *rgb = pixels + i * 3U;
        const uint16_t color = ((uint16_t)(rgb[0] & 0xf8U) << 8U) |
            ((uint16_t)(rgb[1] & 0xfcU) << 3U) | (rgb[2] >> 3U);
        pixels[i * 2U] = color >> 8U;
        pixels[i * 2U + 1U] = color;
    }
}

static inline void solar_os_rgb565_scale_row(const uint8_t *source,
    uint16_t source_width, uint8_t *output, uint16_t output_width)
{
    if (source_width == output_width) {
        memcpy(output, source, (size_t)output_width * 2U);
        return;
    }
    uint32_t source_x = 0, remainder = 0;
    for (uint16_t x = 0; x < output_width; x++) {
        output[x * 2U] = source[source_x * 2U];
        output[x * 2U + 1U] = source[source_x * 2U + 1U];
        remainder += source_width;
        while (remainder >= output_width) {
            remainder -= output_width; source_x++;
        }
    }
}

static inline void solar_os_rgb565_rotate(const uint8_t *source,
    uint16_t width, uint16_t height, size_t source_stride,
    uint8_t *output, size_t output_stride, solar_os_display_rotation_t rotation)
{
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint16_t dx = x, dy = y;
            if (rotation == SOLAR_OS_DISPLAY_ROTATION_90) { dx = height - 1U - y; dy = x; }
            else if (rotation == SOLAR_OS_DISPLAY_ROTATION_180) { dx = width - 1U - x; dy = height - 1U - y; }
            else if (rotation == SOLAR_OS_DISPLAY_ROTATION_270) { dx = y; dy = width - 1U - x; }
            const uint8_t *src = source + (size_t)y * source_stride + x * 2U;
            uint8_t *dst = output + (size_t)dy * output_stride + dx * 2U;
            dst[0] = src[0]; dst[1] = src[1];
        }
    }
}
