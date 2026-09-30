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
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "esp_timer.h"

#define RTSP_NETWORK_STACK 8192U
#define RTSP_DECODE_STACK 24576U
#define RTSP_IMAGE_PIXELS (640U * 480U)
#define RTSP_VIDEO_SLOTS 2U
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(RTSP_DECODE_STACK);
#endif

typedef struct {
    uint8_t *pixels;
    uint32_t width, height, timestamp;
    uint64_t arrived_us;
} rtsp_image_t;

typedef struct {
    solar_os_rtsp_client_t *client;
    solar_os_oscilloscope_widget_t *scope;
    SemaphoreHandle_t image_mutex;
    uint8_t *pixels;
    uint32_t image_width, image_height;
    uint32_t output_width, output_height;
    rtsp_image_t queue[RTSP_VIDEO_SLOTS];
    unsigned queued;
    bool dirty, diagnostics;
    uint32_t displayed, video_skipped, last_ui_ms;
    uint32_t decoded, decode_us, decode_max_us, scale_us, scale_max_us;
    uint32_t draws, draw_us, draw_max_us, age_max_us, gap_max_us;
    uint64_t last_shown_us, last_stats_us;
    uint32_t previous_received, previous_decoded, previous_displayed, previous_draws;
    uint32_t previous_decode_us, previous_scale_us, previous_draw_us, previous_audio_blocks;
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

/* Scale once on the decoder, not on every UI tick. Integer source maps make
 * the presentation blit unscaled and avoid a 64-bit divide per screen pixel. */
static uint8_t *prepare_image(const uint8_t *source, uint32_t sw, uint32_t sh,
                              uint32_t *dw, uint32_t *dh)
{
    uint32_t w = rtsp.output_width, h = (uint64_t)sh * w / sw;
    if (h > rtsp.output_height) { h = rtsp.output_height; w = (uint64_t)sw * h / sh; }
    if (!w || !h || w > RTSP_IMAGE_PIXELS / h) return NULL;
    uint8_t *pixels = solar_os_memory_alloc((size_t)w * h * 3U,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "rtsp.image");
    if (!pixels) return NULL;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = source + (size_t)((uint64_t)y * sh / h) * sw * 3U;
        uint32_t sx = 0, remainder = 0;
        for (uint32_t x = 0; x < w; x++) {
            memcpy(pixels + ((size_t)y * w + x) * 3U, row + sx * 3U, 3U);
            remainder += sw;
            while (remainder >= w) { sx++; remainder -= w; }
        }
    }
    *dw = w; *dh = h; return pixels;
}

static void decode_worker(void *arg)
{
    (void)arg;
    while (!rtsp.stop && !rtsp.network_done) {
        if (rtsp.suspended) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        bool full = rtsp.queued == RTSP_VIDEO_SLOTS;
        xSemaphoreGive(rtsp.image_mutex);
        /* Preserve frames waiting for their presentation deadline. Replacing
         * the oldest on each fast source tick would starve playback forever. */
        if (full) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        solar_os_rtp_jpeg_frame_t frame;
        uint64_t arrived;
        if (!solar_os_rtsp_client_take_video(rtsp.client, &frame, &arrived)) {
            vTaskDelay(pdMS_TO_TICKS(5)); continue;
        }
        uint8_t *pixels = NULL; uint32_t w = 0, h = 0;
        uint64_t decode_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
        esp_err_t err = solar_os_stb_decode_jpeg_rgb_scaled(frame.data, frame.length,
            RTSP_IMAGE_PIXELS, rtsp.output_width, rtsp.output_height, &pixels, &w, &h);
        solar_os_rtsp_client_release_video(rtsp.client);
        if (err != ESP_OK) {
            xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY); rtsp.decode_errors++;
            xSemaphoreGive(rtsp.image_mutex); continue;
        }
        uint64_t scale_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
        uint32_t draw_w, draw_h;
        uint8_t *prepared = prepare_image(pixels, w, h, &draw_w, &draw_h);
        solar_os_stb_image_free(pixels);
        if (!prepared) {
            xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY); rtsp.decode_errors++;
            xSemaphoreGive(rtsp.image_mutex); continue;
        }
        uint64_t prepared_us = rtsp.diagnostics ? esp_timer_get_time() : 0;
        xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
        if (rtsp.diagnostics) {
            uint32_t decode = scale_start - decode_start, scale = prepared_us - scale_start;
            rtsp.decoded++; rtsp.decode_us += decode; rtsp.scale_us += scale;
            if (decode > rtsp.decode_max_us) rtsp.decode_max_us = decode;
            if (scale > rtsp.scale_max_us) rtsp.scale_max_us = scale;
        }
        /* Only this worker adds entries; the UI can only remove them. */
        rtsp.queue[rtsp.queued++] = (rtsp_image_t){.pixels = prepared,
            .width = draw_w, .height = draw_h, .timestamp = frame.timestamp, .arrived_us = arrived};
        xSemaphoreGive(rtsp.image_mutex);
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

