#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_display_surface.h"

typedef int u8g2_t;
typedef struct {
    bool export_enabled, export_publishing;
    unsigned export_readers, refs;
    uint32_t generation, export_frame_id;
    uint8_t *export_buffer;
    size_t export_buffer_size;
    uint16_t export_native_stride;
    struct { bool black_is_one; } target;
} display_target_slot_t;
static display_target_slot_t display_targets[1];
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
static int display_find_slot_by_u8g2_locked(u8g2_t *display) { return display ? 0 : -1; }
static uint8_t *display_detach_export_buffer_locked(display_target_slot_t *slot)
{ (void)slot; return NULL; }
static void solar_os_memory_free(void *p) { assert(p == NULL); }
#include "export.inc"

int main(void)
{
    uint8_t rgb[8 * 20], indices[64], actual[8], indexed[8];
    uint16_t palette[256] = {0};
    u8g2_t display = 1;
    /* Pad RGB rows to check stride; use distinct R/G/B values to detect
     * accidental host-endian reads. The old indexed path is the reference. */
    memset(rgb, 0xcc, sizeof(rgb));
    for (unsigned i = 0; i < 64; ++i) {
        uint16_t color = (uint16_t)(i * 1040U + 7U);
        palette[i] = color;
        indices[i] = i;
        rgb[(i / 8) * 20 + (i % 8) * 2] = color >> 8;
        rgb[(i / 8) * 20 + (i % 8) * 2 + 1] = color & 255;
    }
    solar_os_display_surface_t surface = {
        .data = rgb, .data_size = sizeof(rgb), .width = 8, .height = 8,
        .stride = 20, .native_width = 8, .native_height = 8,
        .format = SOLAR_OS_DISPLAY_FORMAT_RGB565,
    };
    display_target_slot_t *slot = display_targets;
    *slot = (display_target_slot_t){
        .export_enabled = true, .generation = 7, .export_buffer = actual,
        .export_buffer_size = sizeof(actual), .export_native_stride = 8,
    };
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        surface.rotation = rotation;
        for (unsigned inverted = 0; inverted < 2; ++inverted) {
            slot->target.black_is_one = inverted;
            surface.data = rgb; surface.stride = 20;
            surface.format = SOLAR_OS_DISPLAY_FORMAT_RGB565;
            surface.palette_rgb565 = NULL;
            display_publish_surface(&display, &surface);
            memcpy(indexed, actual, sizeof(actual));
            surface.data = indices; surface.stride = 8;
            surface.format = SOLAR_OS_DISPLAY_FORMAT_INDEX8;
            surface.palette_rgb565 = palette;
            display_publish_surface(&display, &surface);
            assert(memcmp(actual, indexed, sizeof(actual)) == 0);
            assert(slot->refs == 0 && !slot->export_publishing);
        }
    }
    uint32_t frame = slot->export_frame_id;
    slot->export_readers = 1;
    memset(actual, 0xa5, sizeof(actual));
    display_publish_surface(&display, &surface);
    assert(slot->export_frame_id == frame && actual[0] == 0xa5);
    slot->export_readers = 0;
    slot->export_enabled = false;
    display_publish_surface(&display, &surface);
    assert(slot->export_frame_id == frame && actual[0] == 0xa5);
    puts("RGB565 export matches indexed dithering at every rotation and polarity; readers retain immutable frames");
}
