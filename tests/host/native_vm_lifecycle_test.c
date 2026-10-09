#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_native_vm_abi.h"
#include "solar_os_keys.h"
#include "solar_os_display_surface.h"
#include "esp_err.h"

typedef void *TaskHandle_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY -1
#define SOLAR_OS_TASK_ROLE_FOREGROUND 1
#define VM_STACK_BYTES (16U * 1024U)
#define SOLAR_OS_STORAGE_PATH_MAX 160
#define SOLAR_OS_STORAGE_MOUNT_POINT_MAX 32
#define pdMS_TO_TICKS(ms) ((ms) / 10U)
#define SOLAR_OS_TASK_STOP_POLL_MS 20U
#define SOLAR_OS_TASK_REAP_WAIT_MS 100U
#define SOLAR_OS_APP_ARG_MAX 8
#define SOLAR_OS_EVENT_TICK 0
#define SOLAR_OS_EVENT_KEY 1
#define SOLAR_OS_EVENT_CHAR 2
typedef struct {
    bool streaming, finished;
    int result, argc;
    void *gfx, *shell_session;
    char argv[8][160], launch_cwd[160];
} solar_os_context_t;
typedef struct {
    unsigned type;
    union { solar_os_native_vm_key_v1_t key; uint8_t ch; } data;
} solar_os_event_t;
typedef struct { int (*write)(const char *, size_t, void *); } solar_os_native_run_options_t;
typedef struct {
    atomic_bool done, stop_requested, input_reset;
    uint64_t stop_deadline_us;
    bool worker_started;
    void *presenter, *keys, *gfx;
    int argc, exit_code;
    char args[8][160], detail[160], cwd[160];
    esp_err_t result;
} vm_state_t;
static vm_state_t state;
static void *vm_state;
#define vm (*(vm_state_t *)vm_state)
static _Atomic(TaskHandle_t) vm_worker_task;
static TaskHandle_t current_task;
static int worker_token, main_token;
static uint64_t clock_us;
static unsigned sequence, unload_order, audio_order, presenter_order, delete_order, release_order;
static unsigned sent_keys, restarts;
static bool finish_during_wait, fail_presenter;
static jmp_buf restarted;
static const int vm_app;
static const char *shell_cwd = "/sdcard";
static bool reject_launch;
static size_t strlcpy(char *dst, const char *src, size_t size);
static const char *storage_default_base_path(void) { return "/sdcard"; }
static const char *mount_prefix(const char *path)
{
    static const char *const mounts[] = {"/sdcard", "/flash", "/ram"};
    if (path) for (unsigned i = 0; i < 3; ++i) {
        size_t n = strlen(mounts[i]);
        if (!strncmp(path, mounts[i], n) && (path[n] == '/' || path[n] == 0)) return mounts[i];
    }
    return NULL;
}
static bool solar_os_storage_path_has_mount_prefix(const char *path) { return mount_prefix(path) != NULL; }
static int solar_os_storage_path_mount_point(const char *path, char *out, size_t size)
{
    const char *root = mount_prefix(path);
    if (!root) return ESP_ERR_NOT_FOUND;
    return strlcpy(out, root, size) >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
static int solar_os_storage_resolve_path_at(const char *cwd, const char *arg, char *path, size_t size);
static int solar_os_shell_resolve_path(solar_os_context_t *ctx, const char *arg, char *path, size_t size)
{ assert(ctx->shell_session); return solar_os_storage_resolve_path_at(shell_cwd, arg, path, size); }
static int solar_os_context_request_launch(solar_os_context_t *ctx, const void *app, int argc, char **argv)
{
    assert(app == &vm_app);
    if (reject_launch) return ESP_ERR_INVALID_STATE;
    ctx->argc = argc; ctx->launch_cwd[0] = 0;
    for (int i = 0; i < argc; ++i) assert(strlcpy(ctx->argv[i], argv[i], sizeof(ctx->argv[i])) < sizeof(ctx->argv[i]));
    return ESP_OK;
}
static bool target_found = true, theme_inverted;
static uint32_t target_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT;
static unsigned color_reads;
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16U
typedef struct { uint32_t frame_formats; } solar_os_display_target_t;
static bool solar_os_gfx_display_target_name(void *gfx, char *name, size_t size)
{ (void)gfx; assert(size >= 9U); strcpy(name, "display0"); return true; }
static bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target)
{ assert(!strcmp(name, "display0")); target->frame_formats = target_formats; return target_found; }
static int solar_os_display_get_colors(uint32_t *fg, uint32_t *bg)
{ color_reads++; *fg = 0x00ff00U; *bg = 0x123456U; return ESP_OK; }
static bool solar_os_gfx_palette_inverted(void *gfx) { (void)gfx; return theme_inverted; }

