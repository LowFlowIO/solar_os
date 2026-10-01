#include "solar_os_memory.h"
#include "solar_os_mpeg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union allocation {
    max_align_t align;
    struct {
        size_t size;
    } block;
} allocation_t;
static size_t live, peak;
static unsigned calls, fail_at;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)tag;
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED);
    if (++calls == fail_at)
        return NULL;
    allocation_t *a = malloc(sizeof(*a) + size);
    assert(a);
    a->block.size = size;
    live += size;
    if (live > peak)
        peak = live;
    return a + 1;
}
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls,
                             const char *tag)
{
    void *p = solar_os_memory_alloc(count * size, cls, tag);
    if (p)
        memset(p, 0, count * size);
    return p;
}
void solar_os_memory_free(void *ptr)
{
    if (!ptr)
        return;
    allocation_t *a = (allocation_t *)ptr - 1;
    live -= a->block.size;
    free(a);
}
void *solar_os_memory_realloc(void *ptr, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    void *p = solar_os_memory_alloc(size, cls, tag);
    if (!p)
        return NULL;
    if (ptr) {
        allocation_t *a = (allocation_t *)ptr - 1;
        memcpy(p, ptr, size < a->block.size ? size : a->block.size);
        solar_os_memory_free(ptr);
    }
    return p;
}
static bool cancel(void *user) { return *(bool *)user; }
static esp_err_t play(const char *path, unsigned *frames)
{
    solar_os_mpeg_t *d = NULL;
    char detail[96];
    esp_err_t err = solar_os_mpeg_open(path, NULL, NULL, &d, detail, sizeof(detail));
    *frames = 0;
    if (err != ESP_OK) {
        assert(d == NULL);
        assert(detail[0]);
        return err;
    }
    solar_os_mpeg_info_t info;
    solar_os_mpeg_info(d, &info);
    assert(info.width == 160 && info.height == 120 && info.fps == 25);
    bool vend = false, aend = !info.audio;
    double ahead = 0, previous = -1;
    unsigned audio_frames = 0;
    uint8_t *rgb = malloc(161 * 123 * 2), *mono = malloc(161 * 123);
    while (!vend && err == ESP_OK) {
        solar_os_mpeg_frame_t f;
        err = solar_os_mpeg_video(d, &f, &vend);
        if (err != ESP_OK || vend)
            break;
        assert(f.time > previous);
        previous = f.time;
        (*frames)++;
        solar_os_mpeg_raster(&f, rgb, 161, 123, true, true);
        solar_os_mpeg_raster(&f, mono, 161, 123, false, true);
        assert(mono[0] == f.y[0]);
        while (!aend && ahead < f.time + 0.2) {
            solar_os_mpeg_audio_t a;
            err = solar_os_mpeg_audio(d, &a, &aend);
            if (err != ESP_OK || aend)
                break;
            assert(a.frames == 1152);
            for (unsigned i = 0; i < a.frames * 2; i++)
                assert(isfinite(a.samples[i]));
            audio_frames += a.frames;
            ahead = a.time + (double)a.frames / a.sample_rate;
        }
        if (*frames > 110)
            abort();
    }
    free(rgb);
    free(mono);
    if (err != ESP_OK)
        assert(solar_os_mpeg_error(d)[0]);
    if (err == ESP_OK && info.audio)
        assert(audio_frames > 40000);
    solar_os_mpeg_close(d);
    assert(live == 0);
    return err;
}
static void raster_tests(void)
{
    uint8_t y[32 * 32], cb[16 * 16], cr[16 * 16], output[37 * 35 * 2], expected[37 * 35 * 2];
    uint32_t seed = 7;
    for (size_t i = 0; i < sizeof(y); i++) {
        seed = seed * 1664525 + 1013904223;
        y[i] = seed >> 24;
    }
    for (size_t i = 0; i < sizeof(cb); i++) {
        seed = seed * 1664525 + 1013904223;
        cb[i] = seed >> 24;
        seed = seed * 1664525 + 1013904223;
        cr[i] = seed >> 24;
    }
    solar_os_mpeg_frame_t f = {
        .width = 29, .height = 27, .y = y, .cb = cb, .cr = cr, .y_stride = 32, .chroma_stride = 16};
    const unsigned widths[] = {1, 7, 8, 13, 29, 37}, heights[] = {1, 9, 27, 35};
    for (unsigned color = 0; color < 2; color++)
        for (unsigned i = 0; i < 6; i++)
            for (unsigned j = 0; j < 4; j++) {
                unsigned w = widths[i], h = heights[j], bpp = color ? 2 : 1;
                memset(output, 0xa5, sizeof(output));
                solar_os_mpeg_raster(&f, output, w, h, color, true);
                solar_os_mpeg_raster(&f, expected, w, h, color, false);
                assert(!memcmp(output, expected, w * h * bpp));
                for (size_t k = w * h * bpp; k < sizeof(output); k++)
                    assert(output[k] == 0xa5);
            }
}
int main(int argc, char **argv)
{
    assert(argc >= 5);
    raster_tests();
    unsigned frames;
    assert(play(argv[1], &frames) == ESP_OK && frames == 100);
    unsigned allocations = calls;
    assert(peak < 1024 * 1024);
    calls = 0;
    assert(play(argv[2], &frames) == ESP_OK && frames == 100);
    assert(play(argv[3], &frames) == ESP_ERR_NOT_SUPPORTED);
    assert(play(argv[4], &frames) == ESP_OK && frames == 100);
    for (unsigned i = 1; i <= allocations; i++) {
        calls = 0;
        fail_at = i;
        esp_err_t err = play(argv[1], &frames);
        assert(err == ESP_ERR_NO_MEM);
        assert(live == 0);
    }
    fail_at = 0;
    for (int i = 5; i < argc; i++) {
        solar_os_mpeg_t *invalid = NULL;
        char detail[96];
        esp_err_t err = solar_os_mpeg_open(argv[i], NULL, NULL, &invalid, detail, sizeof(detail));
        assert(err != ESP_OK && invalid == NULL && detail[0] && live == 0);
    }
    bool stopped = true;
    solar_os_mpeg_t *d = NULL;
    char detail[96];
    assert(solar_os_mpeg_open(argv[1], cancel, &stopped, &d, detail, sizeof(detail)) ==
           ESP_ERR_TIMEOUT);
    assert(!d && !live);
    stopped = false;
    assert(solar_os_mpeg_open(argv[1], cancel, &stopped, &d, detail, sizeof(detail)) == ESP_OK);
    stopped = true;
    solar_os_mpeg_frame_t frame;
    bool ended = false;
    assert(solar_os_mpeg_video(d, &frame, &ended) == ESP_ERR_TIMEOUT);
    solar_os_mpeg_close(d);
    assert(live == 0);
    puts("MPEG frames, audio, raster, format rejection, cancellation and allocation failures "
         "passed");
}
