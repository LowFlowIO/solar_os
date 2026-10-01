#include "solar_os_raster_image.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_gfx.h"
#include "solar_os_memory.h"
#include "solar_os_rgb565.h"
#include "solar_os_stb_image.h"
#include "solar_os_webp_decoder.h"

#define RASTER_IMAGE_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define RASTER_IMAGE_MAX_PIXELS (2U * 1024U * 1024U)

typedef enum {
    RASTER_IMAGE_PIXELS_STB,
    RASTER_IMAGE_PIXELS_WEBP,
} raster_image_pixels_owner_t;

struct solar_os_raster_image {
    uint32_t references;
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
    uint8_t *rgb565;
    raster_image_pixels_owner_t pixels_owner;
};

static bool raster_image_is_webp(const uint8_t *data, size_t len)
{
    return data != NULL && len >= 12U &&
        memcmp(data, "RIFF", 4U) == 0 &&
        memcmp(data + 8U, "WEBP", 4U) == 0;
}

static esp_err_t raster_image_read_file(const char *path,
                                        uint8_t **out_data,
                                        size_t *out_len)
{
    if (path == NULL || path[0] == '\0' || out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || (uint64_t)st.st_size > RASTER_IMAGE_MAX_FILE_BYTES ||
        (uint64_t)st.st_size > SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const size_t len = (size_t)st.st_size;
    uint8_t *data = solar_os_memory_alloc(len,
                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                          "raster.file");
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const bool read_ok = fread(data, 1U, len, file) == len;
    fclose(file);
    if (!read_ok) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_len = len;
    return ESP_OK;
}

esp_err_t solar_os_raster_image_open(const char *path,
                                     solar_os_raster_image_t **out_image)
{
    if (out_image == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_image = NULL;

    uint8_t *data = NULL;
    size_t data_len = 0;
    esp_err_t err = raster_image_read_file(path, &data, &data_len);
    if (err != ESP_OK) {
        return err;
    }

    err = solar_os_raster_image_decode(data, data_len, out_image);
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_raster_image_decode(const uint8_t *data, size_t data_len,
                                       solar_os_raster_image_t **out_image)
{
    if (out_image == NULL) return ESP_ERR_INVALID_ARG;
    *out_image = NULL;
    if (data == NULL || data_len == 0U || data_len > RASTER_IMAGE_MAX_FILE_BYTES)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err;
    solar_os_raster_image_t *image = solar_os_memory_calloc(
        1U,
        sizeof(*image),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "raster.image");
    if (image == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (raster_image_is_webp(data, data_len)) {
        err = solar_os_webp_decode_rgb(data,
                                       data_len,
                                       RASTER_IMAGE_MAX_PIXELS,
                                       &image->pixels,
                                       &image->width,
                                       &image->height);
        image->pixels_owner = RASTER_IMAGE_PIXELS_WEBP;
    } else {
        err = solar_os_stb_decode_rgb(data,
                                      data_len,
                                      RASTER_IMAGE_MAX_PIXELS,
                                      &image->pixels,
                                      &image->width,
                                      &image->height);
        image->pixels_owner = RASTER_IMAGE_PIXELS_STB;
    }
    if (err != ESP_OK) {
        solar_os_memory_free(image);
        return err;
    }

    image->references = 1U;
    *out_image = image;
    return ESP_OK;
}

void solar_os_raster_image_retain(solar_os_raster_image_t *image)
{
    if (image != NULL) {
        (void)__atomic_add_fetch(&image->references, 1U, __ATOMIC_RELAXED);
    }
}

void solar_os_raster_image_release(solar_os_raster_image_t *image)
{
    if (image == NULL ||
        __atomic_sub_fetch(&image->references, 1U, __ATOMIC_ACQ_REL) != 0U) {
        return;
    }

    if (image->pixels_owner == RASTER_IMAGE_PIXELS_WEBP) {
        solar_os_webp_free(image->pixels);
    } else {
        solar_os_stb_image_free(image->pixels);
    }
    image->pixels = NULL;
    solar_os_memory_free(image->rgb565);
    solar_os_memory_free(image);
}

uint32_t solar_os_raster_image_width(const solar_os_raster_image_t *image)
{
    return image != NULL ? image->width : 0U;
}

uint32_t solar_os_raster_image_height(const solar_os_raster_image_t *image)
{
    return image != NULL ? image->height : 0U;
}

esp_err_t solar_os_raster_image_draw(const solar_os_raster_image_t *image,
                                     solar_os_gfx_t *gfx,
                                     int x,
                                     int y,
                                     uint32_t width,
                                     uint32_t height)
{
    if (image == NULL || gfx == NULL || image->pixels == NULL ||
        image->width == 0U || image->height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width == 0U) {
        width = image->width;
    }
    if (height == 0U) {
        height = image->height;
    }
    if (width > INT_MAX || height > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const solar_os_gfx_raster_t raster = {
        .pixels = image->pixels,
        .pixels_size = (size_t)image->width * image->height * 3U,
        .width = image->width,
        .height = image->height,
        .stride = (size_t)image->width * 3U,
        .format = SOLAR_OS_GFX_RASTER_RGB888,
    };
    return solar_os_gfx_blit_raster(gfx,
                                    &raster,
                                    x,
                                    y,
                                    (int)width,
                                    (int)height,
                                    NULL);
}

esp_err_t solar_os_raster_image_present(solar_os_raster_image_t *image,
    solar_os_gfx_t *gfx, int x, int y, uint32_t width, uint32_t height)
{
    if (!image || !gfx || !image->pixels || x < 0 || y < 0) return ESP_ERR_INVALID_ARG;
    if (!width) width = image->width;
    if (!height) height = image->height;
    if (!width || !height || (uint64_t)x + width > (uint32_t)solar_os_gfx_width(gfx) ||
        (uint64_t)y + height > (uint32_t)solar_os_gfx_height(gfx)) return ESP_ERR_INVALID_SIZE;
    if (!solar_os_gfx_supports_frame_format(gfx, SOLAR_OS_DISPLAY_FORMAT_RGB565)) {
        esp_err_t err = solar_os_raster_image_draw(image, gfx, x, y, width, height);
        if (err == ESP_OK) solar_os_gfx_present(gfx);
        return err;
    }
    if (image->width > UINT16_MAX / 2U || image->height > UINT16_MAX ||
        width > UINT16_MAX || height > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    const size_t count = (size_t)image->width * image->height;
    if (!image->rgb565) {
        image->rgb565 = solar_os_memory_alloc(count * 2U,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "raster.rgb565");
        if (!image->rgb565) return ESP_ERR_NO_MEM;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *rgb = image->pixels + i * 3U;
            const uint16_t color = ((uint16_t)(rgb[0] & 0xf8U) << 8U) |
                ((uint16_t)(rgb[1] & 0xfcU) << 3U) | (rgb[2] >> 3U);
            image->rgb565[i * 2U] = color >> 8U;
            image->rgb565[i * 2U + 1U] = color;
        }
    }
    const solar_os_display_raster_t raster = {
        .data = image->rgb565, .data_size = count * 2U,
        .source_width = image->width, .source_height = image->height,
        .source_stride = image->width * 2U, .x = x, .y = y,
        .width = width, .height = height, .format = SOLAR_OS_DISPLAY_FORMAT_RGB565,
    };
    return solar_os_gfx_present_frame(gfx, &raster);
}
