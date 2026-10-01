#include "jpeg_fast.h"

#include <stdbool.h>
#include <string.h>
#include "esp_heap_caps.h"

#ifdef ESP_PLATFORM
#include "esp_rom_caps.h"
#if ESP_ROM_HAS_JPEG_DECODE
#define SOLAR_OS_JPEG_FAST_ROM 1
#include "rom/tjpgd.h"
typedef unsigned int jpeg_input_size_t;
typedef unsigned int jpeg_output_result_t;
#endif
#elif defined(SOLAR_OS_JPEG_FAST_HOST)
#include "tjpgd.h"
typedef size_t jpeg_input_size_t;
typedef int jpeg_output_result_t;
#endif

#if defined(SOLAR_OS_JPEG_FAST_ROM) || defined(SOLAR_OS_JPEG_FAST_HOST)

#ifndef JD_FORMAT
#define JD_FORMAT 0 /* ROM decoder emits RGB888 MCU blocks. */
#endif
#if JD_FORMAT != 0
#error SolarOS JPEG adapter requires RGB888 MCU output
#endif

typedef struct {
    const uint8_t *data;
    size_t length, position;
    uint8_t *pixels;
    uint32_t width, height, decoded_width, decoded_height;
    solar_os_jpeg_format_t format;
} jpeg_io_t;

/* TJpgDec assumes YCbCr; RGB/CMYK, progressive and unusual component layouts
 * retain stb's color handling. Parse bounded segments before calling ROM. */
static bool baseline_ycbcr(const uint8_t *data, size_t length)
{
    if (length < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
    size_t pos = 2;
    bool baseline = false;
    while (pos + 4 <= length) {
        if (data[pos++] != 0xff) return false;
        while (pos < length && data[pos] == 0xff) pos++;
        if (pos + 3 > length) return false;
        uint8_t marker = data[pos++];
        size_t size = ((size_t)data[pos] << 8) | data[pos + 1];
        if (size < 2 || size > length - pos) return false;
        const uint8_t *segment = data + pos + 2;
        if (marker == 0xee && size >= 14 && !memcmp(segment, "Adobe", 5) &&
            segment[11] == 0) return false;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
            marker != 0xc8 && marker != 0xcc) {
            if (marker != 0xc0 || size < 11 || segment[0] != 8) return false;
            unsigned components = segment[5];
            if (components != 1 && components != 3) return false;
            if (size < 8U + components * 3U) return false;
            for (unsigned i = 0; i < components; i++)
                if (segment[6 + i * 3] != i + 1) return false;
            baseline = true;
        }
        if (marker == 0xda) return baseline;
        if (marker == 0xd9) return false;
        pos += size;
    }
    return false;
}

static jpeg_input_size_t jpeg_read(JDEC *decoder, uint8_t *buffer, jpeg_input_size_t requested)
{
    jpeg_io_t *io = decoder->device;
    size_t count = requested;
    if (count > io->length - io->position) count = io->length - io->position;
    if (buffer) memcpy(buffer, io->data + io->position, count);
    io->position += count; /* Skips obey the same bound as reads. */
    return (jpeg_input_size_t)count;
}

static uint32_t ceil_ratio(uint32_t value, uint32_t numerator, uint32_t denominator)
{
    return ((uint64_t)value * numerator + denominator - 1U) / denominator;
}

static jpeg_output_result_t jpeg_write(JDEC *decoder, void *bitmap, JRECT *rect)
{
    jpeg_io_t *io = decoder->device;
    if (!bitmap || rect->left > rect->right || rect->top > rect->bottom ||
        rect->right >= io->decoded_width || rect->bottom >= io->decoded_height) return 0;
    uint32_t x0 = ceil_ratio(rect->left, io->width, io->decoded_width);
    uint32_t x1 = ceil_ratio(rect->right + 1U, io->width, io->decoded_width);
    uint32_t y0 = ceil_ratio(rect->top, io->height, io->decoded_height);
    uint32_t y1 = ceil_ratio(rect->bottom + 1U, io->height, io->decoded_height);
    uint32_t stride = rect->right - rect->left + 1U;
    for (uint32_t y = y0; y < y1; y++) {
        uint32_t sy = (uint64_t)y * io->decoded_height / io->height;
        const uint8_t *row = (const uint8_t *)bitmap + (size_t)(sy - rect->top) * stride * 3U;
        uint8_t *dst = io->pixels + ((size_t)y * io->width + x0) * io->format;
        if (io->format == SOLAR_OS_JPEG_RGB888 && io->width == io->decoded_width) {
            memcpy(dst, row, (size_t)(x1 - x0) * 3U);
            continue;
        }
        uint32_t sx = (uint64_t)x0 * io->decoded_width / io->width;
        uint32_t remainder = (uint64_t)x0 * io->decoded_width % io->width;
        for (uint32_t x = x0; x < x1; x++) {
            const uint8_t *rgb = row + (sx - rect->left) * 3U;
            if (io->format == SOLAR_OS_JPEG_RGB888) {
                memcpy(dst, rgb, 3);
            } else if (io->format == SOLAR_OS_JPEG_RGB565) {
                uint16_t color = ((uint16_t)(rgb[0] & 0xf8U) << 8U) |
                    ((uint16_t)(rgb[1] & 0xfcU) << 3U) | (rgb[2] >> 3U);
                dst[0] = color >> 8U; dst[1] = color;
            } else {
                dst[0] = ((uint32_t)rgb[0] * 77U + (uint32_t)rgb[1] * 150U +
                    (uint32_t)rgb[2] * 29U) >> 8U;
            }
            dst += io->format;
            remainder += io->decoded_width;
            while (remainder >= io->width) { remainder -= io->width; sx++; }
        }
    }
    return 1;
}

