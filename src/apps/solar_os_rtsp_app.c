#include "solar_os_rtsp_app.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_audio.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_media_widgets.h"
#include "solar_os_rtsp_client.h"
#include "solar_os_shell_io.h"
#include "solar_os_signal_widgets.h"
#include "solar_os_stb_image.h"
#include "solar_os_task.h"

#define RTSP_NETWORK_STACK 8192U
#define RTSP_DECODE_STACK 24576U
#define RTSP_IMAGE_PIXELS (640U * 480U)
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(RTSP_DECODE_STACK);
#endif

typedef struct {
    solar_os_rtsp_client_t *client;
    solar_os_oscilloscope_widget_t *scope;
    SemaphoreHandle_t image_mutex;
    uint8_t *pixels;
    uint32_t image_width, image_height;
    uint32_t output_width, output_height;
    bool graphical, suspended, high_refresh, ui_started;
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char url[SOLAR_OS_RTSP_URI_MAX];
    TaskHandle_t network_task, decode_task;
    volatile bool network_done, decode_done, stop;
    uint32_t last_port_ms;
    uint32_t decode_errors;
} rtsp_app_state_t;

static void *rtsp_state;
#define rtsp (*(rtsp_app_state_t *)rtsp_state)

static void rtsp_samples(const int16_t *samples, size_t count, uint8_t channels, void *user)
{
    (void)user;
    if (rtsp.scope && channels)
        (void)solar_os_oscilloscope_widget_submit_s16(rtsp.scope, samples, count / channels, channels);
}

static void network_worker(void *arg)
{
    (void)arg;
    (void)solar_os_rtsp_client_run(rtsp.client);
    rtsp.network_done = true;
    for (;;) vTaskSuspend(NULL);
}

static void decode_worker(void *arg)
{
    (void)arg;
    while (!rtsp.stop && !rtsp.network_done) {
        if (rtsp.suspended) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        solar_os_rtp_jpeg_frame_t frame;
        if (!solar_os_rtsp_client_take_video(rtsp.client, &frame)) {
            vTaskDelay(pdMS_TO_TICKS(5)); continue;
        }
        uint8_t *pixels = NULL; uint32_t w = 0, h = 0;
        esp_err_t err = solar_os_stb_decode_jpeg_rgb_scaled(frame.data, frame.length,
            RTSP_IMAGE_PIXELS, rtsp.output_width, rtsp.output_height, &pixels, &w, &h);
        solar_os_rtsp_client_release_video(rtsp.client);
        if (err != ESP_OK) { rtsp.decode_errors++; continue; }
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        uint8_t *old = rtsp.pixels;
        rtsp.pixels = pixels; rtsp.image_width = w; rtsp.image_height = h;
        xSemaphoreGive(rtsp.image_mutex);
        solar_os_stb_image_free(old);
    }
    rtsp.decode_done = true;
    for (;;) vTaskSuspend(NULL);
}

static void refresh_override(solar_os_context_t *ctx, bool enabled)
{
    if (!rtsp.graphical || rtsp.high_refresh == enabled) return;
    if (enabled && !solar_os_gfx_display_target_name(solar_os_context_gfx(ctx),
        rtsp.display_target, sizeof(rtsp.display_target))) return;
    if (solar_os_display_set_high_refresh_override(rtsp.display_target, enabled, 255U) == ESP_OK)
        rtsp.high_refresh = enabled;
}

