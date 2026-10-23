#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
extern "C" {
#include "solar_os_jet_app.h"
#include "solar_os_gfx.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include "solar_os_frame_presenter.h"
}

struct solar_os_gfx {
    unsigned width = 64, height = 48;
    uint32_t formats = SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT;
};
static solar_os_gfx_t gfx;
static std::atomic<int> allocations{0}, attempts{0}, fail_at{-1};
static std::atomic<unsigned> presented{0};
static std::atomic<bool> present_error{false};
static bool join_ok = true, create_ok = true;
static std::mutex image_mutex;
static std::vector<uint8_t> image;
struct jet_test_task { std::thread thread; };
struct solar_os_frame_presenter {
    solar_os_frame_presenter_config_t config{};
    uint8_t *buffers[3]{}, *writing = nullptr, *last = nullptr;
    unsigned index = 0;
    bool active = false;
    solar_os_frame_presenter_stats_t stats{};
    std::mutex mutex;
};

extern "C" {
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t, const char *) {
    if (attempts++ == fail_at.load()) return nullptr;
    void *storage = std::malloc(bytes);
    if (storage) ++allocations;
    return storage;
}
void solar_os_memory_free(void *storage) {
    if (storage) { --allocations; std::free(storage); }
}
int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int solar_os_task_create_pinned_internal(TaskFunction_t function, const char *,
    uint32_t stack, void *argument, unsigned, TaskHandle_t *handle, int, int role) {
    assert(stack == solar_os_jet_app.worker_stack_bytes && role == SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (!create_ok) return 0;
    *handle = new jet_test_task;
    (*handle)->thread = std::thread(function, argument);
    return pdPASS;
}
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *, uint32_t) {
    if (!join_ok) return false;
    task->thread.join(); delete task;
    return true;
}
void solar_os_task_delete_internal(TaskHandle_t) {}
void vTaskDelay(unsigned ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
const char *esp_err_to_name(esp_err_t) { return "error"; }
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
int solar_os_context_argc(const solar_os_context_t *ctx) { return ctx->argc; }
const char *solar_os_context_argv(const solar_os_context_t *ctx, int index) { return ctx->argv[index]; }
void solar_os_context_set_streaming_graphics_active(solar_os_context_t *ctx, bool active) { ctx->graphics_active = active; }
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active) { ctx->graphics_active = active; }
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *) {
    ctx->exit_requested = true; ctx->exit_code = code;
}
size_t solar_os_gfx_width(const solar_os_gfx_t *surface) { return surface->width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *surface) { return surface->height; }
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *surface, solar_os_display_format_t format) {
    return (surface->formats & SOLAR_OS_DISPLAY_FORMAT_BIT(format)) != 0;
}
}

// Capture the RGB producer side of the mocked presenter. Actual dithering,
// packed transport and target metadata are tested with the real presenter.
static esp_err_t capture_source_frame(const solar_os_display_raster_t *frame) {
    assert(frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565);
    assert(frame->source_width == gfx.width && frame->source_height == gfx.height);
    assert(frame->source_stride == gfx.width * 2);
    if (present_error) return ESP_FAIL;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::lock_guard<std::mutex> lock(image_mutex);
    image.assign(frame->data, frame->data + frame->data_size);
    ++presented;
    return ESP_OK;
}