static void render(solar_os_context_t *ctx, bool force)
{
    if (!rtsp.graphical || rtsp.suspended) return;
    solar_os_rtsp_client_status_t status;
    solar_os_rtsp_client_status(rtsp.client, &status);
    bool changed = false;
    uint64_t arrived = 0;
    xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
    while (rtsp.queued) {
        rtsp_image_t image = rtsp.queue[0];
        int64_t late = solar_os_rtsp_client_video_lateness(rtsp.client, image.timestamp, image.arrived_us);
        if (late < 0) break;
        memmove(rtsp.queue, rtsp.queue + 1, (--rtsp.queued) * sizeof(rtsp.queue[0]));
        if (late > 150000) { solar_os_memory_free(image.pixels); rtsp.video_skipped++; continue; }
        if (changed) rtsp.video_skipped++; /* Due frames coalesced into one draw. */
        solar_os_memory_free(rtsp.pixels);
        rtsp.pixels = image.pixels; rtsp.image_width = image.width; rtsp.image_height = image.height;
        arrived = image.arrived_us; changed = true;
    }
    bool dirty = rtsp.dirty;
    rtsp.dirty = false;
    xSemaphoreGive(rtsp.image_mutex);
    if (status.video && !changed && !dirty && !force) return;
    uint64_t draw_start = rtsp.diagnostics ? esp_timer_get_time() : 0;
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
            int draw_w = rtsp.image_width, draw_h = rtsp.image_height;
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
    if (changed) rtsp.displayed++;
    if (rtsp.diagnostics) {
        uint64_t now = esp_timer_get_time();
        uint32_t duration = now - draw_start;
        rtsp.draws++; rtsp.draw_us += duration;
        if (duration > rtsp.draw_max_us) rtsp.draw_max_us = duration;
        if (changed) {
            uint32_t age = now - arrived;
            uint32_t gap = rtsp.last_shown_us ? now - rtsp.last_shown_us : 0;
            if (age > rtsp.age_max_us) rtsp.age_max_us = age;
            if (gap > rtsp.gap_max_us) rtsp.gap_max_us = gap;
            rtsp.last_shown_us = now;
        }
    }
}

