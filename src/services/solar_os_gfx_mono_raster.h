#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_display_surface.h"

/* Internal, unscaled GRAY8/RGB888 blit into u8g2's vertical-eight-bit tiles.
 * The caller validates buffers and clips the logical rectangle. Preserve bits
 * outside that rectangle, including partial bytes at either end. Coalesce
 * adjacent bits when rotation maps a logical row along a native tile column:
 * one framebuffer read/write per byte instead of one per pixel in PSRAM. */
static inline void solar_os_gfx_mono_blit_unscaled(
    uint8_t *dst, size_t dst_stride, const solar_os_display_surface_t *surface,
    const uint8_t *src, size_t src_stride, unsigned channels,
    int x, int y, int x0, int y0, int x1, int y1, bool invert)
{
    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
    };
    int dx = 1, dy = 0;
    if (surface->rotation == SOLAR_OS_DISPLAY_ROTATION_90) { dx = 0; dy = 1; }
    else if (surface->rotation == SOLAR_OS_DISPLAY_ROTATION_180) dx = -1;
    else if (surface->rotation == SOLAR_OS_DISPLAY_ROTATION_270) { dx = 0; dy = -1; }
    for (int yy = y0; yy < y1; yy++) {
        int nx = x0, ny = yy;
        switch (surface->rotation) {
        case SOLAR_OS_DISPLAY_ROTATION_90:
            nx = (int)surface->native_width - 1 - yy; ny = x0; break;
        case SOLAR_OS_DISPLAY_ROTATION_180:
            nx = (int)surface->native_width - 1 - x0;
            ny = (int)surface->native_height - 1 - yy; break;
        case SOLAR_OS_DISPLAY_ROTATION_270:
            nx = yy; ny = (int)surface->native_height - 1 - x0; break;
        default: break;
        }
        const uint8_t *pixel = src + (size_t)(yy - y) * src_stride +
            (size_t)(x0 - x) * channels;
        size_t cached = (size_t)(ny >> 3) * dst_stride + nx;
        uint8_t bits = dst[cached];
        for (int xx = x0; xx < x1; xx++, nx += dx, ny += dy, pixel += channels) {
            size_t offset = (size_t)(ny >> 3) * dst_stride + nx;
            if (offset != cached) {
                dst[cached] = bits; cached = offset; bits = dst[cached];
            }
            unsigned luminance = channels == 1U ? pixel[0] :
                (77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2]) >> 8U;
            unsigned threshold = (luminance * 16U + 127U) / 255U;
            uint8_t mask = (uint8_t)(1U << (ny & 7));
            bool white = bayer[yy & 3][xx & 3] < threshold;
            if (white != invert) bits |= mask;
            else bits &= (uint8_t)~mask;
        }
        dst[cached] = bits;
    }
}
