#ifndef TINY386_TEXT_H
#define TINY386_TEXT_H
#include "vendor/vga.h"
/* Cache both guest fonts and ordinary DOS text cells. Larger text grids still
 * render correctly, but redraw all their cells. Initialize with tiny_text_reset. */
typedef struct {
    uint16_t glyphs[512][32], cells[4000];
    uint8_t fonts[512][32], palette[16];
    bool valid[512];
    unsigned width, height, columns, rows, cw, ch, glyph_height;
    unsigned start_address, line_offset, cursor_address, cursor_start, cursor_end;
    bool cursor_visible, changed;
} tiny_text_cache_t;
#define TINY_TEXT_SCRATCH_BYTES sizeof(tiny_text_cache_t)
void tiny_text_reset(uint16_t *scratch);
bool tiny_text_changed(const uint16_t *scratch);
bool tiny_render_text(const VGATextFrame *text, uint8_t *pixels,
                      unsigned width, unsigned height, uint16_t *scratch);
#endif
