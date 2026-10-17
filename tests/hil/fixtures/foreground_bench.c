// SPDX-License-Identifier: MIT
#include <stddef.h>
#include <stdint.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include "solar_os_native_service_abi.h"

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_POINTER_EVENTS,
    .worker_stack_bytes = 8192, .worker_priority = 1,
    .minimum_internal_bytes = 4096, .minimum_psram_bytes = 512 * 1024,
    .tick_interval_ms = 5, .id = "fgbench", .summary = "Foreground runtime benchmark",
};

static const solar_os_native_foreground_api_v1_t *app;
static char result[2048];
static unsigned used;
static volatile uint32_t checksum;

static void text(const char *s) { while (*s && used < sizeof(result)) result[used++] = *s++; }
static void number(uint64_t n)
{
    char digits[24]; unsigned count = 0;
    do { digits[count++] = '0' + n % 10; n /= 10; } while (n);
    while (count && used < sizeof(result)) result[used++] = digits[--count];
}
static void record(const char *name, unsigned count, uint64_t work, uint64_t wall)
{
    text(name); text(" "); number(count); text(" "); number(work); text(" "); number(wall); text("\n");
}
static void silence(int16_t *out, size_t frames, uint32_t rate, void *user)
{
    (void)rate; (void)user;
    for (size_t i = 0; i < frames * 2; ++i) out[i] = 0;
}
static void calls(unsigned which, const char *name)
{
    uint64_t wall = app->time_us(), work = 0;
    unsigned count = 0;
    for (unsigned batch = 0; batch < 50; ++batch) {
        uint64_t start = app->time_us();
        for (unsigned i = 0; i < 1000; ++i) {
            solar_os_native_foreground_key_v1_t key;
            solar_os_native_foreground_pointer_v1_t pointer;
            bool available;
            switch (which) {
            case 0: checksum = app->time_us(); break;
            case 1: checksum = app->stop_requested(); break;
            case 2: checksum = app->read_key(&key, &available); break;
            case 3: checksum = app->read_pointer(&pointer, &available); break;
            }
        }
        work += app->time_us() - start; count += 1000;
        if (app->stop_requested()) break;
        app->sleep_ms(1);
    }
    record(name, count, work, app->time_us() - wall);
}
static void frames(uint8_t *pixels, size_t size, unsigned pace, const char *name)
{
    solar_os_native_foreground_video_stats_v1_t stats;
    app->video_stats(&stats);
    uint64_t wall = app->time_us(), work = 0;
    unsigned count = 0;
    for (unsigned i = 0; i < 60 && !app->stop_requested(); ++i) {
        pixels[i % size] ^= 0xff;
        uint64_t start = app->time_us();
        if (app->present(pixels, size)) break;
        work += app->time_us() - start; ++count;
        app->sleep_ms(pace);
    }
    record(name, count, work, app->time_us() - wall);
    app->video_stats(&stats);
    record("presented", stats.frames, stats.present_us, stats.replaced_frames);
}

int main(int argc, char **argv)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    if (!host || argc != 2) return 1;
    app = host->get_service("foreground", 1,
        offsetof(solar_os_native_foreground_api_v1_t, calendar_read));
    const solar_os_native_memory_api_v1_t *memory = host->get_service("memory", 1,
        offsetof(solar_os_native_memory_api_v1_t, alloc_internal));
    if (!app || !memory) return 1;
    uint16_t width, height;
    if (app->video_dimensions(&width, &height)) return 1;
    used = 0; text("FGBENCH1\nsize "); number(width); text(" "); number(height); text("\n");
    calls(0, "time"); calls(1, "stop"); calls(2, "keys"); calls(3, "pointer");
    uint64_t wall = app->time_us();
    uint32_t x = 0x12345678;
    for (unsigned batch = 0; batch < 50; ++batch) {
        for (unsigned i = 0; i < 10000; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
        checksum = x;
    }
    record("kernel", 500000, app->time_us() - wall, x);
    size_t size = (size_t)width * height;
    uint8_t *pixels = memory->alloc(size);
    if (!pixels) return 1;
    for (size_t i = 0; i < size; ++i) pixels[i] = (i / width / 16) & 255;
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; ++i) palette[i] = i * 257U;
    if (app->video_open_ex(width, height, palette, 25, 0)) { memory->free(pixels); return 1; }
    frames(pixels, size, 1, "burst"); frames(pixels, size, 40, "paced");
    uint32_t rate; uint8_t channels;
    if (!app->audio_open(&rate, &channels)) {
        app->audio_close();
        if (!app->synth_open(silence, NULL, 256, rate)) {
            frames(pixels, size, 40, "audio"); app->synth_close();
        }
    }
    text("END\n");
    int error = app->replace_file(argv[1], result, used);
    memory->free(pixels);
    app->set_exit_code(error ? 1 : 0);
    return error ? 1 : 0;
}
