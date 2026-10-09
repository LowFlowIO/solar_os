#include "solar_os_native_vm.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "solar_os_config.h"
#include "solar_os_frame_presenter.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_native.h"
#include "solar_os_native_vm_abi.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
#include "solar_os_audio.h"
#endif

#define VM_STACK_BYTES (16U * 1024U)
#define VM_FILE_MAX (1024U * 1024U)

typedef struct {
    solar_os_gfx_t *gfx;
    bool keyboard_captured;
    QueueHandle_t keys;
    atomic_bool done, input_reset;
    solar_os_frame_presenter_t *presenter;
    uint16_t palette[256];
    size_t frame_size;
    int argc;
    char args[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char detail[160];
    esp_err_t result;
    int exit_code;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    solar_os_audio_stream_t *audio;
    solar_os_audio_stream_format_t audio_format;
#endif
} vm_state_t;

static void *vm_state;
/* Reject other tasks before touching foreground state that may be released. */
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("VM worker identity checked before accessing releasable state")
static _Atomic(TaskHandle_t) vm_worker_task;
#define vm (*(vm_state_t *)vm_state)

static bool vm_caller(void)
{
    return atomic_load(&vm_worker_task) == xTaskGetCurrentTaskHandle() &&
        vm_state != NULL && !atomic_load(&vm.done);
}

static int vm_result(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}

static uint64_t vm_time_us(void) { return (uint64_t)esp_timer_get_time(); }
static void vm_sleep_ms(uint32_t ms)
{
    if (vm_caller()) {
        if (ms > 1000U) ms = 1000U;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1U);
    }
}
static void vm_restart(void) { if (vm_caller()) esp_restart(); }

static int vm_video_open(uint16_t width, uint16_t height,
    const uint16_t *palette, uint16_t fps)
{
    if (!vm_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!width || !height || width > 1024U || height > 1024U ||
        !palette || fps > 60U) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (vm.presenter) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    memcpy(vm.palette, palette, sizeof(vm.palette));
    const solar_os_frame_presenter_config_t config = {
        .gfx = vm.gfx, .format = SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .width = width, .height = height, .stride = width,
        .palette_rgb565 = vm.palette, .palette_size = 256U,
        .fit = SOLAR_OS_FRAME_FIT_HEIGHT,
        .preferred_fps = fps, .allow_mono_fallback = true,
        .request_high_refresh = true, .clear_background_on_resume = true,
        .background_index = 15U,
    };
    esp_err_t err = solar_os_frame_presenter_init(&vm.presenter, &config);
    if (err == ESP_OK) err = solar_os_frame_presenter_resume(vm.presenter);
    if (err == ESP_OK) vm.frame_size = (size_t)width * height;
    return vm_result(err);
}

static int vm_present(const uint8_t *pixels, size_t length)
{
    if (!vm_caller() || !vm.presenter) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!pixels || length != vm.frame_size) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return solar_os_frame_presenter_submit(vm.presenter, pixels, length) ?
        SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
}

static int vm_read_key(solar_os_native_vm_key_v1_t *key, bool *available)
{
    if (!vm_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!key || !available) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(key, 0, sizeof(*key));
    if (atomic_exchange(&vm.input_reset, false)) {
        key->reset = true;
        *available = true;
    } else {
        *available = xQueueReceive(vm.keys, key, 0) == pdTRUE;
    }
    return SOLAR_OS_NATIVE_OK;
}

static bool vm_path_valid(const char *path)
{
    return path && path[0] == '/' &&
        strnlen(path, SOLAR_OS_STORAGE_PATH_MAX) < SOLAR_OS_STORAGE_PATH_MAX;
}