extern "C" {
esp_err_t solar_os_frame_presenter_init(solar_os_frame_presenter_t **out,
    const solar_os_frame_presenter_config_t *config) {
    assert(!*out && config->format == SOLAR_OS_DISPLAY_FORMAT_RGB565);
    assert(config->allow_mono_fallback && config->literal_mono_palette);
    const bool direct = gfx.formats & SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT;
    assert(config->request_high_refresh == !direct);
    assert(config->preferred_fps == (direct ? 30 : 0));
    void *storage = solar_os_memory_alloc(sizeof(solar_os_frame_presenter_t),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "frame.presenter");
    if (!storage) return ESP_ERR_NO_MEM;
    auto *p = new (storage) solar_os_frame_presenter_t;
    p->config = *config;
    for (auto &buffer : p->buffers) {
        buffer = static_cast<uint8_t *>(solar_os_memory_alloc(
            size_t(config->stride) * config->height,
            SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "frame.buffer"));
        if (!buffer) { solar_os_frame_presenter_deinit(p); return ESP_ERR_NO_MEM; }
    }
    p->active = true; *out = p;
    return ESP_OK;
}
esp_err_t solar_os_frame_presenter_acquire(solar_os_frame_presenter_t *p, uint8_t **data, size_t *size) {
    p->writing = p->buffers[0]; *data = p->writing;
    *size = size_t(p->config.stride) * p->config.height;
    return ESP_OK;
}
static void mock_present(solar_os_frame_presenter_t *p, const uint8_t *data) {
    solar_os_display_raster_t frame{};
    frame.data = data; frame.data_size = size_t(p->config.stride) * p->config.height;
    frame.source_width = frame.width = p->config.width;
    frame.source_height = frame.height = p->config.height;
    frame.source_stride = p->config.stride; frame.format = p->config.format;
    const auto started = esp_timer_get_time();
    p->stats.last_error = capture_source_frame(&frame);
    if (p->stats.last_error == ESP_OK) {
        ++p->stats.presented_frames;
        p->stats.present_us += esp_timer_get_time() - started;
    }
}
esp_err_t solar_os_frame_presenter_exchange(solar_os_frame_presenter_t *p, uint8_t **data) {
    std::lock_guard<std::mutex> lock(p->mutex);
    assert(p->active && *data == p->writing);
    mock_present(p, p->writing);
    p->last = p->writing;
    p->index = (p->index + 1) % 3;
    p->writing = p->buffers[p->index]; *data = p->writing;
    assert(p->writing != p->last);
    return ESP_OK;
}
bool solar_os_frame_presenter_redraw(solar_os_frame_presenter_t *p) {
    std::lock_guard<std::mutex> lock(p->mutex);
    if (!p->active || !p->last) return false;
    mock_present(p, p->last); return true;
}
esp_err_t solar_os_frame_presenter_resume(solar_os_frame_presenter_t *p) {
    if (p->active) return ESP_OK;
    p->active = true; solar_os_frame_presenter_redraw(p);
    return ESP_OK;
}
esp_err_t solar_os_frame_presenter_suspend_checked(solar_os_frame_presenter_t *p) {
    if (!p) return ESP_OK;
    if (!join_ok) return ESP_ERR_INVALID_STATE;
    p->active = false; return ESP_OK;
}
esp_err_t solar_os_frame_presenter_deinit(solar_os_frame_presenter_t *p) {
    if (!p) return ESP_OK;
    assert(!p->active);
    for (auto buffer : p->buffers) solar_os_memory_free(buffer);
    p->~solar_os_frame_presenter_t(); solar_os_memory_free(p);
    return ESP_OK;
}
void solar_os_frame_presenter_take_stats(solar_os_frame_presenter_t *p,
    solar_os_frame_presenter_stats_t *stats) {
    *stats = {};
    if (!p) return;
    std::lock_guard<std::mutex> lock(p->mutex);
    *stats = p->stats; p->stats = {};
}
}

static solar_os_context_t prepare(const char *argument = nullptr) {
    assert(allocations == 0 && !*solar_os_jet_app.state_slot);
    *solar_os_jet_app.state_slot = std::calloc(1, solar_os_jet_app.state_size);
    solar_os_context_t ctx{};
    ctx.gfx = &gfx; ctx.argc = argument ? 2 : 1;
    std::strcpy(ctx.argv[0], "jet");
    if (argument) std::strcpy(ctx.argv[1], argument);
    attempts = 0; presented = 0; present_error = false;
    return ctx;
}
static void release(solar_os_context_t &ctx, bool started) {
    if (started) solar_os_jet_app.stop(&ctx);
    assert(solar_os_jet_app.state_release_ready());
    solar_os_jet_app.state_release_cleanup();
    std::free(*solar_os_jet_app.state_slot); *solar_os_jet_app.state_slot = nullptr;
    assert(allocations == 0);
}
static void tick(solar_os_context_t &ctx) {
    solar_os_event_t event{}; event.type = SOLAR_OS_EVENT_TICK;
    solar_os_jet_app.event(&ctx, &event);
}
static void character(solar_os_context_t &ctx, char ch) {
    solar_os_event_t event{}; event.type = SOLAR_OS_EVENT_CHAR; event.data.ch = ch;
    assert(solar_os_jet_app.event(&ctx, &event));
}

