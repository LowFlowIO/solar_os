#include "solar_os_dlp_app.h"

#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "solar_os_dlp_receiver.h"
#include "solar_os_gfx.h"
#include "solar_os_memory.h"

typedef struct {
    solar_os_dlp_receiver_t receiver;
    solar_os_stream_handle_t stream;
    solar_os_gfx_t *gfx;
    uint8_t *storage;
    char display[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char stream_name[SOLAR_OS_STREAM_ID_MAX];
    bool direct_mono, suspended;
} dlp_app_state_t;

static void *dlp_state;
#define state (*(dlp_app_state_t *)dlp_state)

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("Cross-task status snapshot remains valid after cold app state is released")
static solar_os_dlp_app_status_t status_snapshot;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("Protect cross-task status snapshot without accessing cold app state")
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

void solar_os_dlp_app_get_status(solar_os_dlp_app_status_t *status)
{
    portENTER_CRITICAL(&status_lock);
    *status = status_snapshot;
    portEXIT_CRITICAL(&status_lock);
}

static void publish_status(bool running)
{
    solar_os_dlp_app_status_t status = {.running = running, .suspended = state.suspended,
        .received_bytes = state.receiver.received_bytes, .sent_bytes = state.receiver.sent_bytes,
        .presented_frames = state.receiver.presented_frames};
    strlcpy(status.display, state.display, sizeof(status.display));
    strlcpy(status.stream, state.stream_name, sizeof(status.stream));
    portENTER_CRITICAL(&status_lock);
    status_snapshot = status;
    portEXIT_CRITICAL(&status_lock);
}

static int read_bytes(void *user, uint8_t *bytes, size_t size)
{
    dlp_app_state_t *s = user;
    size_t count = 0;
    esp_err_t err = solar_os_stream_read(&s->stream, bytes, size, 0, &count);
    return err == ESP_OK || err == ESP_ERR_TIMEOUT ? (int)count : -1;
}

static int write_bytes(void *user, const uint8_t *bytes, size_t size)
{
    dlp_app_state_t *s = user;
    size_t count = 0;
    esp_err_t err = solar_os_stream_write(&s->stream, bytes, size, 0, &count);
    return err == ESP_OK || err == ESP_ERR_TIMEOUT ? (int)count : -1;
}

static int present_image(void *user, unsigned format, const uint8_t *data,
    size_t size, uint16_t width, uint16_t height)
{
    dlp_app_state_t *s = user;
    if (s->suspended) return DLP_BUSY;
    if (solar_os_gfx_width(s->gfx) != width || solar_os_gfx_height(s->gfx) != height)
        return DLP_IO_ERROR;
    if (format == DLP_RGB565_BE || s->direct_mono) {
        static const uint16_t palette[] = {0xffff, 0};
        solar_os_display_raster_t frame = {.data = data, .data_size = size,
            .source_width = width, .source_height = height,
            .source_stride = dlp_row_bytes(width, format), .width = width, .height = height,
            .format = format == DLP_RGB565_BE ? SOLAR_OS_DISPLAY_FORMAT_RGB565 : SOLAR_OS_DISPLAY_FORMAT_MONO1,
            .palette_rgb565 = palette, .palette_size = 2};
        return solar_os_gfx_present_frame(s->gfx, &frame) == ESP_OK ? DLP_OK : DLP_IO_ERROR;
    }
    solar_os_gfx_clear(s->gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(s->gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_bitmap(s->gfx, 0, 0, width, height, data);
    solar_os_gfx_present(s->gfx);
    return DLP_OK;
}

static void cleanup(void)
{
    if (!dlp_state) return;
    (void)solar_os_stream_close_ex(&state.stream);
    solar_os_memory_free(state.storage);
    state.storage = NULL;
    publish_status(false);
}

static esp_err_t start(solar_os_context_t *ctx)
{
    state.stream = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    state.gfx = solar_os_context_gfx(ctx);
    if (!state.gfx || !solar_os_gfx_display_target_name(state.gfx, state.display, sizeof(state.display)))
        return ESP_ERR_NOT_SUPPORTED;
    const char *arg = solar_os_context_argv(ctx, 1);
    if (solar_os_context_argc(ctx) != 2 || !arg || strncmp(arg, "stream=", 7) || !arg[7] ||
        strlcpy(state.stream_name, arg + 7, sizeof(state.stream_name)) >= sizeof(state.stream_name))
        return ESP_ERR_INVALID_ARG;
    solar_os_stream_info_t stream;
    esp_err_t err = solar_os_stream_get_info(state.stream_name, &stream);
    if (err != ESP_OK) return err;
    if (stream.type != SOLAR_OS_STREAM_TYPE_BYTES || stream.direction != SOLAR_OS_STREAM_DIRECTION_DUPLEX ||
        stream.sharing != SOLAR_OS_STREAM_SHARING_EXCLUSIVE) return ESP_ERR_NOT_SUPPORTED;
    solar_os_stream_open_options_t options = {.direction = SOLAR_OS_STREAM_DIRECTION_DUPLEX};
    err = solar_os_stream_open_ex(state.stream_name, "app-dlp", &options, &state.stream);
    if (err != ESP_OK) return err;

    size_t width = solar_os_gfx_width(state.gfx), height = solar_os_gfx_height(state.gfx);
    if (!width || !height || width > UINT16_MAX || height > UINT16_MAX ||
        width * height > 1024U * 1024U) return ESP_ERR_INVALID_SIZE;
    bool rgb = solar_os_gfx_supports_frame_format(state.gfx, SOLAR_OS_DISPLAY_FORMAT_RGB565);
    state.direct_mono = solar_os_gfx_supports_frame_format(state.gfx, SOLAR_OS_DISPLAY_FORMAT_MONO1);
    size_t capacity = rgb ? width * height * 2U : ((width + 7U) / 8U) * height;
    state.storage = solar_os_memory_alloc(capacity * 2U, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "dlp.images");
    if (!state.storage) return ESP_ERR_NO_MEM;
    dlp_info_t info = {.width = width, .height = height, .preferred_format = DLP_MONO1,
        .formats = DLP_FORMAT_BIT(DLP_MONO1) | (rgb ? DLP_FORMAT_BIT(DLP_RGB565_BE) : 0),
        .capabilities = DLP_PRESERVE_CONTENT | DLP_STAGED_FRAME | DLP_RLE | DLP_SYNC,
        .max_rect_bytes = capacity, .command_ms = 1000, .present_ms = 60000,
        .nominal_refresh_ms = 1000};
    snprintf(info.model, sizeof(info.model), "SolarOS %s", state.display);
    strlcpy(info.firmware, SOLAR_OS_VERSION, sizeof(info.firmware));
    solar_os_dlp_receiver_io_t io = {.user = &state, .read = read_bytes, .write = write_bytes,
        .present = present_image};
    uint64_t boot = ((uint64_t)esp_random() << 32) | esp_random();
    if (!solar_os_dlp_receiver_init(&state.receiver, &info, boot ? boot : 1,
        state.storage, capacity, &io)) return ESP_FAIL;
    solar_os_context_set_graphics_active(ctx, true);
    solar_os_gfx_clear(state.gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(state.gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(state.gfx, 8, 20, "DLP: waiting for host");
    solar_os_gfx_present(state.gfx);
    publish_status(true);
    return ESP_OK;
}

static void stop(solar_os_context_t *ctx)
{
    cleanup();
    solar_os_context_set_graphics_active(ctx, false);
}

static void suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    state.suspended = true;
    publish_status(true);
}

static void resume(solar_os_context_t *ctx)
{
    (void)ctx;
    state.suspended = false;
    if (state.receiver.valid)
        (void)present_image(&state, state.receiver.format, state.receiver.front,
            state.receiver.image_size, state.receiver.device.info.width, state.receiver.device.info.height);
    publish_status(true);
}

static bool event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_RESUME) { resume(ctx); return true; }
    if (event->type != SOLAR_OS_EVENT_TICK) return false;
    int result = solar_os_dlp_receiver_pump(&state.receiver, esp_timer_get_time());
    publish_status(true);
    if (result != DLP_OK) solar_os_context_finish(ctx, 1, "DLP: byte stream disconnected or stalled");
    return true;
}

const solar_os_app_t solar_os_dlp_app = {
    .name = "dlp", .summary = "DLP display receiver", .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE, .start = start, .stop = stop,
    .suspend = suspend, .resume = resume, .event = event,
    .state_slot = &dlp_state, .state_size = sizeof(dlp_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_cleanup = cleanup, .tick_interval_ms = 1,
};
