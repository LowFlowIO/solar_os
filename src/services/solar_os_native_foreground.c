#include "solar_os_native_foreground.h"
#include "solar_os_app_registry.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "solar_os_config.h"
#include "solar_os_display.h"
#include "solar_os_frame_presenter.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_native.h"
#include "solar_os_native_app_abi.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_shell.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif

#define FOREGROUND_FILE_MAX (1024U * 1024U)
#define FOREGROUND_STOP_WAIT_US (5000000ULL)

typedef struct foreground_slot foreground_slot_t;

typedef struct {
    const foreground_slot_t *slot;
    _Atomic(TaskHandle_t) worker;
    solar_os_gfx_t *gfx;
    bool keyboard_captured;
    QueueHandle_t keys;
    atomic_bool done, input_reset, stop_requested, redraw_requested;
    atomic_bool resumable, suspend_requested, suspended;
    uint64_t stop_deadline_us;
    bool worker_started;
    solar_os_frame_presenter_t *presenter;
    uint16_t palette[256];
    size_t frame_size;
    int argc;
    char args[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char cwd[SOLAR_OS_APP_ARG_LEN];
    char detail[160];
    esp_err_t result;
    int exit_code;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solar_os_audio_stream_t *audio;
    solar_os_audio_stream_format_t audio_format;
#endif
} foreground_state_t;


struct foreground_slot {
    foreground_slot_t *next;
    solar_os_native_app_manifest_v2_t manifest;
    solar_os_app_t app;
    solar_os_app_registry_entry_t registry;
    void *state;
    bool legacy;
    atomic_bool active;
    char owner[SOLAR_OS_APP_OWNER_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
};

static foreground_slot_t *foreground_slots;
static portMUX_TYPE foreground_slots_lock = portMUX_INITIALIZER_UNLOCKED;

static foreground_state_t *foreground_current(void)
{
    return solar_os_native_run_service_user();
}

static foreground_state_t *foreground_context_state(solar_os_context_t *ctx)
{
    return ctx && ctx->app && ctx->app->state_slot ? *ctx->app->state_slot : NULL;
}

static bool foreground_caller(void)
{
    foreground_state_t *state = foreground_current();
    return state && atomic_load(&state->worker) == xTaskGetCurrentTaskHandle() &&
        !atomic_load(&state->done);
}

static foreground_slot_t *foreground_find_slot(const solar_os_app_t *app)
{
    foreground_slot_t *found = NULL;
    portENTER_CRITICAL(&foreground_slots_lock);
    for (foreground_slot_t *slot = foreground_slots; slot; slot = slot->next) {
        if (&slot->app == app) { found = slot; break; }
    }
    portEXIT_CRITICAL(&foreground_slots_lock);
    return found;
}


static int foreground_result(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}

static uint64_t foreground_time_us(void) { return (uint64_t)esp_timer_get_time(); }

static void foreground_sleep_ms(uint32_t ms)
{
    if (foreground_caller()) {
        if (ms > 1000U) ms = 1000U;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1U);
    }
}

static void foreground_restart(void) { if (foreground_caller()) esp_restart(); }

static bool foreground_stop_requested(void)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller() || atomic_load(&state->stop_requested)) return true;
    /* The guest task owns presenter lifetime, including cleanup on return. */
    if (atomic_exchange(&state->redraw_requested, false) && state->presenter)
        (void)solar_os_frame_presenter_redraw(state->presenter);
    return false;
}

static uint16_t foreground_rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8U) & 0xf800U) |
        ((rgb >> 5U) & 0x07e0U) | ((rgb >> 3U) & 0x001fU));
}

static int foreground_text_colors(uint16_t *foreground, uint16_t *background)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!foreground || !background) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    solar_os_display_target_t target;
    if (!solar_os_gfx_display_target_name(state->gfx, name, sizeof(name)) ||
        !solar_os_display_find_target(name, &target)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *foreground = 0x0000U;
    *background = 0xffffU;
    if (target.frame_formats & SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT) {
        uint32_t fg, bg;
        esp_err_t err = solar_os_display_get_colors(&fg, &bg);
        if (err != ESP_OK) return foreground_result(err);
        bool inverted = solar_os_gfx_palette_inverted(state->gfx);
        *foreground = foreground_rgb565(inverted ? bg : fg);
        *background = foreground_rgb565(inverted ? fg : bg);
    }
    return SOLAR_OS_NATIVE_OK;
}