static bool vm_stop_requested(void);
static void vm_worker(void *arg);
static bool vm_ready(void);
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
static uint64_t vm_time_us(void) { return clock_us; }
static size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) { size_t copy = n < size - 1 ? n : size - 1; memcpy(dst, src, copy); dst[copy] = 0; }
    return n;
}
static int vm_write(const char *text, size_t size, void *user)
{ (void)text; (void)size; (void)user; return ESP_OK; }
static int solar_os_native_run(const char *path, int argc, char **argv,
    const solar_os_native_run_options_t *options, void *unused, char *detail, size_t size)
{
    (void)path; (void)argc; (void)argv; (void)options; (void)unused;
    assert(size && !vm_ready());
    assert(!vm_stop_requested() || atomic_load(&vm.stop_requested));
    detail[0] = 0;
    vm.exit_code = 0;
    unload_order = ++sequence;
    return ESP_OK;
}
static void vm_audio_close(void) { assert(!vm_ready()); audio_order = ++sequence; }
static int solar_os_frame_presenter_deinit(void *presenter)
{
    assert(presenter && !vm_ready()); presenter_order = ++sequence;
    return fail_presenter ? ESP_FAIL : ESP_OK;
}
static void solar_os_task_delete_internal(TaskHandle_t task)
{
    assert(!task && vm_ready() && !atomic_load(&vm_worker_task)); delete_order = ++sequence;
}
static void complete_worker(void)
{
    current_task = &worker_token;
    vm_worker(NULL);
    current_task = &main_token;
}
static void vTaskDelay(TickType_t ticks)
{
    assert(ticks); clock_us += ticks * 10000U;
    if (finish_during_wait && !atomic_load(&vm.done)) complete_worker();
}
static void esp_restart(void) { restarts++; longjmp(restarted, 1); }
static void solar_os_input_release_keyboard(void *owner)
{ assert(owner == vm_state && vm_ready()); release_order = ++sequence; }
static void solar_os_context_set_streaming_graphics_active(solar_os_context_t *ctx, bool active)
{ assert(active ? !vm_ready() : vm_ready()); ctx->streaming = active; }
static void *xQueueCreate(unsigned length, size_t item_size)
{ assert(length == 64 && item_size == sizeof(solar_os_native_vm_key_v1_t)); return &worker_token; }
static int solar_os_task_create_pinned_internal(void (*task)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle, int core, int role)
{
    assert(task == vm_worker && !strcmp(name, "native_vm") && stack == VM_STACK_BYTES);
    assert(!arg && priority == 1 && !handle && core == -1 && role == 1); return pdPASS;
}
static int xQueueSend(void *queue, const void *key, unsigned timeout)
{ (void)queue; (void)key; (void)timeout; sent_keys++; return pdTRUE; }
static void xQueueReset(void *queue) { (void)queue; }
static void vm_poll_keyboard(void) {}
static solar_os_native_vm_key_v1_t vm_key(const solar_os_native_vm_key_v1_t *key) { return *key; }
static void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *detail)
{ assert(vm_ready() && detail); ctx->finished = true; ctx->result = code; }

#include "vm_lifecycle.inc"

static void setup(solar_os_context_t *ctx)
{
    memset(&state, 0, sizeof(state)); vm_state = &state;
    atomic_init(&vm.done, false); atomic_init(&vm.stop_requested, false);
    atomic_init(&vm.input_reset, false); atomic_store(&vm_worker_task, NULL);
    current_task = &main_token; clock_us = 0;
    vm.presenter = &state; vm.worker_started = true;
    *ctx = (solar_os_context_t){.streaming = true};
    sequence = unload_order = audio_order = presenter_order = delete_order = release_order = 0;
    finish_during_wait = fail_presenter = false; sent_keys = 0;
}

