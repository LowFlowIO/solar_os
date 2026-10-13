#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_indexed_raster.h"

static void reference_set(uint8_t *row, unsigned x, unsigned value, bool index8)
{
    if (index8) row[x] = (uint8_t)value;
    else {
        unsigned shift = (x % 4U) * 2U;
        row[x / 4U] = (uint8_t)((row[x / 4U] & ~(3U << shift)) | ((value & 3U) << shift));
    }
}

int main(void)
{
    uint16_t palette[256], colors[256];
    for (unsigned i = 0; i < 256U; i++) palette[i] = (uint16_t)(i * 257U ^ 0xf81fU);
    unsigned cases = 0;
    for (unsigned index8 = 0; index8 <= 1U; index8++) {
        solar_os_display_format_t format = index8 ? SOLAR_OS_DISPLAY_FORMAT_INDEX8 : SOLAR_OS_DISPLAY_FORMAT_INDEX2;
        for (unsigned invert = 0; invert <= 1U; invert++) {
            solar_os_indexed_palette(palette, format, invert, colors);
            for (unsigned i = 0; i < (index8 ? 256U : 4U); i++) {
                uint16_t expected = index8 ? (invert ? (uint16_t)~palette[i] : palette[i]) :
                    palette[invert ? 3U - i : i];
                assert(colors[i] == expected);
            }
            for (unsigned width = 1; width <= 13U; width++)
            for (unsigned height = 1; height <= 9U; height++)
            for (unsigned rotation = 0; rotation < 4U; rotation++) {
                unsigned dw = rotation % 2U ? height : width;
                unsigned dh = rotation % 2U ? width : height;
                unsigned stride = (index8 ? width : (width + 3U) / 4U) + 3U;
                unsigned output_stride = (index8 ? dw : (dw + 3U) / 4U) + 2U;
                uint8_t source[512], actual[512], expected[512], indices[117];
                memset(source, 0x5a, sizeof(source));
                memset(actual, 0xa5, sizeof(actual));
                memset(expected, 0xa5, sizeof(expected));
                for (unsigned y = 0; y < height; y++)
                    for (unsigned x = 0; x < width; x++) {
                        unsigned value = (x * 37U + y * 71U + 255U) & (index8 ? 255U : 3U);
                        indices[y * width + x] = (uint8_t)value;
                        reference_set(source + 3U + y * stride, x, value, index8);
                    }
                solar_os_indexed_rotate(source + 3U, width, height, stride,
                    actual + 5U, output_stride, format, (solar_os_display_rotation_t)rotation);
                for (unsigned y = 0; y < dh; y++) {
                    uint8_t expected_indices[13];
                    for (unsigned x = 0; x < dw; x++) {
                        unsigned sx, sy;
                        switch (rotation) {
                        case 1: sx = y; sy = height - 1U - x; break;
                        case 2: sx = width - 1U - x; sy = height - 1U - y; break;
                        case 3: sx = width - 1U - y; sy = x; break;
                        default: sx = x; sy = y; break;
                        }
                        expected_indices[x] = indices[sy * width + sx];
                        reference_set(expected + 5U + y * output_stride, x, expected_indices[x], index8);
                    }
                    for (unsigned output_width = 1; output_width <= 31U; output_width++) {
                        uint8_t line[68]; memset(line, 0xcc, sizeof(line));
                        solar_os_indexed_scale_row(actual + 5U + y * output_stride,
                            dw, format, colors, line + 3U, output_width);
                        for (unsigned x = 0; x < output_width; x++) {
                            uint16_t color = colors[expected_indices[x * dw / output_width]];
                            assert(line[3U + x * 2U] == (uint8_t)(color >> 8U));
                            assert(line[4U + x * 2U] == (uint8_t)color);
                        }
                        assert(line[2] == 0xcc && line[3U + output_width * 2U] == 0xcc);
                        cases++;
                    }
                }
                assert(!memcmp(actual, expected, sizeof(actual)));
            }
        }
    }
    printf("indexed raster: %u rotation/palette/scaling cases passed\n", cases);
    return 0;
}
