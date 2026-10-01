#include "solar_os_mpeg.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
void solar_os_mpeg_rgb565_s3(const int16_t *planes, uint8_t *output);
#define MPEG_SIMD 1
#else
#define MPEG_SIMD 0
#endif

bool solar_os_mpeg_simd_available(void) { return MPEG_SIMD != 0; }
static uint8_t clamp(int value) { return value < 0 ? 0 : value > 255 ? 255 : value; }
static void pixel(uint8_t *out, int y, int cb, int cr)
{
    int c = 298 * (y - 16), d = cb - 128, e = cr - 128;
    uint8_t r = clamp((c + 409 * e + 128) >> 8);
    uint8_t g = clamp((c - 100 * d - 208 * e + 128) >> 8);
    uint8_t b = clamp((c + 516 * d + 128) >> 8);
    uint16_t rgb = ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
    out[0] = rgb >> 8;
    out[1] = rgb;
}
bool solar_os_mpeg_simd_selftest(void)
{
#if MPEG_SIMD
    int16_t input[24] __attribute__((aligned(16)));
    uint8_t actual[16] __attribute__((aligned(16))), expected[16];
    uint32_t seed = 1;
    for (unsigned block = 0; block < 256; block++) {
        for (unsigned lane = 0; lane < 8; lane++) {
            for (unsigned plane = 0; plane < 3; plane++) {
                seed = seed * 1664525U + 1013904223U;
                /* Include extremes and integer-rounding boundaries. */
                unsigned value = block < 8 ? ((block >> plane) & 1U) * 255U : seed >> 24;
                input[plane * 8 + lane] = (int)value - (plane ? 128 : 16);
            }
            pixel(expected + lane * 2, input[lane] + 16, input[8 + lane] + 128,
                  input[16 + lane] + 128);
        }
        solar_os_mpeg_rgb565_s3(input, actual);
        if (memcmp(actual, expected, sizeof(actual)))
            return false;
    }
    return true;
#else
    return false;
#endif
}
void solar_os_mpeg_raster(const solar_os_mpeg_frame_t *f, uint8_t *out, uint32_t w, uint32_t h,
                          bool color, bool simd)
{
    if (!f || !out || !w || !h || !f->width || !f->height)
        return;
    unsigned bpp = color ? 2 : 1;
    uint32_t last_sy = UINT32_MAX;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t sy = (uint64_t)y * f->height / h;
        uint8_t *row = out + (size_t)y * w * bpp;
        if (sy == last_sy) {
            memcpy(row, row - (size_t)w * bpp, (size_t)w * bpp);
            continue;
        }
        last_sy = sy;
        const uint8_t *luma = f->y + (size_t)sy * f->y_stride;
        const uint8_t *cb = f->cb + (size_t)(sy / 2) * f->chroma_stride;
        const uint8_t *cr = f->cr + (size_t)(sy / 2) * f->chroma_stride;
        uint32_t sx = 0, remainder = 0, x = 0;
#if MPEG_SIMD
        if (color && simd) {
            /* The aligned workspace is 48 bytes of this worker's stack.
             * Gathering handles odd dimensions and arbitrary scale ratios;
             * no vector access reads beyond a decoded plane or raster. */
            int16_t lanes[24] __attribute__((aligned(16)));
            uint8_t packed[16] __attribute__((aligned(16)));
            for (; x + 8 <= w; x += 8) {
                for (unsigned i = 0; i < 8; i++) {
                    lanes[i] = (int)luma[sx] - 16;
                    lanes[8 + i] = (int)cb[sx / 2] - 128;
                    lanes[16 + i] = (int)cr[sx / 2] - 128;
                    remainder += f->width;
                    while (remainder >= w) {
                        remainder -= w;
                        sx++;
                    }
                }
                solar_os_mpeg_rgb565_s3(lanes, packed);
                memcpy(row + x * 2, packed, sizeof(packed));
            }
        }
#else
        (void)simd;
#endif
        for (; x < w; x++) {
            if (color)
                pixel(row + x * 2, luma[sx], cb[sx / 2], cr[sx / 2]);
            else
                row[x] = luma[sx];
            remainder += f->width;
            while (remainder >= w) {
                remainder -= w;
                sx++;
            }
        }
    }
}
