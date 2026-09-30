#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_gfx_mono_raster.h"

/* Independent pixel-at-a-time reference for the existing raster contract. */
static void reference(uint8_t *dst, size_t stride,
    const solar_os_display_surface_t *surface, const uint8_t *src,
    size_t src_stride, unsigned channels, int x, int y,
    int x0, int y0, int x1, int y1, bool invert)
{
    static const uint8_t bayer[] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
        const uint8_t *p = src + (size_t)(yy - y) * src_stride + (size_t)(xx - x) * channels;
        unsigned gray = channels == 1 ? p[0] : (77U*p[0] + 150U*p[1] + 29U*p[2]) / 256U;
        unsigned level = (gray * 16U + 127U) / 255U;
        bool bit = (bayer[(yy % 4) * 4 + xx % 4] < level) != invert;
        int nx, ny;
        switch (surface->rotation) {
        case SOLAR_OS_DISPLAY_ROTATION_90: nx=surface->native_width-1-yy; ny=xx; break;
        case SOLAR_OS_DISPLAY_ROTATION_180: nx=surface->native_width-1-xx; ny=surface->native_height-1-yy; break;
        case SOLAR_OS_DISPLAY_ROTATION_270: nx=yy; ny=surface->native_height-1-xx; break;
        default: nx=xx; ny=yy; break;
        }
        size_t offset = (size_t)(ny / 8) * stride + nx;
        unsigned mask = 1U << (ny % 8);
        if (bit) dst[offset] |= mask;
        else dst[offset] &= ~mask;
    }
}

int main(void)
{
    uint8_t src[40 * 40 * 3 + 40 * 5], expected[160], actual[160];
    unsigned cases = 0;
    for (unsigned rotation = 0; rotation < 4; rotation++) {
        solar_os_display_surface_t surface = {.native_width=19, .native_height=27, .rotation=rotation};
        int w = rotation & 1U ? 27 : 19, h = rotation & 1U ? 19 : 27;
        for (unsigned channels = 1; channels <= 3; channels += 2)
        for (unsigned invert = 0; invert < 2; invert++)
        for (unsigned seed = 0; seed < 256; seed++)
        for (unsigned clip = 0; clip < 3; clip++) {
            int x = clip == 1 ? -3 : 0, y = clip == 1 ? -2 : 0;
            int x0 = clip == 2 ? 3 : 0, y0 = clip == 2 ? 5 : 0;
            int x1 = clip == 2 ? w-2 : w, y1 = clip == 2 ? h-3 : h;
            size_t src_stride = (size_t)(w - x) * channels + 5;
            for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(seed + i * 71);
            /* Canary prefix/suffix, native row padding and untouched bits. */
            for (size_t i = 0; i < sizeof(actual); i++) actual[i] = (uint8_t)(i * 13 + seed);
            memcpy(expected, actual, sizeof(actual));
            reference(expected + 7, 21, &surface, src, src_stride, channels,
                x, y, x0, y0, x1, y1, invert);
            solar_os_gfx_mono_blit_unscaled(actual + 7, 21, &surface, src, src_stride,
                channels, x, y, x0, y0, x1, y1, invert);
            assert(!memcmp(actual, expected, sizeof(actual)));
            cases++;
        }
    }
    printf("gfx mono raster: %u rotation/format/polarity/clipping cases passed\n", cases);
    return 0;
}
