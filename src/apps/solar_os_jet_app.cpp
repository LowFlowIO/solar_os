#include <new>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <atomic>

extern "C" {
#include "solar_os_jet_app.h"
#include "solar_os_gfx.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#include "solar_os_log.h"
#include "solar_os_task.h"
#include "solar_os_frame_presenter.h"
}
#include "Scene.hpp"
#include "Primitives.hpp"

namespace {
using namespace Renderer;
static_assert(JET_FRAMEBUFFER_SWAP_BYTES == 1, "SolarOS direct frames require RGB565 wire byte order");
constexpr uint32_t worker_stack = 8192;

struct JetScene {
    Camera camera;
    AmbientLight ambient{Color{90, 90, 90}};
    DirectionalLight light{Vector3{35, 40, 0}, Color{255, 245, 230}, 220};
    Material blue{uint16_t(0x249f)}, orange{uint16_t(0xfbe0)};
    std::unique_ptr<Object> cube, small;
    Scene scene;

    JetScene(uint16_t *color, uint16_t *depth, int width, int height)
        : scene(color, depth, width, height) {
        scene.setBackcolor(0xffff);
        scene.setClearBuffer(true);
        camera.setPosition(0, 0, -500);
        camera.setFOV(int32_t(60), int32_t(width));
        camera.nearPlane = 16;
        camera.farPlane = 4096;
        scene.setCamera(&camera);
        scene.setAmbientLight(&ambient);
        scene.setDirectionalLight(&light);
        blue.shadingMode = ShadingMode::GOURAUD;
        orange.shadingMode = ShadingMode::GOURAUD;
        cube.reset(Primitives::createCube(150, 150, 150, &blue));
        small.reset(Primitives::createCube(80, 80, 80, &orange));
        cube->setPosition(-55, 0, 100);
        small->setPosition(100, 35, 30);
        scene.addObject(cube.get());
        scene.addObject(small.get());
    }
};

struct JetState {
    solar_os_gfx_t *gfx;
    JetScene *renderer;
    solar_os_frame_presenter_t *presenter;
    uint16_t *color, *depth;
    uint16_t width, height;
    uint32_t frames, limit;
    uint64_t render_us, present_us;
    uint64_t frame_started_us, prepare_us, raster_us;
    uint64_t started_us;
    uint32_t displayed, replaced;
    TaskHandle_t task;
    volatile bool task_done;
    std::atomic<bool> stop_requested{false}, finished{false}, paused{false};
    esp_err_t error;
    esp_err_t presentation_error;
    bool suspended;
};

static void *jet_state;
#define state (*(JetState *)jet_state)

static bool release_ready() {
    if (!jet_state) return true;
    state.stop_requested.store(true);
    if (state.task && !solar_os_task_wait_done(state.task, &state.task_done,
        SOLAR_OS_TASK_STOP_WAIT_MS)) return false;
    state.task = nullptr;
    return solar_os_frame_presenter_suspend_checked(state.presenter) == ESP_OK;
}

static void cleanup() {
    if (!jet_state) return;
    if (state.renderer) {
        state.renderer->~JetScene();
        solar_os_memory_free(state.renderer);
        state.renderer = nullptr;
    }
    if (state.presenter) {
        (void)solar_os_frame_presenter_deinit(state.presenter);
        state.presenter = nullptr;
    }
    state.color = nullptr;
    solar_os_memory_free(state.depth); state.depth = nullptr;
}

static void collect_stats() {
    solar_os_frame_presenter_stats_t stats{};
    solar_os_frame_presenter_take_stats(state.presenter, &stats);
    state.present_us += stats.present_us;
    state.displayed += stats.presented_frames;
    state.replaced += stats.replaced_frames;
    if (stats.last_error != ESP_OK) state.presentation_error = stats.last_error;
}

static void rasterize(Scene &scene) {
    const int64_t started = esp_timer_get_time();
    state.prepare_us += started - state.frame_started_us;
    scene.rasterizeBand(0, state.height);
    state.raster_us += esp_timer_get_time() - started;
}

static esp_err_t render_frame() {
    const int64_t started = esp_timer_get_time();
    state.frame_started_us = started;
    state.renderer->cube->setRotation(state.frames % 360, (state.frames * 2) % 360, 15);
    state.renderer->small->setRotation(25, (360 - state.frames % 360) % 360, state.frames % 360);
#if defined(__cpp_exceptions)
    try {
        state.renderer->scene.render(rasterize);
    } catch (const std::bad_alloc &) {
        return ESP_ERR_NO_MEM;
    }
#else
    state.renderer->scene.render(rasterize);
#endif
    const int64_t rendered = esp_timer_get_time();
    uint8_t *next = reinterpret_cast<uint8_t *>(state.color);
    const esp_err_t error = solar_os_frame_presenter_exchange(state.presenter, &next);
    if (error == ESP_OK) {
        state.render_us += rendered - started;
        state.color = reinterpret_cast<uint16_t *>(next);
        state.renderer->scene.setFramebuffer(state.color);
        ++state.frames;
    }
    return error;
}

// Render only into the acquired writing buffer. Exchange transfers ownership
// without copying pixels, so the shared presenter can scan out another buffer
// concurrently. Join the producer before suspending/releasing the presenter.
static void render_worker(void *) {
    while (!state.stop_requested.load() && state.error == ESP_OK &&
        (!state.limit || state.frames < state.limit)) {
        if (state.paused.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        state.error = render_frame();
        vTaskDelay(1);
    }
    if (state.error != ESP_OK)
        SOLAR_OS_LOGE("solar_os_jet", "render/present: %s", esp_err_to_name(state.error));
    state.finished.store(true, std::memory_order_release);
    state.task_done = true;
    solar_os_task_delete_internal(nullptr);
}

static esp_err_t start_worker() {
    state.stop_requested.store(false);
    state.finished.store(false);
    state.task_done = false;
    return solar_os_task_create_pinned_internal(render_worker, "jet_render", worker_stack,
        nullptr, tskIDLE_PRIORITY + 1, &state.task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t start(solar_os_context_t *ctx) {
    new (jet_state) JetState{};
    state.gfx = solar_os_context_gfx(ctx);
    if (!state.gfx || !solar_os_gfx_supports_frame_format(state.gfx, SOLAR_OS_DISPLAY_FORMAT_RGB565))
        return ESP_ERR_NOT_SUPPORTED;
    if (solar_os_context_argc(ctx) > 2) return ESP_ERR_INVALID_ARG;
    if (solar_os_context_argc(ctx) == 2) {
        const char *arg = solar_os_context_argv(ctx, 1);
        if (!arg || strncmp(arg, "frames=", 7) || !arg[7]) return ESP_ERR_INVALID_ARG;
        errno = 0;
        char *end;
        unsigned long value = strtoul(arg + 7, &end, 10);
        if (errno || *end || value < 1 || value > 1000000) return ESP_ERR_INVALID_ARG;
        state.limit = value;
    }
    size_t width = solar_os_gfx_width(state.gfx), height = solar_os_gfx_height(state.gfx);
    // Full-width RGB565 + matching per-pixel depth. Avoid upstream packed-row
    // alignment assumptions; no resizing, interlace or conversion buffer.
    if (!width || !height || width % 8 || width > 1024 || height > 1024)
        return ESP_ERR_INVALID_SIZE;
    state.width = width; state.height = height;
    size_t bytes = width * height * sizeof(uint16_t);
    solar_os_frame_presenter_config_t config{};
    config.gfx = state.gfx;
    config.format = SOLAR_OS_DISPLAY_FORMAT_RGB565;
    config.width = width; config.height = height; config.stride = width * 2;
    config.fit = SOLAR_OS_FRAME_FIT_HEIGHT;
    config.preferred_fps = 30;
    config.clear_background_on_resume = true;
    config.background_rgb565 = 0xffff;
    esp_err_t error = solar_os_frame_presenter_init(&state.presenter, &config);
    if (error != ESP_OK) return error;
    uint8_t *writing = nullptr;
    size_t capacity = 0;
    error = solar_os_frame_presenter_acquire(state.presenter, &writing, &capacity);
    if (error != ESP_OK) return error;
    if (capacity != bytes) return ESP_ERR_INVALID_SIZE;
    state.color = reinterpret_cast<uint16_t *>(writing);
    state.depth = static_cast<uint16_t *>(solar_os_memory_alloc(bytes,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "jet.depth"));
    void *storage = solar_os_memory_alloc(sizeof(JetScene),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "jet.scene");
    if (!state.color || !state.depth || !storage) {
        solar_os_memory_free(storage);
        return ESP_ERR_NO_MEM;
    }
#if defined(__cpp_exceptions)
    try {
        state.renderer = new (storage) JetScene(state.color, state.depth, width, height);
    } catch (const std::bad_alloc &) {
        solar_os_memory_free(storage);
        return ESP_ERR_NO_MEM;
    }
#else
    state.renderer = new (storage) JetScene(state.color, state.depth, width, height);
#endif
    solar_os_context_set_streaming_graphics_active(ctx, true);
    SOLAR_OS_LOGI("solar_os_jet", "%ux%u RGB565-BE color=3x%u depth=%u exchange=direct",
        state.width, state.height, unsigned(bytes), unsigned(bytes));
    state.started_us = esp_timer_get_time();
    return start_worker();
}

static void stop(solar_os_context_t *ctx) {
    if (!release_ready()) return;
    collect_stats();
    if (state.frames) {
        SOLAR_OS_LOGI("solar_os_jet", "rendered=%lu displayed=%lu skipped=%lu render=%llu present=%llu us elapsed=%llu ms",
            static_cast<unsigned long>(state.frames),
            static_cast<unsigned long>(state.displayed),
            static_cast<unsigned long>(state.replaced),
            static_cast<unsigned long long>(state.render_us / state.frames),
            static_cast<unsigned long long>(state.displayed ? state.present_us / state.displayed : 0),
            static_cast<unsigned long long>((esp_timer_get_time() - state.started_us) / 1000));
        SOLAR_OS_LOGI("solar_os_jet", "prepare=%llu raster=%llu us (mean)",
            static_cast<unsigned long long>(state.prepare_us / state.frames),
            static_cast<unsigned long long>(state.raster_us / state.frames));
    }
    cleanup();
    solar_os_context_set_graphics_active(ctx, false);
}
static void suspend(solar_os_context_t *ctx) {
    state.suspended = true;
    if (!release_ready())
        solar_os_context_finish(ctx, 1, "jet: rendering worker did not stop");
}
static void resume(solar_os_context_t *ctx) {
    state.suspended = false;
    // Runtime also calls resume for active-view redraws. Only the worker may
    // read the live framebuffer; request its repaint without restarting it.
    if (state.task) {
        (void)solar_os_frame_presenter_redraw(state.presenter);
        return;
    }
    if (solar_os_frame_presenter_resume(state.presenter) != ESP_OK || start_worker() != ESP_OK)
        solar_os_context_finish(ctx, 1, "jet: could not resume rendering worker");
}
static bool event(solar_os_context_t *ctx, const solar_os_event_t *event) {
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        if (event->data.ch == 'q') solar_os_context_finish(ctx, 0, nullptr);
        else if (event->data.ch == ' ') state.paused.store(!state.paused.load());
        else return false;
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_TICK || state.suspended) return false;
    collect_stats();
    if (state.presentation_error != ESP_OK) {
        solar_os_context_finish(ctx, 1, "jet: display presentation failed");
        return true;
    }
    if (!state.finished.load(std::memory_order_acquire)) return false;
    solar_os_context_finish(ctx, state.error == ESP_OK ? 0 : 1,
        state.error == ESP_OK ? nullptr : "jet: rendering or presentation failed");
    return true;
}
} // namespace

// Field order follows solar_os_app_t for C++17 aggregate initialization.
extern "C" const solar_os_app_t solar_os_jet_app = {
    .name = "jet", .summary = "Jet 3D renderer demo",
    .app_class = SOLAR_OS_APP_CLASS_GUI, .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = start, .suspend = suspend, .resume = resume, .stop = stop, .event = event,
    .title = nullptr, .state_slot = &jet_state, .state_size = sizeof(JetState),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_REQUIRED,
    .state_release_ready = release_ready, .state_release_cleanup = cleanup,
    .worker_stack_bytes = worker_stack, .worker_stack_external = false,
    .tick_interval_ms = 25, .tick_deadline_ms = 25,
};