static int vm_file_size(const char *path, uint32_t *size)
{
    if (!vm_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!vm_path_valid(path) || !size) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_storage_metadata_t metadata;
    esp_err_t err = solar_os_storage_stat(path, &metadata);
    if (err == ESP_OK && (metadata.type != SOLAR_OS_STORAGE_ENTRY_FILE ||
        metadata.size_bytes > UINT32_MAX)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (err == ESP_OK) *size = (uint32_t)metadata.size_bytes;
    return vm_result(err);
}

static int vm_read_file(const char *path, uint32_t offset, void *buffer,
    size_t length, size_t *read_length)
{
    if (!vm_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!vm_path_valid(path) || !buffer || !read_length ||
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

static int vm_replace_file(const char *path, const void *buffer, size_t length)
{
    if (!vm_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!vm_path_valid(path) || (!buffer && length) || length > VM_FILE_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    char staged[SOLAR_OS_STORAGE_PATH_MAX], backup[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t err = solar_os_storage_sibling_path(path, ".vm.tmp", staged, sizeof(staged));
    if (err == ESP_OK) err = solar_os_storage_sibling_path(path, ".vm.bak", backup, sizeof(backup));
    if (err != ESP_OK) return vm_result(err);
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
    return vm_result(err);
}

static void vm_audio_close(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (vm_caller() && vm.audio) {
        solar_os_audio_stream_close(vm.audio);
        vm.audio = NULL;
    }
#endif
}

static int vm_audio_open(uint32_t *rate, uint8_t *channels)
{
    if (!vm_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!rate || !channels) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (vm.audio) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    esp_err_t err = solar_os_audio_stream_open("native.vm", 0, &vm.audio, &vm.audio_format);
    if (err == ESP_OK && (vm.audio_format.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
        vm.audio_format.bits_per_sample != 16U || vm.audio_format.channels > 2U ||
        !vm.audio_format.channels || !vm.audio_format.sample_rate)) {
        vm_audio_close();
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) { *rate = vm.audio_format.sample_rate; *channels = vm.audio_format.channels; }
    return vm_result(err);
#else
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int vm_audio_write(const int16_t *samples, size_t frames)
{
    if (!vm_caller()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO
    if (!vm.audio) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!samples || frames > 4096U) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return vm_result(solar_os_audio_stream_write(vm.audio, samples,
        frames * vm.audio_format.channels * sizeof(*samples)));
#else
    (void)samples; (void)frames;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static void vm_set_exit_code(int code) { if (vm_caller()) vm.exit_code = code; }

static const solar_os_native_vm_api_v1_t vm_api = {
    .abi_version = 1U, .struct_size = sizeof(vm_api),
    .time_us = vm_time_us, .sleep_ms = vm_sleep_ms, .restart = vm_restart,
    .video_open = vm_video_open, .present = vm_present, .read_key = vm_read_key,
    .file_size = vm_file_size, .read_file = vm_read_file, .replace_file = vm_replace_file,
    .audio_open = vm_audio_open, .audio_write = vm_audio_write, .audio_close = vm_audio_close,
    .set_exit_code = vm_set_exit_code,
};

const void *solar_os_native_vm_get_service(const char *name, uint32_t abi, uint32_t size)
{
    return name && !strcmp(name, SOLAR_OS_NATIVE_VM_SERVICE) &&
        abi == SOLAR_OS_NATIVE_VM_ABI && size <= sizeof(vm_api) && vm_caller() ? &vm_api : NULL;
}

static esp_err_t vm_write(const char *text, size_t length, void *user)
{
    (void)user;
    if (!text) return ESP_ERR_INVALID_ARG;
    if (length >= sizeof(vm.detail)) length = sizeof(vm.detail) - 1U;
    memcpy(vm.detail, text, length);
    vm.detail[length] = '\0';
    return ESP_OK;
}

static void vm_worker(void *arg)
{
    (void)arg;
    /* Set on the worker itself: task creation may schedule before it returns. */
    atomic_store(&vm_worker_task, xTaskGetCurrentTaskHandle());
    char *argv[SOLAR_OS_APP_ARG_MAX];
    for (int i = 0; i < vm.argc; ++i) argv[i] = vm.args[i];
    const solar_os_native_run_options_t options = {.write = vm_write};
    char detail[160];
    vm.result = solar_os_native_run(argv[0], vm.argc, argv, &options, NULL, detail, sizeof(detail));
    if (vm.result != ESP_OK && detail[0]) strlcpy(vm.detail, detail, sizeof(vm.detail));
    if (vm.result == ESP_OK && vm.exit_code != 0) vm.result = ESP_FAIL;
    vm_audio_close();
    if (vm.presenter) {
        /* Deinit joins the presenter before its host state can be released. */
        if (solar_os_frame_presenter_deinit(vm.presenter) != ESP_OK) esp_restart();
        vm.presenter = NULL;
    }
    atomic_store(&vm_worker_task, NULL);
    atomic_store(&vm.done, true);
    solar_os_task_delete_internal(NULL);
}

static esp_err_t vm_start(solar_os_context_t *ctx)
{
    atomic_init(&vm.done, true);
    atomic_init(&vm.input_reset, false);
    if (!ctx->gfx || ctx->argc < 1) return ESP_ERR_NOT_SUPPORTED;
    vm.gfx = ctx->gfx;
    vm.argc = ctx->argc;
    memcpy(vm.args, ctx->argv, sizeof(vm.args));
    atomic_store(&vm.done, false);
    vm.exit_code = 1;
    vm.keys = xQueueCreate(64U, sizeof(solar_os_native_vm_key_v1_t));
    if (!vm.keys) { atomic_store(&vm.done, true); return ESP_ERR_NO_MEM; }
    solar_os_context_set_streaming_graphics_active(ctx, true);
    if (solar_os_task_create_pinned_internal(vm_worker, "native_vm", VM_STACK_BYTES,
        NULL, tskIDLE_PRIORITY + 1U, NULL, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        atomic_store(&vm.done, true);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void vm_stop(solar_os_context_t *ctx)
{
    if (!atomic_load(&vm.done)) esp_restart();
    solar_os_input_release_keyboard(vm_state);
    solar_os_context_set_streaming_graphics_active(ctx, false);
}

static void vm_suspend(solar_os_context_t *ctx) { vm_stop(ctx); }
static bool vm_ready(void) { return vm_state == NULL || atomic_load(&vm.done); }
static void vm_cleanup(void)
{
    solar_os_input_release_keyboard(vm_state);
    if (vm.keys) vQueueDelete(vm.keys);
}

static void vm_queue_key(solar_os_native_vm_key_v1_t *key)
{
    if (key->key == SOLAR_OS_KEY_APP_EXIT || key->key == 0x1dU) {
        esp_restart();
        return;
    }
    if (!atomic_load(&vm.done) && xQueueSend(vm.keys, key, 0) != pdTRUE) {
        xQueueReset(vm.keys);
        atomic_store(&vm.input_reset, true);
    }
}

static solar_os_native_vm_key_v1_t vm_key(const solar_os_input_key_event_t *key)
{
    return (solar_os_native_vm_key_v1_t){
        .source = key->source, .physical_key = key->physical_key,
        .usage = key->usage, .key = key->key,
        .modifiers = key->modifiers, .action = (uint8_t)key->action,
    };
}

static void vm_poll_keyboard(void)
{
    if (!vm.keyboard_captured) {
        for (size_t i = 0; i < solar_os_input_source_count(); ++i) {
            solar_os_input_source_info_t source;
            if (solar_os_input_source_get(i, &source) && source.ready &&
                source.source_class == SOLAR_OS_INPUT_SOURCE_KEYBOARD &&
                (source.capabilities & SOLAR_OS_INPUT_CAP_KEY_EVENTS) &&
                solar_os_input_capture_keyboard(source.name, vm_state) == ESP_OK) {
                vm.keyboard_captured = true;
                break;
            }
        }
    }
    for (unsigned i = 0; i < 64U; ++i) {
        solar_os_input_key_event_t event = {0};
        bool reset;
        if (!solar_os_input_read_captured_key(vm_state, &event, &reset)) break;
        if (reset) {
            xQueueReset(vm.keys);
            atomic_store(&vm.input_reset, true);
        } else {
            solar_os_native_vm_key_v1_t key = vm_key(&event);
            vm_queue_key(&key);
        }
    }
}

static bool vm_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (atomic_load(&vm.done)) solar_os_context_finish(ctx,
            vm.result == ESP_OK ? 0 : 1, vm.detail[0] ? vm.detail : "vm: module returned");
        else vm_poll_keyboard();
        return true;
    }
    solar_os_native_vm_key_v1_t key = {0};
    if (event->type == SOLAR_OS_EVENT_KEY) {
        key = vm_key(&event->data.key);
    } else if (event->type == SOLAR_OS_EVENT_CHAR) key.key = (uint8_t)event->data.ch;
    else return false;
    vm_queue_key(&key);
    return true;
}

static const solar_os_app_t vm_app = {
    .name = "vm", .summary = "external virtual machine", .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_KEY_EVENTS,
    .start = vm_start, .stop = vm_stop, .suspend = vm_suspend, .event = vm_event,
    .state_slot = &vm_state, .state_size = sizeof(vm_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_REQUIRED,
    .state_release_ready = vm_ready, .state_release_cleanup = vm_cleanup,
    .worker_stack_bytes = VM_STACK_BYTES, .tick_interval_ms = 5U,
};

esp_err_t solar_os_native_vm_launch(solar_os_context_t *ctx, int argc, char **argv)
{
    if (!ctx || !ctx->gfx) return ESP_ERR_NOT_SUPPORTED;
    if (argc < 1 || argc > SOLAR_OS_APP_ARG_MAX) return ESP_ERR_INVALID_ARG;
    return solar_os_context_request_launch(ctx, &vm_app, argc, argv);
}