static int foreground_video_open_ex(uint16_t width, uint16_t height,
    const uint16_t *palette, uint16_t fps, uint8_t background_index)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!width || !height || width > 1024U || height > 1024U ||
        !palette || fps > 60U) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (state->presenter) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    memcpy(state->palette, palette, sizeof(state->palette));
    const solar_os_frame_presenter_config_t config = {
        .gfx = state->gfx, .format = SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .width = width, .height = height, .stride = width,
        .palette_rgb565 = state->palette, .palette_size = 256U,
        .fit = SOLAR_OS_FRAME_FIT_HEIGHT,
        .preferred_fps = fps, .allow_mono_fallback = true,
        .request_high_refresh = true, .clear_background_on_resume = true,
        .literal_direct_palette = true, .background_index = background_index,
    };
    esp_err_t err = solar_os_frame_presenter_init(&state->presenter, &config);
    if (err == ESP_OK) err = solar_os_frame_presenter_resume(state->presenter);
    if (err == ESP_OK) state->frame_size = (size_t)width * height;
    return foreground_result(err);
}

static int foreground_video_open(uint16_t width, uint16_t height,
    const uint16_t *palette, uint16_t fps)
{
    return foreground_video_open_ex(width, height, palette, fps, 15U);
}

static int foreground_present(const uint8_t *pixels, size_t length)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller() || !state->presenter) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!pixels || length != state->frame_size) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return solar_os_frame_presenter_submit(state->presenter, pixels, length) ?
        SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
}

static int foreground_read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!key || !available) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(key, 0, sizeof(*key));
    if (atomic_exchange(&state->input_reset, false)) {
        key->reset = true;
        *available = true;
    } else {
        *available = xQueueReceive(state->keys, key, 0) == pdTRUE;
    }
    return SOLAR_OS_NATIVE_OK;
}

static bool foreground_path_valid(const char *path)
{
    return path && path[0] == '/' &&
        strnlen(path, SOLAR_OS_STORAGE_PATH_MAX) < SOLAR_OS_STORAGE_PATH_MAX;
}

static int foreground_resolve_path(const char *path, char *resolved, size_t resolved_size)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!path || !resolved || !resolved_size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return foreground_result(solar_os_storage_resolve_path_at(state->cwd, path, resolved, resolved_size));
}

static int foreground_file_size(const char *path, uint32_t *size)
{
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!foreground_path_valid(path) || !size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_storage_metadata_t metadata;
    esp_err_t err = solar_os_storage_stat(path, &metadata);
    if (err == ESP_OK && (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE ||
        metadata.size_bytes > UINT32_MAX)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (err == ESP_OK) *size = (uint32_t)metadata.size_bytes;
    return foreground_result(err);
}

static int foreground_read_file(const char *path, uint32_t offset, void *buffer,
    size_t length, size_t *read_length)
{
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!foreground_path_valid(path) || !buffer || !read_length ||
        length > SOLAR_OS_STORAGE_READ_MAX_BYTES || offset > LONG_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *read_length = 0U;
    FILE *file = fopen(path, "rb");
    if (!file) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_OK;
    if (fseek(file, (long)offset, SEEK_SET)) result = SOLAR_OS_NATIVE_ERROR_FAILED;
    else {
        *read_length = fread(buffer, 1, length, file);
        if (ferror(file)) result = SOLAR_OS_NATIVE_ERROR_FAILED;
    }
    fclose(file);
    return result;
}

static int foreground_replace_file(const char *path, const void *buffer, size_t length)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!foreground_path_valid(path) || (!buffer && length) || length > FOREGROUND_FILE_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    char staged[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_sibling_path(path, ".app.tmp", staged, sizeof(staged));
    if (err == ESP_OK) err = solar_os_storage_sibling_path(path, ".app.bak", backup, sizeof(backup));
    if (err != ESP_OK) return foreground_result(err);
    FILE *file = fopen(staged, "wb");
    if (!file) return SOLAR_OS_NATIVE_ERROR_FAILED;
    const uint8_t *bytes = buffer;
    for (size_t offset = 0; offset < length && err == ESP_OK;) {
        size_t count = length - offset;
        if (count > SOLAR_OS_STORAGE_WRITE_MAX_BYTES) count = SOLAR_OS_STORAGE_WRITE_MAX_BYTES;
        if (fwrite(bytes + offset, 1, count, file) != count) err = ESP_FAIL;
        offset += count;
    }
    if (err == ESP_OK) err = solar_os_storage_sync_file(file);
    if (fclose(file) && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) err = solar_os_storage_replace_file(staged, path, backup);
    if (err != ESP_OK) (void)solar_os_storage_remove(staged);
    return foreground_result(err);
}

static void foreground_audio_close(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    foreground_state_t *state = foreground_current();
    if (foreground_caller() && state->audio) {
        solar_os_audio_stream_close(state->audio);
        state->audio = NULL;
    }
#endif
}

static int foreground_audio_open(uint32_t *rate, uint8_t *channels)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!rate || !channels) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (state->audio) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    esp_err_t err = solar_os_audio_stream_open(state->slot->manifest.id, 0, &state->audio, &state->audio_format);
    if (err == ESP_OK && (state->audio_format.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
        state->audio_format.bits_per_sample != 16U || state->audio_format.channels > 2U ||
        !state->audio_format.channels || !state->audio_format.sample_rate)) {
        foreground_audio_close();
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) { *rate = state->audio_format.sample_rate; *channels = state->audio_format.channels; }
    return foreground_result(err);