template<class F> static void wait_until(F condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static void wait_exit(solar_os_context_t &ctx) {
    wait_until([&] { tick(ctx); return ctx.exit_requested; });
}

static void test_lifecycle() {
    // Every managed allocation through construction and the first rendered
    // frame fails once. No partial scene survives any failed attempt.
    bool reached = false;
    for (int failure = 0; failure < 100; ++failure) {
        auto ctx = prepare("frames=1"); fail_at = failure;
        const auto result = solar_os_jet_app.start(&ctx);
        if (result == ESP_OK) wait_exit(ctx);
        else assert(result == ESP_ERR_NO_MEM);
        release(ctx, result == ESP_OK);
        reached = attempts <= failure;
        if (reached) break;
    }
    assert(reached); fail_at = -1;
    for (int cycle = 0; cycle < 3; ++cycle) {
        auto ctx = prepare("frames=50");
        assert(solar_os_jet_app.start(&ctx) == ESP_OK);
        wait_until([] { return presented >= 1; });
        character(ctx, ' ');
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const unsigned before_redraw = presented;
        solar_os_jet_app.resume(&ctx);
        wait_until([&] { return presented == before_redraw + 1; });
        assert(!ctx.exit_requested);
        const unsigned paused = presented;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(presented == paused);
        solar_os_jet_app.suspend(&ctx); tick(ctx); assert(presented == paused);
        solar_os_jet_app.resume(&ctx);
        wait_until([&] { return presented == paused + 1; });
        character(ctx, ' '); wait_exit(ctx);
        assert(presented == 52 && ctx.exit_code == 0);
        release(ctx, true);
    }
    for (const char *argument : {"frames=0", "frames=-1", "frames=1000001", "frames=abc", "bad"}) {
        auto ctx = prepare(argument);
        assert(solar_os_jet_app.start(&ctx) == ESP_ERR_INVALID_ARG);
        release(ctx, false);
    }
    auto ctx = prepare(); const auto formats = gfx.formats;
    gfx.formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT;
    assert(solar_os_jet_app.start(&ctx) == ESP_ERR_NOT_SUPPORTED);
    release(ctx, false); gfx.formats = formats;
    ctx = prepare(); assert(solar_os_jet_app.start(&ctx) == ESP_OK);
    present_error = true; wait_exit(ctx); assert(ctx.exit_code == 1);
    release(ctx, true);
    ctx = prepare(); assert(solar_os_jet_app.start(&ctx) == ESP_OK);
    character(ctx, 'q'); assert(ctx.exit_requested && ctx.exit_code == 0);
    release(ctx, true);

    ctx = prepare(); create_ok = false;
    assert(solar_os_jet_app.start(&ctx) == ESP_ERR_NO_MEM);
    release(ctx, false); create_ok = true;
    ctx = prepare(); assert(solar_os_jet_app.start(&ctx) == ESP_OK);
    join_ok = false;
    solar_os_jet_app.stop(&ctx);
    assert(!solar_os_jet_app.state_release_ready() && allocations > 0);
    join_ok = true; release(ctx, true);
}

int main(int argc, char **argv) {
    for (auto formats : {SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT,
                        SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT}) {
        gfx.formats = formats;
        test_lifecycle();
    }
    gfx.formats = SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT | SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    // Render the actual application scene at nova's output dimensions.
    gfx.width = 480; gfx.height = 320;
    auto ctx = prepare(); assert(solar_os_jet_app.start(&ctx) == ESP_OK);
    wait_until([] { return presented >= 20; });
    solar_os_jet_app.suspend(&ctx);
    assert(presented >= 20 && !ctx.exit_requested);
    size_t nonwhite = 0;
    for (size_t i = 0; i < image.size(); i += 2)
        nonwhite += image[i] != 0xff || image[i + 1] != 0xff;
    assert(nonwhite > 1000 && nonwhite < 480 * 320 / 2);
    if (argc == 2) {
        FILE *file = std::fopen(argv[1], "wb"); assert(file);
        std::fprintf(file, "P6\n480 320\n255\n");
        for (size_t i = 0; i < image.size(); i += 2) {
            unsigned pixel = (unsigned(image[i]) << 8) | image[i + 1];
            const uint8_t rgb[] = {uint8_t(((pixel >> 11) & 31) * 255 / 31),
                uint8_t(((pixel >> 5) & 63) * 255 / 63), uint8_t((pixel & 31) * 255 / 31)};
            assert(std::fwrite(rgb, 1, 3, file) == 3);
        }
        std::fclose(file);
    }
    release(ctx, true);
    // A reflective display retains the same renderer and depth at full size.
    gfx.width = 400; gfx.height = 300; gfx.formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    ctx = prepare("frames=20");
    assert(solar_os_jet_app.start(&ctx) == ESP_OK);
    wait_exit(ctx); assert(ctx.exit_code == 0 && presented == 20);
    assert(image.size() == 400 * 300 * 2);
    release(ctx, true);
}
