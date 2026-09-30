#include <assert.h>
#include <stdio.h>
#include "solar_os_rgb565.h"

static void colors(void)
{
    uint8_t rgb[] = {255,0,0, 0,255,0, 0,0,255, 255,255,255, 0,0,0, 127,127,127};
    const uint8_t expected[] = {0xf8,0, 7,0xe0, 0,0x1f, 255,255, 0,0, 0x7b,0xef};
    solar_os_rgb565_from_rgb888(rgb, 6);
    assert(!memcmp(rgb, expected, sizeof(expected)));
}

static void scale(void)
{
    const uint8_t row[] = {0xf8,0, 7,0xe0, 0,0x1f};
    uint8_t out[16]; memset(out, 0xaa, sizeof(out));
    solar_os_rgb565_scale_row(row, 3, out + 2, 6);
    const uint8_t enlarged[] = {0xf8,0,0xf8,0, 7,0xe0,7,0xe0, 0,0x1f,0,0x1f};
    assert(!memcmp(out + 2, enlarged, sizeof(enlarged)));
    assert(out[0] == 0xaa && out[1] == 0xaa && out[14] == 0xaa && out[15] == 0xaa);
    solar_os_rgb565_scale_row(row, 3, out, 2);
    assert(!memcmp(out, row, 4));
    solar_os_rgb565_scale_row(row, 3, out, 3);
    assert(!memcmp(out, row, sizeof(row)));
    solar_os_rgb565_scale_row(row, 1, out, 7);
    for (unsigned i = 0; i < 7; i++) assert(out[i * 2] == 0xf8 && out[i * 2 + 1] == 0);
    uint8_t source[66], actual[96];
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 71U);
    for (unsigned sw = 1; sw <= 33; sw++) for (unsigned dw = 1; dw <= 47; dw++) {
        memset(actual, 0xaa, sizeof(actual));
        solar_os_rgb565_scale_row(source, sw, actual + 1, dw);
        for (unsigned x = 0; x < dw; x++) {
            unsigned sx = x * sw / dw;
            assert(actual[1 + x * 2] == source[sx * 2]);
            assert(actual[2 + x * 2] == source[sx * 2 + 1]);
        }
        assert(actual[0] == 0xaa && actual[1 + dw * 2] == 0xaa);
    }
}

static void rotation(void)
{
    /* Non-square source, padding and guard bytes catch stride/orientation errors. */
    const uint8_t src[] = {0x10,1, 0x20,2, 0x30,3, 0xaa,0xaa,
                           0x40,4, 0x50,5, 0x60,6, 0xaa,0xaa};
    const uint8_t order[4][6] = {{1,2,3,4,5,6},{4,1,5,2,6,3},{6,5,4,3,2,1},{3,6,2,5,1,4}};
    for (unsigned r = 0; r < 4; r++) {
        uint8_t out[32]; memset(out, 0xcc, sizeof(out));
        const unsigned w = r % 2 ? 2 : 3, h = r % 2 ? 3 : 2, stride = w * 2 + 2;
        solar_os_rgb565_rotate(src, 3, 2, 8, out + 2, stride, (solar_os_display_rotation_t)r);
        for (unsigned y = 0; y < h; y++) {
            for (unsigned x = 0; x < w; x++) {
                const uint8_t value = order[r][y * w + x];
                assert(out[2 + y * stride + x * 2] == value * 0x10);
                assert(out[3 + y * stride + x * 2] == value);
            }
            assert(out[2 + y * stride + w * 2] == 0xcc);
        }
        assert(out[0] == 0xcc && out[1] == 0xcc && out[2 + h * stride] == 0xcc);
    }
}

int main(void)
{
    colors(); scale(); rotation();
    puts("rgb565_raster_test: OK");
    return 0;
}