#else
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int foreground_audio_write(const int16_t *samples, size_t frames)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (!state->audio) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!samples || frames > 4096U) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return foreground_result(solar_os_audio_stream_write(state->audio, samples,
        frames * state->audio_format.channels * sizeof(*samples)));
#else
    (void)samples; (void)frames;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static void foreground_set_exit_code(int code) {
    foreground_state_t *state = foreground_current(); if (foreground_caller()) state->exit_code = code; }

static int foreground_enable_suspend(void)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller() || atomic_load(&state->suspend_requested))
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!state->slot->legacy &&
        !(state->slot->manifest.flags & SOLAR_OS_NATIVE_APP_RESUMABLE))
        return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    atomic_store(&state->resumable, true);
    return SOLAR_OS_NATIVE_OK;
}

static bool foreground_suspend_requested(void)
{
    foreground_state_t *state = foreground_current();
    return foreground_caller() && atomic_load(&state->suspend_requested);
}

static int foreground_guest_suspend(void)
{
    foreground_state_t *state = foreground_current();
    if (!foreground_caller() || !atomic_load(&state->resumable) ||
        !atomic_load(&state->suspend_requested)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    foreground_audio_close();
    /* Publish the acknowledgement only after the display worker has joined.
     * The main task can then release the graphics surface and keyboard. */
    if (state->presenter && solar_os_frame_presenter_suspend_checked(state->presenter) != ESP_OK)
        esp_restart();
    atomic_store(&state->suspended, true);
    while (atomic_load(&state->suspend_requested) && !atomic_load(&state->stop_requested))
        foreground_sleep_ms(SOLAR_OS_TASK_STOP_POLL_MS);
    int result = SOLAR_OS_NATIVE_OK;
    if (!atomic_load(&state->stop_requested) && state->presenter)
        result = foreground_result(solar_os_frame_presenter_resume(state->presenter));
    atomic_store(&state->suspended, false);
    return result;
}

static const solar_os_native_foreground_api_v1_t foreground_api = {
    .abi_version = 1U, .struct_size = sizeof(foreground_api),
    .time_us = foreground_time_us, .sleep_ms = foreground_sleep_ms, .restart = foreground_restart,
    .video_open = foreground_video_open, .present = foreground_present, .read_key = foreground_read_key,
    .file_size = foreground_file_size, .read_file = foreground_read_file, .replace_file = foreground_replace_file,
    .audio_open = foreground_audio_open, .audio_write = foreground_audio_write, .audio_close = foreground_audio_close,
    .set_exit_code = foreground_set_exit_code,
    .stop_requested = foreground_stop_requested,
    .text_colors = foreground_text_colors, .video_open_ex = foreground_video_open_ex,
    .resolve_path = foreground_resolve_path,
    .enable_suspend = foreground_enable_suspend, .suspend_requested = foreground_suspend_requested,
    .suspend = foreground_guest_suspend,
};



static const void *foreground_get_service(const char *name, uint32_t abi,
                                           uint32_t size, void *user)
{
    return user == foreground_current() && foreground_caller() && name &&
        (!strcmp(name, SOLAR_OS_NATIVE_FOREGROUND_SERVICE) || !strcmp(name, "vm")) &&
        abi == SOLAR_OS_NATIVE_FOREGROUND_ABI && size <= sizeof(foreground_api) ?
        &foreground_api : NULL;
}


static esp_err_t foreground_write(const char *text, size_t length, void *user)
{
    foreground_state_t *state = user;
    if (!text) return ESP_ERR_INVALID_ARG;
    if (length >= sizeof(state->detail)) length = sizeof(state->detail) - 1U;
    memcpy(state->detail, text, length);
    state->detail[length] = '\0';
    return ESP_OK;
}

static void foreground_worker_cleanup(void *user)
{
    foreground_state_t *state = user;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (state->audio) {
        solar_os_audio_stream_close(state->audio);
        state->audio = NULL;
    }
#endif
    if (state->presenter) {
        if (solar_os_frame_presenter_deinit(state->presenter) != ESP_OK) esp_restart();
        state->presenter = NULL;
    }
}

static void foreground_worker(void *arg)
{
    foreground_state_t *state = arg;
    atomic_store(&state->worker, xTaskGetCurrentTaskHandle());
    char *argv[SOLAR_OS_APP_ARG_MAX];
    for (int i = 0; i < state->argc; ++i) argv[i] = state->args[i];
    const solar_os_native_run_options_t options = {
        .write = foreground_write, .write_user = state,
        .get_service = foreground_get_service, .service_user = state,
        .cleanup = foreground_worker_cleanup,
        .expected_manifest = state->slot->legacy ? NULL : &state->slot->manifest,
    };
    char detail[160];
    state->result = solar_os_native_run(state->slot->path, state->argc, argv,
        &options, NULL, detail, sizeof(detail));
    if (state->result != ESP_OK && detail[0]) strlcpy(state->detail, detail, sizeof(state->detail));
    if (state->result == ESP_OK && state->exit_code != 0) state->result = ESP_FAIL;
    atomic_store(&state->worker, NULL);
    atomic_store(&state->done, true);
    solar_os_task_delete_internal(NULL);
}

static esp_err_t foreground_admit(const foreground_slot_t *slot)
{
    solar_os_memory_status_t memory;
    solar_os_memory_get_status(&memory);
    const solar_os_native_app_manifest_v2_t *manifest = &slot->manifest;
    if (memory.external.free < manifest->minimum_psram_bytes ||
        memory.internal.free < memory.internal_reserve + manifest->minimum_internal_bytes + manifest->worker_stack_bytes ||
        !solar_os_task_admit(manifest->id, manifest->worker_stack_bytes,
                             SOLAR_OS_TASK_ROLE_FOREGROUND, false)) return ESP_ERR_NO_MEM;
    return ESP_OK;
}


static esp_err_t foreground_start(solar_os_context_t *ctx)
{
    foreground_state_t *state = foreground_context_state(ctx);
    if (!state) return ESP_ERR_INVALID_STATE;
    state->slot = foreground_find_slot(ctx->app);
    if (!state->slot) return ESP_ERR_INVALID_STATE;
    const esp_err_t admission = foreground_admit(state->slot);
    if (admission != ESP_OK) return admission;
    atomic_store(&((foreground_slot_t *)state->slot)->active, true);
    atomic_init(&state->done, true);
    atomic_init(&state->worker, NULL);
    atomic_init(&state->input_reset, false);
    atomic_init(&state->stop_requested, false);
    atomic_init(&state->redraw_requested, false);
    atomic_init(&state->resumable, !state->slot->legacy &&
        (state->slot->manifest.flags & SOLAR_OS_NATIVE_APP_RESUMABLE));
    atomic_init(&state->suspend_requested, false);
    atomic_init(&state->suspended, false);
    state->stop_deadline_us = 0;
    state->worker_started = false;
    if (!ctx->gfx || ctx->argc < 1) {
        atomic_store(&((foreground_slot_t *)state->slot)->active, false);
        return ESP_ERR_NOT_SUPPORTED;
    }
    state->gfx = ctx->gfx;
    state->argc = ctx->argc;
    memcpy(state->args, ctx->argv, sizeof(state->args));
    strlcpy(state->args[0], state->slot->path, sizeof(state->args[0]));
    memcpy(state->cwd, ctx->launch_cwd, sizeof(state->cwd));
    atomic_store(&state->done, false);
    state->exit_code = 1;
    state->keys = xQueueCreate(64U, sizeof(solar_os_native_foreground_key_v1_t));
    if (!state->keys) {
        atomic_store(&state->done, true);
        atomic_store(&((foreground_slot_t *)state->slot)->active, false);
        return ESP_ERR_NO_MEM;
    }
    solar_os_context_set_streaming_graphics_active(ctx, true);
    if (solar_os_task_create_pinned_internal(foreground_worker, state->slot->manifest.id, state->slot->manifest.worker_stack_bytes,
        state, tskIDLE_PRIORITY + state->slot->manifest.worker_priority, NULL, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        atomic_store(&state->done, true);
        vQueueDelete(state->keys); state->keys = NULL;
        solar_os_context_set_streaming_graphics_active(ctx, false);
        atomic_store(&((foreground_slot_t *)state->slot)->active, false);
        return ESP_ERR_NO_MEM;
    }
    state->worker_started = true;
    return ESP_OK;
}

static void foreground_request_stop(foreground_state_t *state)
{
    if (!atomic_exchange(&state->stop_requested, true))
        state->stop_deadline_us = foreground_time_us() + FOREGROUND_STOP_WAIT_US;
}

static void foreground_stop(solar_os_context_t *ctx)
{
    foreground_state_t *state = foreground_context_state(ctx);
    if (!state) return;
    foreground_request_stop(state);
    /* The context and graphics must remain alive until the worker has joined
     * the presenter. Never delete a task executing module code forcibly. */
    while (!atomic_load(&state->done)) {
        if (foreground_time_us() >= state->stop_deadline_us) esp_restart();
        TickType_t delay = pdMS_TO_TICKS(SOLAR_OS_TASK_STOP_POLL_MS);
        vTaskDelay(delay ? delay : 1U);
    }
    if (state->worker_started) {
        TickType_t delay = pdMS_TO_TICKS(SOLAR_OS_TASK_REAP_WAIT_MS);
        vTaskDelay(delay ? delay : 1U);
        state->worker_started = false;
    }
    solar_os_input_release_keyboard(state);
    state->keyboard_captured = false;
    solar_os_context_set_streaming_graphics_active(ctx, false);
    if (state->keys) vQueueDelete(state->keys);
    state->keys = NULL;
    atomic_store(&((foreground_slot_t *)state->slot)->active, false);
}

static void foreground_suspend(solar_os_context_t *ctx)
{
    foreground_state_t *state = foreground_context_state(ctx);
    if (!state) return;
    if (!atomic_load(&state->resumable) || atomic_load(&state->stop_requested)) {
        foreground_stop(ctx);
        return;
    }
    atomic_store(&state->suspend_requested, true);
    const uint64_t deadline = foreground_time_us() + FOREGROUND_STOP_WAIT_US;
    while (!atomic_load(&state->suspended) && !atomic_load(&state->done)) {
        if (foreground_time_us() >= deadline) esp_restart();
        vTaskDelay(pdMS_TO_TICKS(SOLAR_OS_TASK_STOP_POLL_MS) ?
            pdMS_TO_TICKS(SOLAR_OS_TASK_STOP_POLL_MS) : 1U);
    }
    solar_os_input_release_keyboard(state);
    state->keyboard_captured = false;
    xQueueReset(state->keys);
    atomic_store(&state->input_reset, true);
    solar_os_context_set_streaming_graphics_active(ctx, false);
}

static void foreground_resume(solar_os_context_t *ctx)
{
    foreground_state_t *state = foreground_context_state(ctx);
    if (!state) return;
    if (atomic_load(&state->done)) return;
    if (!atomic_load(&state->suspended)) return;
    /* Restore graphics ownership before allowing the guest/presenter to run. */
    state->gfx = ctx->gfx;
    xQueueReset(state->keys);
    atomic_store(&state->input_reset, true);
    solar_os_context_set_streaming_graphics_active(ctx, true);
    atomic_store(&state->suspend_requested, false);
    const uint64_t deadline = foreground_time_us() + FOREGROUND_STOP_WAIT_US;
    while (atomic_load(&state->suspended) && !atomic_load(&state->done)) {
        if (foreground_time_us() >= deadline) esp_restart();
        vTaskDelay(pdMS_TO_TICKS(SOLAR_OS_TASK_STOP_POLL_MS) ?
            pdMS_TO_TICKS(SOLAR_OS_TASK_STOP_POLL_MS) : 1U);
    }
}

static void foreground_queue_key(foreground_state_t *state, solar_os_native_foreground_key_v1_t *key)
{
    if (key->key == SOLAR_OS_KEY_APP_EXIT || key->key == 0x1dU) {
        if (key->action != SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE) foreground_request_stop(state);
        return;
    }
    if (!atomic_load(&state->done) && xQueueSend(state->keys, key, 0) != pdTRUE) {
        xQueueReset(state->keys);
        atomic_store(&state->input_reset, true);
    }
}

static solar_os_native_foreground_key_v1_t foreground_key(const solar_os_input_key_event_t *key)
{
    return (solar_os_native_foreground_key_v1_t){
        .source = key->source, .physical_key = key->physical_key,
        .usage = key->usage, .key = key->key,
        .modifiers = key->modifiers, .action = (uint8_t)key->action,
    };
}

static void foreground_poll_keyboard(foreground_state_t *state)
{
    if (!state->keyboard_captured) {
        for (size_t i = 0; i < solar_os_input_source_count(); ++i) {
            solar_os_input_source_info_t source;
            if (solar_os_input_source_get(i, &source) && source.ready &&
                source.source_class == SOLAR_OS_INPUT_SOURCE_KEYBOARD &&
                (source.capabilities & SOLAR_OS_INPUT_CAP_KEY_EVENTS) &&
                solar_os_input_capture_keyboard_with_shortcuts(source.name, state) == ESP_OK) {
                state->keyboard_captured = true;
                break;
            }
        }
    }
    for (unsigned i = 0; i < 64U; ++i) {
        solar_os_input_key_event_t event = {0};
        bool reset;
        if (!solar_os_input_read_captured_key(state, &event, &reset)) break;
        if (reset) {
            xQueueReset(state->keys);
            atomic_store(&state->input_reset, true);
        } else {
            solar_os_native_foreground_key_v1_t key = foreground_key(&event);
            foreground_queue_key(state, &key);
        }
    }
}

static bool foreground_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    foreground_state_t *state = foreground_context_state(ctx);
    if (!state) return false;
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        /* An overlay can hide frames that the guest considers unchanged. */
        if (!atomic_load(&state->done)) atomic_store(&state->redraw_requested, true);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (atomic_load(&state->done)) solar_os_context_finish(ctx,
            state->result == ESP_OK ? 0 : 1, state->detail[0] ? state->detail : "app: module returned");
        else {
            foreground_poll_keyboard(state);
            if (atomic_load(&state->stop_requested) &&
                foreground_time_us() >= state->stop_deadline_us && !atomic_load(&state->done))
                esp_restart();
        }
        return true;
    }
    solar_os_native_foreground_key_v1_t key = {0};
    if (event->type == SOLAR_OS_EVENT_KEY) {
        key = foreground_key(&event->data.key);
    } else if (event->type == SOLAR_OS_EVENT_CHAR) key.key = (uint8_t)event->data.ch;
    else return false;
    foreground_queue_key(state, &key);
    return true;
}

