#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_stb_image.h"
#include "jpeg_fast.h"
#include "stb_image.h"

static uint8_t *load(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long n = ftell(file); assert(n > 0);
    rewind(file); uint8_t *data = malloc(n); assert(data);
    assert(fread(data, 1, n, file) == (size_t)n); fclose(file);
    *length = n; return data;
}

static void test_baseline(const char *path)
{
    size_t length; uint8_t *data = load(path, &length);
    uint8_t *rgb, *gray, *packed; uint32_t w, h, gw, gh, pw, ph;
    assert(solar_os_jpeg_fast_decode(data, length, 1000000, 0, 0,
        SOLAR_OS_JPEG_RGB888, &rgb, &w, &h) == ESP_OK);
    assert(solar_os_stb_decode_gray(data, length, 1000000, &gray, &gw, &gh) == ESP_OK);
    assert(solar_os_stb_decode_jpeg_rgb565_scaled(data, length, 1000000,
        w, h, &packed, &pw, &ph) == ESP_OK);
    assert(w == gw && h == gh && w == pw && h == ph);
    int rw, rh, channels;
    uint8_t *reference = stbi_load_from_memory(data, length, &rw, &rh, &channels, 3);
    assert(reference && rw == (int)w && rh == (int)h);
    unsigned difference = 0;
    for (size_t i = 0; i < (size_t)w * h * 3; i++)
        difference += abs((int)rgb[i] - reference[i]);
    assert(difference < (size_t)w * h * 3 * 8); /* Different chroma interpolation. */
    stbi_image_free(reference);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint16_t color = ((uint16_t)(rgb[i*3] & 0xf8) << 8) |
            ((uint16_t)(rgb[i*3+1] & 0xfc) << 3) | (rgb[i*3+2] >> 3);
        assert(packed[i*2] == (uint8_t)(color >> 8) && packed[i*2+1] == (uint8_t)color);
        assert(gray[i] == (uint8_t)(((unsigned)rgb[i*3]*77 +
            (unsigned)rgb[i*3+1]*150 + (unsigned)rgb[i*3+2]*29) >> 8));
    }
    solar_os_stb_image_free(gray); solar_os_stb_image_free(packed);
    /* Non-integral scaling covers MCU seams and partial edge blocks. */
    assert(solar_os_stb_decode_jpeg_rgb_scaled(data, length, 1000000,
        w - 3, h - 5, &gray, &gw, &gh) == ESP_OK);
    for (uint32_t y = 0; y < gh; y++) for (uint32_t x = 0; x < gw; x++) {
        size_t source = ((size_t)((uint64_t)y*h/gh)*w + (uint64_t)x*w/gw)*3;
        assert(!memcmp(gray + ((size_t)y*gw + x)*3, rgb + source, 3));
    }
    solar_os_stb_image_free(gray); solar_os_stb_image_free(rgb);
    for (unsigned scale = 1; scale <= 3; scale++) {
        assert(solar_os_jpeg_fast_decode(data, length, 1000000,
            w >> scale, h >> scale, SOLAR_OS_JPEG_RGB888, &rgb, &gw, &gh) == ESP_OK);
        assert(gw <= (w >> scale) && gh <= (h >> scale));
        solar_os_stb_image_free(rgb);
    }
    assert(solar_os_jpeg_fast_decode(data, length, 1, 0, 0,
        SOLAR_OS_JPEG_RGB888, &rgb, &gw, &gh) == ESP_ERR_INVALID_SIZE && !rgb);
    /* Every truncation and a corrupt oversized segment must stay in bounds. */
    for (size_t n = 1; n < length; n++) {
        esp_err_t error = solar_os_jpeg_fast_decode(data, n, 1000000,
            16, 16, SOLAR_OS_JPEG_RGB888, &rgb, &gw, &gh);
        if (error == ESP_OK) solar_os_stb_image_free(rgb);
        else assert(!rgb);
    }
    data[4] = 0xff; data[5] = 0xff;
    assert(solar_os_jpeg_fast_decode(data, length, 1000000, 0, 0,
        SOLAR_OS_JPEG_RGB888, &rgb, &gw, &gh) == ESP_ERR_NOT_SUPPORTED);
    free(data);
}

static void test_fallback(const char *path)
{
    size_t length; uint8_t *data = load(path, &length), *pixels;
    uint32_t w, h;
    assert(solar_os_jpeg_fast_decode(data, length, 1000000, 0, 0,
        SOLAR_OS_JPEG_RGB888, &pixels, &w, &h) == ESP_ERR_NOT_SUPPORTED && !pixels);
    assert(solar_os_stb_decode_rgb(data, length, 1000000, &pixels, &w, &h) == ESP_OK);
    assert(w && h); solar_os_stb_image_free(pixels);
    assert(solar_os_stb_decode_jpeg_rgb565_scaled(data, length, 1000000,
        48, 32, &pixels, &w, &h) == ESP_OK);
    assert(w <= 48 && h <= 32); solar_os_stb_image_free(pixels); free(data);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    test_baseline(argv[1]); test_baseline(argv[2]);
    test_fallback(argv[3]); test_fallback(argv[4]);
    puts("jpeg_fast_test: OK");
    return 0;
}
