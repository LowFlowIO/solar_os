#include "solar_os_camera_esp32.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_camera.h"
#include "solar_os_board.h"
#include "solar_os_camera.h"

#define CAMERA_XCLK_HZ 20000000
#define CAMERA_LEDC_TIMER LEDC_TIMER_1
#define CAMERA_LEDC_CHANNEL LEDC_CHANNEL_4

typedef struct {
    bool started;
} camera_esp32_state_t;

static camera_esp32_state_t camera_esp32;

static framesize_t native_frame_size(solar_os_camera_frame_size_t frame_size)
{
    return frame_size == SOLAR_OS_CAMERA_FRAME_SIZE_VGA ?
        FRAMESIZE_VGA : FRAMESIZE_QVGA;
}

static const char *sensor_name(uint16_t product_id)
{
    for (size_t index = 0U; index < CAMERA_MODEL_MAX; index++) {
        if ((uint16_t)camera_sensor[index].pid == product_id) {
            return camera_sensor[index].name;
        }
    }
    return "unknown";
}

static esp_err_t camera_start(void *ctx,
                              const solar_os_camera_config_t *config,
                              solar_os_camera_sensor_info_t *sensor_info)
{
    camera_esp32_state_t *state = ctx;
    if (state->started) {
        return ESP_ERR_INVALID_STATE;
    }

    const camera_config_t native_config = {
        .pin_pwdn = SOLAR_OS_BOARD_PIN_CAMERA_PWDN,
        .pin_reset = SOLAR_OS_BOARD_PIN_CAMERA_RESET,
        .pin_xclk = SOLAR_OS_BOARD_PIN_CAMERA_XCLK,
        .pin_sccb_sda = SOLAR_OS_BOARD_PIN_CAMERA_SIOD,
        .pin_sccb_scl = SOLAR_OS_BOARD_PIN_CAMERA_SIOC,
        .pin_d7 = SOLAR_OS_BOARD_PIN_CAMERA_D7,
        .pin_d6 = SOLAR_OS_BOARD_PIN_CAMERA_D6,
        .pin_d5 = SOLAR_OS_BOARD_PIN_CAMERA_D5,
        .pin_d4 = SOLAR_OS_BOARD_PIN_CAMERA_D4,
        .pin_d3 = SOLAR_OS_BOARD_PIN_CAMERA_D3,
        .pin_d2 = SOLAR_OS_BOARD_PIN_CAMERA_D2,
        .pin_d1 = SOLAR_OS_BOARD_PIN_CAMERA_D1,
        .pin_d0 = SOLAR_OS_BOARD_PIN_CAMERA_D0,
        .pin_vsync = SOLAR_OS_BOARD_PIN_CAMERA_VSYNC,
        .pin_href = SOLAR_OS_BOARD_PIN_CAMERA_HREF,
        .pin_pclk = SOLAR_OS_BOARD_PIN_CAMERA_PCLK,
        .xclk_freq_hz = CAMERA_XCLK_HZ,
        .ledc_timer = CAMERA_LEDC_TIMER,
        .ledc_channel = CAMERA_LEDC_CHANNEL,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = native_frame_size(config->frame_size),
        .jpeg_quality = config->jpeg_quality,
        .fb_count = 1U,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .sccb_i2c_port = -1,
    };

    const esp_err_t error = esp_camera_init(&native_config);
    if (error != ESP_OK) {
        return error;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        (void)esp_camera_deinit();
        return ESP_ERR_INVALID_RESPONSE;
    }

    state->started = true;
    sensor_info->product_id = sensor->id.PID;
    strlcpy(sensor_info->name, sensor_name(sensor->id.PID),
            sizeof(sensor_info->name));
    return ESP_OK;
}

static esp_err_t camera_stop(void *ctx)
{
    camera_esp32_state_t *state = ctx;
    if (!state->started) {
        return ESP_OK;
    }
    const esp_err_t error = esp_camera_deinit();
    if (error == ESP_OK) {
        state->started = false;
    }
    return error;
}

static esp_err_t camera_capture(void *ctx,
                                solar_os_camera_backend_frame_t *frame)
{
    camera_esp32_state_t *state = ctx;
    if (!state->started) {
        return ESP_ERR_INVALID_STATE;
    }
    camera_fb_t *native_frame = esp_camera_fb_get();
    if (native_frame == NULL) {
        return ESP_ERR_TIMEOUT;
    }
    if (native_frame->format != PIXFORMAT_JPEG || native_frame->buf == NULL ||
        native_frame->len < 4U || native_frame->buf[0] != 0xffU ||
        native_frame->buf[1] != 0xd8U ||
        native_frame->buf[native_frame->len - 2U] != 0xffU ||
        native_frame->buf[native_frame->len - 1U] != 0xd9U) {
        esp_camera_fb_return(native_frame);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *frame = (solar_os_camera_backend_frame_t) {
        .data = native_frame->buf,
        .length = native_frame->len,
        .width = native_frame->width,
        .height = native_frame->height,
        .timestamp_us = (uint64_t)native_frame->timestamp.tv_sec * 1000000ULL +
            (uint64_t)native_frame->timestamp.tv_usec,
        .release_token = native_frame,
    };
    return ESP_OK;
}

static void camera_release(void *ctx, void *release_token)
{
    (void)ctx;
    esp_camera_fb_return(release_token);
}

esp_err_t solar_os_camera_esp32_register(void)
{
    static const solar_os_camera_backend_ops_t operations = {
        .start = camera_start,
        .stop = camera_stop,
        .capture = camera_capture,
        .release = camera_release,
    };
    const solar_os_camera_backend_t backend = {
        .driver = "esp32-camera",
        .ops = &operations,
        .ctx = &camera_esp32,
    };
    return solar_os_camera_register_backend(&backend);
}
