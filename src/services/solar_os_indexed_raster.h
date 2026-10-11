#pragma once

#include <stddef.h>
#include <stdint.h>
#include "solar_os_display_surface.h"

/* Portable INDEX2/INDEX8 raster primitives. Callers validate buffers, dimensions
 * and palettes. INDEX2 packs the first pixel into the low two bits. */
static inline uint8_t solar_os_indexed_pixel(const uint8_t *row,
    uint16_t x, solar_os_display_format_t format)
{
    return format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? row[x] :
        (uint8_t)((row[x >> 2U] >> ((x & 3U) * 2U)) & 3U);
}

static inline void solar_os_indexed_set(uint8_t *row, uint16_t x,
    uint8_t index, solar_os_display_format_t format)
{
    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8) row[x] = index;
    else {
        unsigned shift = (x & 3U) * 2U;
        row[x >> 2U] = (uint8_t)((row[x >> 2U] & ~(3U << shift)) |
            ((index & 3U) << shift));
    }
}

static inline void solar_os_indexed_rotate(const uint8_t *source,
    uint16_t width, uint16_t height, size_t source_stride,
    uint8_t *output, size_t output_stride, solar_os_display_format_t format,
    solar_os_display_rotation_t rotation)
{
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint16_t dx = x, dy = y;
            if (rotation == SOLAR_OS_DISPLAY_ROTATION_90) { dx = height - 1U - y; dy = x; }
            else if (rotation == SOLAR_OS_DISPLAY_ROTATION_180) { dx = width - 1U - x; dy = height - 1U - y; }
            else if (rotation == SOLAR_OS_DISPLAY_ROTATION_270) { dx = y; dy = width - 1U - x; }
            solar_os_indexed_set(output + (size_t)dy * output_stride, dx,
                solar_os_indexed_pixel(source + (size_t)y * source_stride, x, format), format);
        }
    }
}

static inline void solar_os_indexed_palette(const uint16_t *palette,
    solar_os_display_format_t format, bool inverted, uint16_t *colors)
{
    size_t count = format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 256U : 4U;
    for (size_t i = 0; i < count; i++) {
        /* INDEX2 is an ordinal shade ramp; INDEX8 holds arbitrary colors. */
        colors[i] = format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 ?
            palette[inverted ? 3U - i : i] :
            (inverted ? (uint16_t)~palette[i] : palette[i]);
    }
}

/* Palette colors use native uint16_t values; output uses LCD wire byte order. */
static inline void solar_os_indexed_scale_row(const uint8_t *source,
    uint16_t source_width, solar_os_display_format_t format,
    const uint16_t *colors, uint8_t *output, uint16_t output_width)
{
    uint16_t source_x = 0U;
    uint32_t remainder = 0U;
    for (uint16_t x = 0; x < output_width; x++) {
        uint16_t color = colors[solar_os_indexed_pixel(source, source_x, format)];
        output[x * 2U] = (uint8_t)(color >> 8U);
        output[x * 2U + 1U] = (uint8_t)color;
        remainder += source_width;
        while (remainder >= output_width) { remainder -= output_width; source_x++; }
    }
}