esp_err_t solar_os_jpeg_fast_decode(const uint8_t *data, size_t length,
    uint32_t max_pixels, uint32_t max_width, uint32_t max_height,
    solar_os_jpeg_format_t format, uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    if (!data || !length || !pixels || !width || !height ||
        format < SOLAR_OS_JPEG_GRAY8 || format > SOLAR_OS_JPEG_RGB888)
        return ESP_ERR_INVALID_ARG;
    *pixels = NULL; *width = *height = 0;
    if (!baseline_ycbcr(data, length)) return ESP_ERR_NOT_SUPPORTED;
    /* Small per-call workspace; coefficients never require a full-size raster. */
    const size_t work_size = 4096;
    void *work = heap_caps_malloc(work_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) work = heap_caps_malloc(work_size, MALLOC_CAP_8BIT);
    if (!work) return ESP_ERR_NO_MEM;
    jpeg_io_t io = {.data = data, .length = length, .format = format};
    JDEC decoder;
    JRESULT result = jd_prepare(&decoder, jpeg_read, work, work_size, &io);
    esp_err_t error = ESP_ERR_NOT_SUPPORTED;
    if (result != JDR_OK) goto done;
    uint32_t sw = decoder.width, sh = decoder.height;
    if (!sw || !sh || (max_pixels && (uint64_t)sw * sh > max_pixels)) {
        error = ESP_ERR_INVALID_SIZE; goto done;
    }
    io.width = sw; io.height = sh;
    if (max_width && max_height && (sw > max_width || sh > max_height)) {
        if ((uint64_t)sh * max_width <= (uint64_t)sw * max_height) {
            io.width = max_width; io.height = (uint64_t)sh * max_width / sw;
        } else {
            io.height = max_height; io.width = (uint64_t)sw * max_height / sh;
        }
        if (!io.width) io.width = 1;
        if (!io.height) io.height = 1;
    }
    unsigned scale = 0;
    /* Native 1/2, 1/4, 1/8 decode only when it retains the requested detail. */
    while (scale < 3 && (sw >> (scale + 1)) >= io.width &&
        (sh >> (scale + 1)) >= io.height) scale++;
    io.decoded_width = sw >> scale; io.decoded_height = sh >> scale;
    uint64_t bytes = (uint64_t)io.width * io.height * format;
    if (bytes > SIZE_MAX) { error = ESP_ERR_INVALID_SIZE; goto done; }
    io.pixels = heap_caps_malloc((size_t)bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!io.pixels) io.pixels = heap_caps_malloc((size_t)bytes, MALLOC_CAP_8BIT);
    if (!io.pixels) { error = ESP_ERR_NO_MEM; goto done; }
    result = jd_decomp(&decoder, jpeg_write, scale);
    if (result != JDR_OK) {
        heap_caps_free(io.pixels); goto done;
    }
    *pixels = io.pixels; *width = io.width; *height = io.height;
    error = ESP_OK;
done:
    heap_caps_free(work);
    return error;
}
#else
esp_err_t solar_os_jpeg_fast_decode(const uint8_t *data, size_t length,
    uint32_t max_pixels, uint32_t max_width, uint32_t max_height,
    solar_os_jpeg_format_t format, uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    (void)data; (void)length; (void)max_pixels; (void)max_width; (void)max_height;
    (void)format; (void)pixels; (void)width; (void)height;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