int main(void)
{
    solar_os_context_t ctx;
    solar_os_event_t exit = {.type = SOLAR_OS_EVENT_KEY,
        .data.key = {.key = SOLAR_OS_KEY_APP_EXIT, .action = SOLAR_OS_NATIVE_VM_KEY_PRESS}};
    solar_os_event_t tick = {.type = SOLAR_OS_EVENT_TICK};
    for (unsigned run = 0; run < 20; ++run) {
        setup(&ctx);
        /* Release alone must not request shutdown. Repeat must not extend it. */
        exit.data.key.action = SOLAR_OS_NATIVE_VM_KEY_RELEASE;
        vm_event(&ctx, &exit); assert(!atomic_load(&vm.stop_requested));
        exit.data.key.action = SOLAR_OS_NATIVE_VM_KEY_PRESS;
        vm_event(&ctx, &exit);
        assert(atomic_load(&vm.stop_requested) && !ctx.finished && !sent_keys && !restarts);
        uint64_t deadline = vm.stop_deadline_us;
        clock_us = 1000000U; vm_event(&ctx, &exit); assert(vm.stop_deadline_us == deadline);
        complete_worker(); vm_event(&ctx, &tick);
        assert(ctx.finished && !ctx.result);
        vm_stop(&ctx);
        assert(!ctx.streaming && release_order > delete_order);
        assert(unload_order < audio_order && audio_order < presenter_order && presenter_order < delete_order);
        assert(clock_us == 1100000U);
    }
    setup(&ctx); finish_during_wait = true;
    vm_stop(&ctx); /* Session close waits rather than freeing active state. */
    assert(vm_ready() && release_order > presenter_order && !ctx.streaming);
    setup(&ctx);
    solar_os_native_vm_key_v1_t alias = {.key = 0x1d};
    vm_queue_key(&alias); assert(atomic_load(&vm.stop_requested));
    clock_us = 4999999U; vm_event(&ctx, &tick); assert(!restarts);
    clock_us++;
    if (!setjmp(restarted)) { vm_event(&ctx, &tick); assert(!"timeout must restart"); }
    assert(restarts == 1 && !vm_ready() && !release_order && ctx.streaming);
    setup(&ctx);
    if (!setjmp(restarted)) { vm_stop(&ctx); assert(!"close timeout must restart"); }
    assert(restarts == 2 && clock_us >= 5000000U && !release_order && ctx.streaming);
    setup(&ctx); fail_presenter = true;
    if (!setjmp(restarted)) { complete_worker(); assert(!"presenter join failure must restart"); }
    assert(restarts == 3 && !vm_ready() && !delete_order && !release_order);
    current_task = &main_token; vm_state = NULL;
    assert(vm_stop_requested()); /* Foreign callers never dereference released state. */
    setup(&ctx);
    uint16_t fg, bg;
    assert(vm_text_colors(&fg, &bg) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    current_task = &worker_token; atomic_store(&vm_worker_task, current_task);
    assert(vm_text_colors(NULL, &bg) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(vm_text_colors(&fg, &bg) == 0 && fg == 0x07e0U && bg == 0x11aaU);
    theme_inverted = true;
    assert(vm_text_colors(&fg, &bg) == 0 && fg == 0x11aaU && bg == 0x07e0U);
    unsigned reads = color_reads;
    target_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    assert(vm_text_colors(&fg, &bg) == 0 && fg == 0 && bg == 0xffffU);
    assert(color_reads == reads); /* Monochrome is independent of color theme. */
    target_found = false;
    assert(vm_text_colors(&fg, &bg) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    setup(&ctx); ctx.gfx = &state; ctx.shell_session = &ctx;
    char *args[8] = {"/sdcard/modules/vms/apple2.vm.elf", "vm/apple2", "disk.nib", "--flag", "a", "b", "c", "d"};
    assert(solar_os_native_vm_launch(&ctx, 8, args) == ESP_OK);
    assert(ctx.argc == 8 && !strcmp(ctx.launch_cwd, "/sdcard"));
    for (unsigned i = 0; i < 8; ++i) assert(!strcmp(ctx.argv[i], args[i]));
    shell_cwd = "/flash"; ctx.shell_session = NULL;
    vm.presenter = NULL;
    assert(vm_start(&ctx) == ESP_OK && !strcmp(vm.cwd, "/sdcard"));
    ctx.launch_cwd[0] = 0; /* Later shell/app changes cannot affect the worker. */
    current_task = &worker_token; atomic_store(&vm_worker_task, current_task);
    char resolved[160];
    assert(vm_resolve_path("vm/apple2", resolved, sizeof(resolved)) == 0 && !strcmp(resolved, "/sdcard/vm/apple2"));
    assert(vm_resolve_path("/vm/apple2", resolved, sizeof(resolved)) == 0 && !strcmp(resolved, "/sdcard/vm/apple2"));
    strcpy(vm.cwd, "/sdcard/vm/apple2");
    assert(vm_resolve_path(".", resolved, sizeof(resolved)) == 0 && !strcmp(resolved, vm.cwd));
    assert(vm_resolve_path("../apple2/./", resolved, sizeof(resolved)) == 0 && !strcmp(resolved, vm.cwd));
    assert(vm_resolve_path("/flash/vm/apple2", resolved, sizeof(resolved)) == 0 && !strcmp(resolved, "/flash/vm/apple2"));
    assert(vm_resolve_path("../../../../", resolved, sizeof(resolved)) == 0 && !strcmp(resolved, "/sdcard"));
    assert(vm_resolve_path(NULL, resolved, sizeof(resolved)) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(vm_resolve_path(".", resolved, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    current_task = &main_token;
    assert(vm_resolve_path(".", resolved, sizeof(resolved)) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    reject_launch = true;
    assert(solar_os_native_vm_launch(&ctx, 8, args) == ESP_ERR_INVALID_STATE);
    assert(!ctx.launch_cwd[0]);
    reject_launch = false;
    assert(solar_os_native_vm_launch(&ctx, 8, args) == 0 && !strcmp(ctx.launch_cwd, "/sdcard"));
    assert(solar_os_native_vm_launch(&ctx, 9, args) == ESP_ERR_INVALID_ARG);
    puts("Native VM lifecycle, colors and captured working directory tests passed");
    return 0;
}