static void render(solar_os_context_t *ctx)
{
    if (!rtsp.graphical || rtsp.suspended) return;
    solar_os_rtsp_client_status_t status;
    solar_os_rtsp_client_status(rtsp.client, &status);
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    int w = solar_os_gfx_width(gfx), h = solar_os_gfx_height(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 5, 14, "RTSP");
    solar_os_gfx_line(gfx, 0, 20, w - 1, 20);
    if (status.video) {
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        if (rtsp.pixels) {
            const solar_os_gfx_raster_t raster = {
                .pixels = rtsp.pixels, .pixels_size = rtsp.image_width * rtsp.image_height * 3U,
                .width = rtsp.image_width, .height = rtsp.image_height,
                .stride = rtsp.image_width * 3U, .format = SOLAR_OS_GFX_RASTER_RGB888,
            };
            int draw_w = w, draw_h = (int)((uint64_t)rtsp.image_height * w / rtsp.image_width);
            if (draw_h > h - 44) { draw_h = h - 44; draw_w = (int)((uint64_t)rtsp.image_width * draw_h / rtsp.image_height); }
            (void)solar_os_gfx_blit_raster(gfx, &raster, (w - draw_w) / 2, 22 + (h - 44 - draw_h) / 2, draw_w, draw_h, NULL);
        } else solar_os_gfx_text(gfx, 5, 40, "Waiting for JPEG video...");
        xSemaphoreGive(rtsp.image_mutex);
    } else if (status.audio) {
        int bottom = h * 2 / 3;
        solar_os_oscilloscope_widget_draw(rtsp.scope, gfx, 5, 25, w - 10, bottom - 30);
        solar_os_gfx_line(gfx, 0, bottom, w - 1, bottom);
        char text[64];
        snprintf(text, sizeof(text), "%s  %lu Hz  %u ch", status.audio_playing ? "Playing" : "Buffering",
                 (unsigned long)status.sample_rate, status.channels);
        solar_os_gfx_text(gfx, 5, bottom + 16, text);
        solar_os_media_transport_button_draw(gfx, 5, bottom + 22, 26, 22,
            status.audio_playing ? SOLAR_OS_MEDIA_TRANSPORT_PLAY : SOLAR_OS_MEDIA_TRANSPORT_STOP, true);
    } else solar_os_gfx_text(gfx, 5, 40, "Connecting...");
    solar_os_audio_status_t audio;
    solar_os_audio_get_status(&audio);
    char footer[80];
    snprintf(footer, sizeof(footer), "Q exit  +/- volume %u%%  frames %lu", audio.volume, (unsigned long)status.video_frames);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, 5, h - 4, footer);
    solar_os_gfx_present(gfx);
}