static void diagnostics_tick(const solar_os_rtsp_client_status_t *status)
{
    if (!rtsp.diagnostics) return;
    uint64_t now = esp_timer_get_time();
    if (now - rtsp.last_stats_us < 1000000) return;
    uint32_t window_ms = (now - rtsp.last_stats_us) / 1000;
    rtsp.last_stats_us = now;
    if (rtsp.image_mutex) xSemaphoreTake(rtsp.image_mutex, portMAX_DELAY);
    uint32_t decoded = rtsp.decoded - rtsp.previous_decoded;
    uint32_t draws = rtsp.draws - rtsp.previous_draws;
    uint32_t decode_avg = decoded ? (rtsp.decode_us - rtsp.previous_decode_us) / decoded : 0;
    uint32_t scale_avg = decoded ? (rtsp.scale_us - rtsp.previous_scale_us) / decoded : 0;
    uint32_t draw_avg = draws ? (rtsp.draw_us - rtsp.previous_draw_us) / draws : 0;
    uint32_t decode_max = rtsp.decode_max_us, scale_max = rtsp.scale_max_us;
    unsigned queued = rtsp.queued;
    uint32_t errors = rtsp.decode_errors;
    rtsp.previous_decoded = rtsp.decoded;
    rtsp.previous_decode_us = rtsp.decode_us; rtsp.previous_scale_us = rtsp.scale_us;
    if (rtsp.image_mutex) xSemaphoreGive(rtsp.image_mutex);
    SOLAR_OS_LOGI("rtsp.stats", "video %lums rx=%lu dec=%lu shown=%lu q=%u drops=%lu/%lu errors=%lu age_max=%luus gap_max=%luus",
        (unsigned long)window_ms, (unsigned long)(status->video_frames - rtsp.previous_received), (unsigned long)decoded,
        (unsigned long)(rtsp.displayed - rtsp.previous_displayed), queued,
        (unsigned long)status->video_dropped, (unsigned long)rtsp.video_skipped, (unsigned long)errors,
        (unsigned long)rtsp.age_max_us, (unsigned long)rtsp.gap_max_us);
    SOLAR_OS_LOGI("rtsp.stats", "us avg/max decode=%lu/%lu scale=%lu/%lu draw=%lu/%lu",
        (unsigned long)decode_avg, (unsigned long)decode_max, (unsigned long)scale_avg,
        (unsigned long)scale_max, (unsigned long)draw_avg, (unsigned long)rtsp.draw_max_us);
    SOLAR_OS_LOGI("rtsp.stats", "audio %luHz/%u -> %luHz/%u quantum=%u blocks/s=%lu q=%lu drop=%lu conceal=%lu",
        (unsigned long)status->sample_rate, status->channels, (unsigned long)status->audio_output_rate,
        status->audio_output_channels, status->audio_block_frames,
        (unsigned long)(status->audio_blocks - rtsp.previous_audio_blocks),
        (unsigned long)status->audio_queued, (unsigned long)status->audio_dropped, (unsigned long)status->audio_concealed);
    SOLAR_OS_LOGI("rtsp.stats", "audio us write_max=%lu submit_gap_max=%lu jitter_wait_polls=%lu output_frames=%lu",
        (unsigned long)status->audio_write_max_us, (unsigned long)status->audio_gap_max_us,
        (unsigned long)status->audio_wait_polls, (unsigned long)status->audio_output_frames);
    rtsp.previous_received = status->video_frames; rtsp.previous_displayed = rtsp.displayed;
    rtsp.previous_draws = rtsp.draws; rtsp.previous_draw_us = rtsp.draw_us;
    rtsp.previous_audio_blocks = status->audio_blocks;
}

static esp_err_t start(solar_os_context_t *ctx)
{
    rtsp.network_done = rtsp.decode_done = true;
    bool audio_only = false;
    const char *url = NULL;
    for (int i = 1; i < solar_os_context_argc(ctx); i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (!strcmp(arg, "--audio-only")) audio_only = true;
        else if (!strcmp(arg, "--stats")) rtsp.diagnostics = true;
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
        int body_height = solar_os_gfx_height(solar_os_context_gfx(ctx)) - 44;
        if (body_height <= 0) return ESP_ERR_NOT_SUPPORTED;
        rtsp.output_height = body_height;
    }
    const solar_os_rtsp_client_options_t options = {
        .video = rtsp.graphical && !audio_only, .audio = true, .samples = rtsp_samples,
        .diagnostics = rtsp.diagnostics,
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
    rtsp.last_stats_us = esp_timer_get_time();
    if (rtsp.graphical) { refresh_override(ctx, true); solar_os_context_set_graphics_active(ctx, true); }
    else solar_os_shell_io_printf(io, "RTSP %s (audio only on port shell)\r\n", url);
    render(ctx, true);
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
    solar_os_memory_free(rtsp.pixels);
    for (unsigned i = 0; i < rtsp.queued; i++) solar_os_memory_free(rtsp.queue[i].pixels);
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
    solar_os_context_set_graphics_active(ctx, rtsp.graphical); render(ctx, true);
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
            rtsp.dirty = true;
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
    bool refresh = event->data.tick_ms - rtsp.last_ui_ms >= 1000;
    if (refresh) rtsp.last_ui_ms = event->data.tick_ms;
    render(ctx, refresh); diagnostics_tick(&status); return true;
}

const solar_os_app_t solar_os_rtsp_app = {
    .name = "rtsp", .summary = "RTSP JPEG/L16 viewer", .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = start, .stop = stop, .suspend = suspend, .resume = resume, .event = event,
    .state_slot = &rtsp_state, .state_size = sizeof(rtsp_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_ready = release_ready, .state_release_cleanup = cleanup,
    .tick_interval_ms = 20, .worker_stack_bytes = RTSP_NETWORK_STACK, .worker_stack_external = true,
};
