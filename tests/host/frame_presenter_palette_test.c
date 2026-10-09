#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_frame_presenter.h"

struct solar_os_frame_presenter {
    solar_os_frame_presenter_config_t config;
    uint8_t *presenting, *mono;
    size_t source_size, mono_size;
    uint16_t output_x, output_y, output_width, output_height, output_stride;
    bool mono_fallback, clear_background_pending;
    uint8_t mono_luma[256];
};
static bool inverted;
static solar_os_display_raster_t captured;
bool solar_os_gfx_palette_inverted(const solar_os_gfx_t *gfx)
{ assert(gfx); return inverted; }
esp_err_t solar_os_gfx_present_frame(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame)
{ assert(gfx && frame); captured = *frame; return ESP_OK; }

#include "frame_palette.inc"

int main(void)
{
    uint16_t palette[256] = {[0] = 0x00ffU, [3] = 0xf800U, [32] = 0, [33] = 0xffffU};
    uint8_t pixels[32], mono[4];
    for (unsigned format = SOLAR_OS_DISPLAY_FORMAT_INDEX8;
         format <= SOLAR_OS_DISPLAY_FORMAT_INDEX2; ++format) {
        solar_os_frame_presenter_t p = {
            .config = {.gfx = (solar_os_gfx_t *)&captured, .format = format,
                .width = 8, .height = 4, .stride = format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 8 : 2,
                .palette_rgb565 = palette, .palette_size = 256, .background_index = 33},
            .presenting = pixels, .mono = mono, .source_size = sizeof(pixels), .mono_size = sizeof(mono),
            .output_width = 8, .output_height = 4, .output_stride = 1, .clear_background_pending = true,
        };
        memset(pixels, 0, sizeof(pixels));
        for (unsigned i = 0; i < 2; ++i)
            for (unsigned literal = 0; literal < 2; ++literal)
                for (unsigned reverse = 0; reverse < 2; ++reverse) {
                    inverted = i;
                    p.config.literal_direct_palette = literal;
                    p.config.reverse_direct_palette = reverse;
                    p.mono_fallback = false;
                    assert(frame_present(&p) == ESP_OK);
                    assert(captured.palette_inverted == (!literal && (i != reverse)));
                    assert(captured.palette_rgb565 == palette && captured.clear_background);
                    assert(captured.background_index == 33 && captured.format == format);
                    /* Mono polarity is independent of the direct palette flag. */
                    p.mono_fallback = true;
                    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8)
                        for (unsigned n = 0; n < sizeof(pixels); ++n) pixels[n] = n & 1 ? 33 : 32;
                    frame_prepare_mono_palette(&p);
                    assert(frame_present(&p) == ESP_OK);
                    assert(captured.format == SOLAR_OS_DISPLAY_FORMAT_MONO1);
                    assert(captured.palette_inverted == inverted && !captured.clear_background);
                    assert(!captured.palette_rgb565 && captured.data == mono);
                    for (unsigned row = 0; row < 4; ++row)
                        assert(mono[row] == (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 0x55 : 0));
                    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX2)
                        assert(p.mono_luma[0] == 255 && p.mono_luma[3] == 0);
                }
    }
    puts("Literal color palettes, INDEX2 compatibility and mono polarity tests passed");
    return 0;
}
