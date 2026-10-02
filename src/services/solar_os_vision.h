#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_raster_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_VISION_QR_MAX 8U
#define SOLAR_OS_VISION_MAX_WIDTH 640U
#define SOLAR_OS_VISION_MAX_HEIGHT 480U
#define SOLAR_OS_VISION_TIMEOUT_MS 5000U

typedef bool (*solar_os_vision_cancel_fn)(void *user);
typedef struct {
    int32_t x, y;
} solar_os_vision_point_t;
typedef struct {
    solar_os_vision_point_t corners[4]; /* Coordinates in the original image. */
    uint8_t *payload; /* Owned bytes; not necessarily UTF-8 or NUL terminated. */
    size_t length;
    uint32_t eci;
    int version, ecc_level, data_type;
} solar_os_vision_qr_code_t;
typedef struct {
    size_t count;
    uint32_t width, height, processed_width, processed_height;
    uint32_t candidates, decode_failures;
    bool truncated;
    uint64_t preprocess_us, detect_us, decode_us, elapsed_us;
    solar_os_vision_qr_code_t codes[SOLAR_OS_VISION_QR_MAX];
} solar_os_vision_qr_results_t;

/* One admitted native worker at a time; a competing request returns busy
 * (ESP_ERR_INVALID_STATE). No camera ownership or interpreter calls occur.
 * The caller waits cooperatively and may cancel; cancelled/timed-out work is
 * reaped before returning. The worker holds an image reference throughout.
 * Options select a crop/resize; format is ignored (QR uses grayscale).
 * NULL options selects the complete image. Successful results belong to the
 * caller, including an empty result when no code is found. */
esp_err_t solar_os_vision_qrcodes(solar_os_raster_image_t *image,
    const solar_os_raster_image_convert_options_t *options,
    solar_os_vision_cancel_fn cancel, void *user,
    solar_os_vision_qr_results_t **results);
void solar_os_vision_qr_results_free(solar_os_vision_qr_results_t *results);

#ifdef __cplusplus
}
#endif