static esp_err_t start(solar_os_context_t *ctx)
{
    rtsp.network_done = rtsp.decode_done = true;
    bool audio_only = false;
    const char *url = NULL;
    for (int i = 1; i < solar_os_context_argc(ctx); i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (!strcmp(arg, "--audio-only")) audio_only = true;
        else if (!url) url = arg;
        else return ESP_ERR_INVALID_ARG;
    }
    if (!url) return ESP_ERR_INVALID_ARG;
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    rtsp.graphical = solar_os_context_gfx(ctx) &&
        (!io || solar_os_shell_io_kind(io) != SOLAR_OS_SHELL_IO_KIND_PORT);
    solar_os_context_set_app_class(ctx, rtsp.graphical ? SOLAR_OS_APP_CLASS_GUI : SOLAR_OS_APP_CLASS_COMMAND);
    if (rtsp.graphical) {
        esp_err_t err = solar_os_oscilloscope_widget_create(256, &rtsp.scope);
        if (err != ESP_OK) return err;
        rtsp.image_mutex = xSemaphoreCreateMutex();
        if (!rtsp.image_mutex) return ESP_ERR_NO_MEM;
        rtsp.output_width = solar_os_gfx_width(solar_os_context_gfx(ctx));
        rtsp.output_height = solar_os_gfx_height(solar_os_context_gfx(ctx));
    }
    const solar_os_rtsp_client_options_t options = {
        .video = rtsp.graphical && !audio_only, .audio = true, .samples = rtsp_samples,
    };
    esp_err_t err = solar_os_rtsp_client_create(url, &options, &rtsp.client);
    if (err != ESP_OK) return err;
    strcpy(rtsp.url, url);
    rtsp.network_done = false;
    if (solar_os_task_create_pinned_external(network_worker, "rtsp-net", RTSP_NETWORK_STACK, NULL,
        tskIDLE_PRIORITY + 2, &rtsp.network_task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        rtsp.network_done = true; return ESP_ERR_NO_MEM;
    }
    rtsp.ui_started = true;
    if (rtsp.graphical) { refresh_override(ctx, true); solar_os_context_set_graphics_active(ctx, true); }
    else solar_os_shell_io_printf(io, "RTSP %s (audio only on port shell)\r\n", url);
    render(ctx);
    return ESP_OK;
}

static void stop(solar_os_context_t *ctx)
{
    rtsp.stop = true;
    solar_os_rtsp_client_cancel(rtsp.client);
    if (rtsp.ui_started && rtsp.graphical) { refresh_override(ctx, false); solar_os_context_set_graphics_active(ctx, false); }
    rtsp.ui_started = false;
    /* DNS and device writes have their own deadlines. If they outlive this
     * shell stop budget, retain cold state until the owners have finished. */
    (void)solar_os_task_wait_done(rtsp.network_task, &rtsp.network_done, SOLAR_OS_TASK_STOP_WAIT_MS);
    (void)solar_os_task_wait_done(rtsp.decode_task, &rtsp.decode_done, SOLAR_OS_TASK_STOP_WAIT_MS);
}

static bool release_ready(void) { return rtsp.network_done && rtsp.decode_done; }

static void cleanup(void)
{
    if (rtsp.network_task) solar_os_task_delete_external(rtsp.network_task);
    if (rtsp.decode_task) solar_os_task_delete_external(rtsp.decode_task);
    solar_os_rtsp_client_destroy(rtsp.client);
    solar_os_oscilloscope_widget_destroy(rtsp.scope);
    solar_os_stb_image_free(rtsp.pixels);
    if (rtsp.image_mutex) vSemaphoreDelete(rtsp.image_mutex);
}

static void suspend(solar_os_context_t *ctx)
{
    rtsp.suspended = true; refresh_override(ctx, false);
    solar_os_context_set_graphics_active(ctx, false);
}
static void resume(solar_os_context_t *ctx)
{
    rtsp.suspended = false; refresh_override(ctx, true);
    solar_os_context_set_graphics_active(ctx, rtsp.graphical); render(ctx);
}

static bool event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_RESUME) { resume(ctx); return true; }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        uint8_t key = event->data.ch;
        if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q')
            solar_os_context_finish(ctx, 0, NULL);
        else if (key == '+' || key == '-') {
            solar_os_audio_status_t status; solar_os_audio_get_status(&status);
            int volume = status.volume + (key == '+' ? 5 : -5);
            (void)solar_os_audio_set_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_TICK) return false;
    solar_os_rtsp_client_status_t status;
    solar_os_rtsp_client_status(rtsp.client, &status);
    if (rtsp.network_done) {
        char message[128];
        snprintf(message, sizeof(message), "RTSP stream ended: %s", esp_err_to_name(status.error));
        solar_os_context_finish(ctx, status.error == ESP_OK ? 0 : 1, status.error == ESP_OK ? NULL : message);
        return true;
    }
    if (status.video && !rtsp.decode_task) {
        rtsp.decode_done = false;
        if (solar_os_task_create_pinned_external(decode_worker, "rtsp-jpeg", RTSP_DECODE_STACK, NULL,
            tskIDLE_PRIORITY + 1, &rtsp.decode_task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
            rtsp.decode_done = true;
            solar_os_context_finish(ctx, 1, "RTSP JPEG decoder task unavailable"); return true;
        }
    }
    if (!rtsp.graphical && event->data.tick_ms - rtsp.last_port_ms >= 1000) {
        rtsp.last_port_ms = event->data.tick_ms;
        solar_os_shell_io_printf(solar_os_context_shell_io(ctx), "%s  %lu Hz  %u ch  dropped %lu\r\n",
            status.audio_playing ? "Playing" : "Connecting/buffering", (unsigned long)status.sample_rate,
            status.channels, (unsigned long)status.audio_dropped);
    }
    render(ctx); return true;
}

const solar_os_app_t solar_os_rtsp_app = {
    .name = "rtsp", .summary = "RTSP JPEG/L16 viewer", .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = start, .stop = stop, .suspend = suspend, .resume = resume, .event = event,
    .state_slot = &rtsp_state, .state_size = sizeof(rtsp_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_ready = release_ready, .state_release_cleanup = cleanup,
    .tick_interval_ms = 40, .worker_stack_bytes = RTSP_NETWORK_STACK, .worker_stack_external = true,
};
