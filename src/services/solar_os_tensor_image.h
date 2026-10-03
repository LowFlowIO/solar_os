#pragma once
#include "solar_os_inference.h"
#include "solar_os_raster_image.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef enum { SOLAR_OS_TENSOR_NHWC, SOLAR_OS_TENSOR_NCHW,
    SOLAR_OS_TENSOR_HWC, SOLAR_OS_TENSOR_CHW } solar_os_tensor_image_layout_t;
typedef enum { SOLAR_OS_TENSOR_RGB, SOLAR_OS_TENSOR_BGR,
    SOLAR_OS_TENSOR_GRAY } solar_os_tensor_image_color_t;
typedef enum { SOLAR_OS_TENSOR_STRETCH, SOLAR_OS_TENSOR_LETTERBOX } solar_os_tensor_image_resize_t;
typedef struct {
    solar_os_tensor_image_layout_t layout;
    solar_os_tensor_image_color_t color;
    solar_os_tensor_image_resize_t resize;
    uint32_t x, y, width, height;
    double mean[3], std[3];
    uint8_t pad[3]; /* Values in the selected output color order, before normalization. */
} solar_os_tensor_image_options_t;
typedef struct {
    uint32_t source_width, source_height, input_width, input_height;
    uint32_t crop_x, crop_y, crop_width, crop_height;
    uint32_t resized_width, resized_height, pad_left, pad_top;
    uint32_t channels;
    size_t bytes;
    uint64_t preprocess_us;
} solar_os_tensor_image_transform_t;

void solar_os_tensor_image_defaults(solar_os_tensor_image_options_t *options);
esp_err_t solar_os_tensor_image_option(solar_os_tensor_image_options_t *options,
    const char *key, const char *value);
/* Validate source/port/options and obtain exact allocation and inverse-map geometry.
 * Batch must be one; int8/uint8/int16/float32, nearest-neighbor stretch/letterbox.
 * The explicitly selected layout defines the axis for per-channel exponents. */
esp_err_t solar_os_tensor_image_inspect(const solar_os_raster_image_pixels_t *source,
    const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
    solar_os_tensor_image_transform_t *transform);
/* Caller owns the destination. Source is immutable and must not overlap it.
 * Lookup storage uses PSRAM only, is temporary, and is freed on every return.
 * Cancellation may leave partial destination bytes; publish only on ESP_OK. */
esp_err_t solar_os_tensor_image_prepare(const solar_os_raster_image_pixels_t *source,
    const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
    uint8_t *destination, size_t length, solar_os_tensor_image_transform_t *transform,
    solar_os_inference_cancel_fn cancel, void *user);
#ifdef __cplusplus
}
#endif