static esp_err_t foreground_prepare_slot(const char *path,
    const solar_os_native_elf_info_t *info, bool legacy, const solar_os_app_t **app)
{
    solar_os_native_app_manifest_v2_t manifest = info->app;
    if (legacy) {
        manifest = (solar_os_native_app_manifest_v2_t){
            .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
            .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
            .worker_stack_bytes = 16U * 1024U, .worker_priority = 1U,
            .tick_interval_ms = 5U,
        };
        const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
        if (!strncmp(name, "solaros_native_", 15U)) name += 15U;
        size_t n = strcspn(name, ".");
        if (!n || n >= sizeof(manifest.id)) return ESP_ERR_INVALID_ARG;
        memcpy(manifest.id, name, n);
        strlcpy(manifest.summary, "external foreground application", sizeof(manifest.summary));
    }
    portENTER_CRITICAL(&foreground_slots_lock);
    size_t count = 0U;
    for (foreground_slot_t *slot = foreground_slots; slot; slot = slot->next) {
        count++;
        if (!strcmp(slot->manifest.id, manifest.id)) {
            const bool same = !strcmp(slot->path, path) && slot->legacy == legacy &&
                !memcmp(&slot->manifest, &manifest, sizeof(manifest));
            *app = same ? &slot->app : NULL;
            portEXIT_CRITICAL(&foreground_slots_lock);
            /* Descriptors can be referenced by reserved/suspended sessions.
             * Keep their identity and resource declaration immutable. */
            return same ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
    }
    portEXIT_CRITICAL(&foreground_slots_lock);
    if (count >= SOLAR_OS_MODULE_PACKAGE_COUNT_MAX) return ESP_ERR_NO_MEM;
    foreground_slot_t *slot = solar_os_memory_calloc(1U, sizeof(*slot),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.app.slot");
    if (!slot) return ESP_ERR_NO_MEM;
    slot->manifest = manifest;
    slot->legacy = legacy;
    atomic_init(&slot->active, false);
    strlcpy(slot->path, path, sizeof(slot->path));
    slot->app = (solar_os_app_t){
        .name = slot->manifest.id, .summary = slot->manifest.summary,
        .app_class = (solar_os_app_class_t)manifest.app_class,
        .flags = SOLAR_OS_APP_FLAG_KEY_EVENTS |
            ((manifest.flags & SOLAR_OS_NATIVE_APP_RESUMABLE) ? SOLAR_OS_APP_FLAG_RESUMABLE : 0U),
        .start = foreground_start, .stop = foreground_stop, .suspend = foreground_suspend,
        .resume = foreground_resume, .event = foreground_event,
        .state_slot = &slot->state, .state_size = sizeof(foreground_state_t),
        .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_REQUIRED,
        .worker_stack_bytes = manifest.worker_stack_bytes,
        .tick_interval_ms = manifest.tick_interval_ms,
    };
    slot->registry = (solar_os_app_registry_entry_t){
        .name = slot->manifest.id, .summary = slot->manifest.summary, .app = &slot->app,
        .capabilities = SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_TEXT |
            (manifest.app_class == SOLAR_OS_NATIVE_APP_CLASS_GUI ? SOLAR_OS_APP_CAP_GRAPHICS : 0U),
        .min_argc = 1U, .max_argc = SOLAR_OS_APP_ARG_MAX,
    };
    /* Recheck after allocation: another shell can discover the same module. */
    portENTER_CRITICAL(&foreground_slots_lock);
    count = 0U;
    for (foreground_slot_t *other = foreground_slots; other; other = other->next) {
        count++;
        if (!strcmp(other->manifest.id, manifest.id)) {
            const bool same = !strcmp(other->path, path) && other->legacy == legacy &&
                !memcmp(&other->manifest, &manifest, sizeof(manifest));
            *app = same ? &other->app : NULL;
            portEXIT_CRITICAL(&foreground_slots_lock);
            solar_os_memory_free(slot);
            return same ? ESP_OK : ESP_ERR_INVALID_STATE;
        }
    }
    if (count >= SOLAR_OS_MODULE_PACKAGE_COUNT_MAX) {
        portEXIT_CRITICAL(&foreground_slots_lock);
        solar_os_memory_free(slot);
        return ESP_ERR_NO_MEM;
    }
    slot->next = foreground_slots;
    foreground_slots = slot;
    portEXIT_CRITICAL(&foreground_slots_lock);
    *app = &slot->app;
    return ESP_OK;
}

esp_err_t solar_os_native_foreground_resolve(const char *path, bool legacy,
    const solar_os_app_t **app, char *detail, size_t detail_size)
{
    if (!path || !app || strlen(path) >= SOLAR_OS_STORAGE_PATH_MAX) return ESP_ERR_INVALID_ARG;
    *app = NULL;
    solar_os_storage_metadata_t metadata;
    esp_err_t err = solar_os_storage_stat(path, &metadata);
    if (err != ESP_OK) return err;
    if (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE || metadata.size_bytes < 52U ||
        metadata.size_bytes > SOLAR_OS_NATIVE_ELF_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    uint8_t *data = solar_os_memory_alloc((size_t)metadata.size_bytes,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.app.inspect");
    if (!data) return ESP_ERR_NO_MEM;
    size_t read = 0U;
    solar_os_native_elf_info_t info;
    err = solar_os_storage_read_file(path, data, (size_t)metadata.size_bytes, &read);
    if (err == ESP_OK && read != metadata.size_bytes) err = ESP_FAIL;
    if (err == ESP_OK) err = solar_os_native_elf_validate(data, read,
        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA, &info, detail, detail_size);
    solar_os_memory_free(data);
    if (err != ESP_OK) return err;
    if (!info.foreground && !legacy) return ESP_OK; /* lifecycle-1 command */
    return foreground_prepare_slot(path, &info, !info.foreground, app);
}

esp_err_t solar_os_native_foreground_launch(solar_os_context_t *ctx,
    const solar_os_app_t *app, int argc, char **argv)
{
    if (!ctx || !ctx->gfx || !app) return ESP_ERR_NOT_SUPPORTED;
    if (argc < 1 || argc > SOLAR_OS_APP_ARG_MAX) return ESP_ERR_INVALID_ARG;
    char cwd[sizeof(ctx->launch_cwd)] = {0};
    esp_err_t err = ctx->shell_session ?
        solar_os_shell_resolve_path(ctx, NULL, cwd, sizeof(cwd)) :
        solar_os_storage_resolve_path_at(NULL, NULL, cwd, sizeof(cwd));
    if (err != ESP_OK) return err;
    err = solar_os_context_request_launch(ctx, app, argc, argv);
    if (err == ESP_OK) memcpy(ctx->launch_cwd, cwd, sizeof(ctx->launch_cwd));
    return err;
}

const solar_os_app_registry_entry_t *solar_os_native_foreground_find(const char *id)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    const solar_os_app_t *app = NULL;
    if (solar_os_module_package_path(SOLAR_OS_MODULE_TYPE_APP, id, path, sizeof(path)) != ESP_OK ||
        solar_os_native_foreground_resolve(path, false, &app, NULL, 0U) != ESP_OK || !app) return NULL;
    foreground_slot_t *slot = foreground_find_slot(app);
    return slot && !strcmp(slot->manifest.id, id) ? &slot->registry : NULL;
}

bool solar_os_native_foreground_active(const char *id)
{
    bool active = false;
    portENTER_CRITICAL(&foreground_slots_lock);
    for (const foreground_slot_t *slot = foreground_slots; slot; slot = slot->next) {
        if (!strcmp(slot->manifest.id, id)) { active = atomic_load(&slot->active); break; }
    }
    portEXIT_CRITICAL(&foreground_slots_lock);
    return active;
}

const solar_os_app_registry_entry_t *solar_os_native_foreground_entry(const solar_os_app_t *app)
{
    foreground_slot_t *slot = foreground_find_slot(app);
    return slot ? &slot->registry : NULL;
}

bool solar_os_native_foreground_owner(const solar_os_app_t *app, char *owner, size_t size)
{
    foreground_slot_t *slot = foreground_find_slot(app);
    if (!slot) return false;
    portENTER_CRITICAL(&foreground_slots_lock);
    const bool claimed = slot->owner[0] != '\0';
    if (owner && size) strlcpy(owner, slot->owner, size);
    portEXIT_CRITICAL(&foreground_slots_lock);
    return claimed;
}

esp_err_t solar_os_native_foreground_claim(const solar_os_app_t *app,
    const char *owner, char *busy, size_t size)
{
    foreground_slot_t *slot = foreground_find_slot(app);
    if (!slot || !owner || !owner[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&foreground_slots_lock);
    if (slot->owner[0] && strcmp(slot->owner, owner)) {
        if (busy && size) strlcpy(busy, slot->owner, size);
        err = ESP_ERR_INVALID_STATE;
    } else strlcpy(slot->owner, owner, sizeof(slot->owner));
    portEXIT_CRITICAL(&foreground_slots_lock);
    return err;
}

void solar_os_native_foreground_release(const solar_os_app_t *app, const char *owner)
{
    foreground_slot_t *slot = foreground_find_slot(app);
    if (!slot || !owner) return;
    portENTER_CRITICAL(&foreground_slots_lock);
    if (!strcmp(slot->owner, owner)) slot->owner[0] = '\0';
    portEXIT_CRITICAL(&foreground_slots_lock);
}
