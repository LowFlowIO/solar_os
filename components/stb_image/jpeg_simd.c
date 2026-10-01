#include "jpeg_fast.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if (defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)) || \
    defined(SOLAR_OS_JPEG_SIMD_HOST)
#include <limits.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "solar_os_resource_limits.h"

static uint8_t *image_alloc(size_t size)
{
    uint8_t *pixels = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels && size <= SIZE_MAX - SOLAR_OS_INTERNAL_RESERVE_BYTES &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >=
            SOLAR_OS_INTERNAL_RESERVE_BYTES + size)
        pixels = heap_caps_aligned_alloc(16, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return pixels;
}

/* Transfer a decoded strip into the exact aspect-fit raster. No full-size
 * intermediate and no upscaling; the decoder emits RGB565 directly for color. */
static void copy_strip(uint8_t *output, uint32_t w, uint32_t h,
    const uint8_t *strip, uint32_t sw, uint32_t sh, uint32_t first_row,
    uint32_t rows, solar_os_jpeg_format_t format)
{
    uint32_t source_bpp = format == SOLAR_OS_JPEG_RGB565 ? 2 : 3;
    uint32_t y0 = ((uint64_t)first_row * h + sh - 1) / sh;
    uint32_t y1 = ((uint64_t)(first_row + rows) * h + sh - 1) / sh;
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t sy = (uint64_t)y * sh / h;
        const uint8_t *src = strip + (size_t)(sy - first_row) * sw * source_bpp;
        uint8_t *dst = output + (size_t)y * w * format;
        if (w == sw && format != SOLAR_OS_JPEG_GRAY8) {
            memcpy(dst, src, (size_t)w * format);
            continue;
        }
        uint32_t sx = 0, remainder = 0;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *pixel = src + (size_t)sx * source_bpp;
            if (format == SOLAR_OS_JPEG_GRAY8) {
                *dst = ((uint32_t)pixel[0] * 77 + (uint32_t)pixel[1] * 150 +
                    (uint32_t)pixel[2] * 29) >> 8;
            } else {
                memcpy(dst, pixel, source_bpp);
            }
            dst += format;
            remainder += sw;
            while (remainder >= w) { remainder -= w; sx++; }
        }
    }
}

esp_err_t solar_os_jpeg_simd_decode(const uint8_t *data, size_t length,
    uint32_t max_pixels, uint32_t max_width, uint32_t max_height,
    solar_os_jpeg_format_t format, uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    if (!data || !length || length > INT_MAX || !pixels || !width || !height ||
        format < SOLAR_OS_JPEG_GRAY8 || format > SOLAR_OS_JPEG_RGB888)
        return ESP_ERR_INVALID_ARG;
    *pixels = NULL; *width = *height = 0;
    /* Private vendor allocations prefer internal SRAM. Leave the OS reserve
     * plus a conservative workspace allowance, otherwise use the ROM path. */
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
        SOLAR_OS_INTERNAL_RESERVE_BYTES + 16U * 1024U)
        return ESP_ERR_NOT_SUPPORTED;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = format == SOLAR_OS_JPEG_RGB565 ?
        JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB888;
    config.block_enable = true;
    jpeg_dec_handle_t decoder = NULL;
    jpeg_error_t status = jpeg_dec_open(&config, &decoder);
    if (status != JPEG_ERR_OK) return ESP_ERR_NOT_SUPPORTED;
    uint8_t *strip = NULL, *output = NULL;
    esp_err_t error = ESP_ERR_NOT_SUPPORTED;
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)data, .inbuf_len = (int)length};
    jpeg_dec_header_info_t header = {0};
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK) goto done;
    uint32_t sw = header.width, sh = header.height;
    if (!sw || !sh || (max_pixels && (uint64_t)sw * sh > max_pixels)) {
        error = ESP_ERR_INVALID_SIZE; goto done;
    }
    /* Vendor block mode requires dimensions that are multiples of eight.
     * Odd-size images retain the ROM decoder instead of adding padding. */
    if ((sw & 7) || (sh & 7)) goto done;
    uint32_t w = sw, h = sh;
    if (max_width && max_height && (sw > max_width || sh > max_height)) {
        if ((uint64_t)sh * max_width <= (uint64_t)sw * max_height) {
            w = max_width; h = (uint64_t)sh * w / sw;
        } else {
            h = max_height; w = (uint64_t)sw * h / sh;
        }
        if (!w) w = 1;
        if (!h) h = 1;
    }
    int block_size = 0, block_count = 0;
    uint32_t source_bpp = format == SOLAR_OS_JPEG_RGB565 ? 2 : 3;
    if (jpeg_dec_get_outbuf_len(decoder, &block_size) != JPEG_ERR_OK ||
        jpeg_dec_get_process_count(decoder, &block_count) != JPEG_ERR_OK ||
        block_size <= 0 || (uint32_t)block_size > sw * 16U * source_bpp ||
        block_count <= 0 || (uint32_t)block_count > sh) goto done;
    uint64_t bytes = (uint64_t)w * h * format;
    if (bytes > SIZE_MAX) { error = ESP_ERR_INVALID_SIZE; goto done; }
    strip = image_alloc(block_size);
    output = image_alloc((size_t)bytes);
    if (!strip || !output) { error = ESP_ERR_NO_MEM; goto done; }
    io.outbuf = strip;
    uint32_t row = 0;
    for (int block = 0; block < block_count; block++) {
        io.out_size = 0;
        if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK || io.out_size <= 0 ||
            io.out_size > block_size || io.out_size % (sw * source_bpp)) goto done;
        uint32_t rows = io.out_size / (sw * source_bpp);
        if (row >= sh || rows > sh - row) goto done;
        copy_strip(output, w, h, strip, sw, sh, row, rows, format);
        row += rows;
    }
    if (row != sh) goto done;
    *pixels = output; *width = w; *height = h;
    output = NULL;
    error = ESP_OK;
done:
    heap_caps_free(output);
    heap_caps_free(strip);
    jpeg_dec_close(decoder); /* No context or workspace retained while idle. */
    return error;
}
#else
esp_err_t solar_os_jpeg_simd_decode(const uint8_t *data, size_t length,
    uint32_t max_pixels, uint32_t max_width, uint32_t max_height,
    solar_os_jpeg_format_t format, uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    (void)data; (void)length; (void)max_pixels; (void)max_width; (void)max_height;
    (void)format; (void)pixels; (void)width; (void)height;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
