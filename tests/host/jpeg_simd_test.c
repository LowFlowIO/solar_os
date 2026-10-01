#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jpeg_fast.h"
#include "esp_jpeg_dec.h"
#include "jpeg_simd_stubs/esp_heap_caps.h"

int jpeg_test_allocations, jpeg_test_fail_alloc;
int jpeg_test_fail_psram;
size_t jpeg_test_internal_free = 128 * 1024;
static size_t after_header_free;
static unsigned opens, closes, source_w = 64, source_h = 48, current_row;
static int failure;
static jpeg_dec_config_t configuration;

static void sample(uint32_t x, uint32_t y, uint8_t *rgb)
{
    rgb[0] = x * 3; rgb[1] = y * 5; rgb[2] = x + y;
}

jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *config, jpeg_dec_handle_t *handle)
{
    if (failure == 1) return JPEG_ERR_NO_MEM;
    configuration = *config;
    assert(config->block_enable && !config->scale.width && !config->scale.height);
    *handle = &configuration; current_row = 0; opens++;
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t handle)
{
    assert(handle == &configuration); closes++; return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t handle, jpeg_dec_io_t *io,
    jpeg_dec_header_info_t *header)
{
    (void)handle; assert(io->inbuf && io->inbuf_len);
    if (failure == 2) return JPEG_ERR_BAD_DATA;
    header->width = source_w; header->height = source_h;
    if (after_header_free) jpeg_test_internal_free = after_header_free;
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t handle, int *length)
{
    (void)handle;
    if (failure == 3) return JPEG_ERR_FAIL;
    *length = source_w * 16 * (configuration.output_type == JPEG_PIXEL_FORMAT_RGB565_BE ? 2 : 3);
    if (failure == 4) *length *= 100;
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_get_process_count(jpeg_dec_handle_t handle, int *count)
{
    (void)handle; *count = (source_h + 15) / 16;
    if (failure == 5) *count = 0;
    if (failure == 6) (*count)--;
    if (failure == 7) (*count)++;
    return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t handle, jpeg_dec_io_t *io)
{
    (void)handle;
    if (failure == 8) return JPEG_ERR_BAD_DATA;
    assert(!((uintptr_t)io->outbuf & 15));
    unsigned rows = source_h - current_row;
    if (rows > 16) rows = 16;
    unsigned bpp = configuration.output_type == JPEG_PIXEL_FORMAT_RGB565_BE ? 2 : 3;
    for (unsigned y = 0; y < rows; y++) for (unsigned x = 0; x < source_w; x++) {
        uint8_t rgb[3]; sample(x, current_row + y, rgb);
        uint8_t *dst = io->outbuf + (y * source_w + x) * bpp;
        if (bpp == 3) memcpy(dst, rgb, 3);
        else {
            uint16_t color = ((uint16_t)(rgb[0] & 0xf8) << 8) |
                ((uint16_t)(rgb[1] & 0xfc) << 3) | (rgb[2] >> 3);
            dst[0] = color >> 8; dst[1] = color;
        }
    }
    io->out_size = rows * source_w * bpp;
    if (failure == 9) io->out_size--;
    if (failure == 10) io->out_size = source_w * 16 * bpp + 1;
    current_row += rows;
    return JPEG_ERR_OK;
}

static esp_err_t decode(unsigned max_w, unsigned max_h, solar_os_jpeg_format_t format,
    uint8_t **pixels, uint32_t *w, uint32_t *h)
{
    static const uint8_t input[] = {1};
    return solar_os_jpeg_simd_decode(input, sizeof(input), 1000000,
        max_w, max_h, format, pixels, w, h);
}

static void verify(unsigned max_w, unsigned max_h, solar_os_jpeg_format_t format)
{
    uint8_t *pixels; uint32_t w, h;
    assert(decode(max_w, max_h, format, &pixels, &w, &h) == ESP_OK);
    assert(w && h && w <= source_w && h <= source_h && !((uintptr_t)pixels & 15));
    if (max_w && max_h) assert(w <= max_w && h <= max_h);
    assert(jpeg_test_allocations == 1 && opens == closes);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint8_t rgb[3]; sample((uint64_t)x * source_w / w, (uint64_t)y * source_h / h, rgb);
        uint8_t *dst = pixels + ((size_t)y * w + x) * format;
        if (format == SOLAR_OS_JPEG_RGB888) assert(!memcmp(dst, rgb, 3));
        else if (format == SOLAR_OS_JPEG_GRAY8)
            assert(*dst == (uint8_t)(((uint32_t)rgb[0]*77 + (uint32_t)rgb[1]*150 + rgb[2]*29) >> 8));
        else {
            uint16_t color = ((uint16_t)(rgb[0] & 0xf8) << 8) |
                ((uint16_t)(rgb[1] & 0xfc) << 3) | (rgb[2] >> 3);
            assert(dst[0] == (uint8_t)(color >> 8) && dst[1] == (uint8_t)color);
        }
    }
    heap_caps_free(pixels); assert(!jpeg_test_allocations);
}

int main(void)
{
    for (unsigned format = 1; format <= 3; format++) {
        verify(0, 0, format); verify(64, 48, format); verify(100, 100, format);
        verify(41, 31, format); verify(16, 12, format); verify(1, 1, format);
        source_h = 40; verify(17, 17, format); source_h = 48;
    }
    uint8_t *pixels; uint32_t w, h;
    for (failure = 1; failure <= 10; failure++) {
        assert(decode(32, 24, SOLAR_OS_JPEG_RGB565, &pixels, &w, &h) == ESP_ERR_NOT_SUPPORTED);
        assert(!pixels && !w && !h && !jpeg_test_allocations && opens == closes);
    }
    failure = 0; source_w = 63;
    assert(decode(32, 24, SOLAR_OS_JPEG_GRAY8, &pixels, &w, &h) == ESP_ERR_NOT_SUPPORTED);
    source_w = 64; jpeg_test_fail_alloc = 1;
    assert(decode(32, 24, SOLAR_OS_JPEG_GRAY8, &pixels, &w, &h) == ESP_ERR_NO_MEM);
    jpeg_test_fail_alloc = 0;
    jpeg_test_fail_psram = 1;
    verify(32, 24, SOLAR_OS_JPEG_RGB565); /* Budgeted internal fallback. */
    after_header_free = 32 * 1024;
    assert(decode(32, 24, SOLAR_OS_JPEG_GRAY8, &pixels, &w, &h) == ESP_ERR_NO_MEM);
    assert(!pixels && !w && !h && !jpeg_test_allocations && opens == closes);
    after_header_free = 0; jpeg_test_fail_psram = 0;
    jpeg_test_internal_free = 128 * 1024;
    const uint8_t input[] = {1};
    assert(solar_os_jpeg_simd_decode(input, 1, 1, 0, 0, SOLAR_OS_JPEG_RGB888,
        &pixels, &w, &h) == ESP_ERR_INVALID_SIZE);
    assert(!pixels && !jpeg_test_allocations && opens == closes);
    jpeg_test_internal_free = 32 * 1024;
    unsigned before = opens;
    assert(decode(32, 24, SOLAR_OS_JPEG_RGB565, &pixels, &w, &h) == ESP_ERR_NOT_SUPPORTED);
    assert(!pixels && !w && !h && opens == before && !jpeg_test_allocations);
    puts("jpeg_simd_test: OK");
}
