#include "text.h"
#include <string.h>

static unsigned minimum(unsigned a, unsigned b) { return a < b ? a : b; }
static unsigned maximum(unsigned a, unsigned b) { return a > b ? a : b; }
static uint8_t indexed_color(uint32_t rgb)
{ return (uint8_t)(((rgb >> 8) & 0xe0) | ((rgb >> 6) & 0x1c) | ((rgb >> 3) & 3)); }

void tiny_text_reset(uint16_t *scratch)
{ ((tiny_text_cache_t *)scratch)->width = 0; }
bool tiny_text_changed(const uint16_t *scratch)
{ return ((const tiny_text_cache_t *)scratch)->changed; }

bool tiny_render_text(const VGATextFrame *t, uint8_t *pixels,
                      unsigned width, unsigned height, uint16_t *scratch)
{
    if (!t->columns || !t->rows || t->columns > width || t->rows > height ||
        !t->glyph_height || t->glyph_height > 32) return false;
    unsigned cw = minimum(width / t->columns, 16);
    unsigned ch = minimum(height / t->rows, 32);
    unsigned left = (width - t->columns * cw) / 2;
    unsigned top = (height - t->rows * ch) / 2;
    tiny_text_cache_t *cache = (tiny_text_cache_t *)scratch;
    bool full = cache->width != width || cache->height != height ||
        cache->columns != t->columns || cache->rows != t->rows ||
        cache->start_address != t->start_address || cache->line_offset != t->line_offset;
    if (full || cache->cw != cw || cache->ch != ch || cache->glyph_height != t->glyph_height) {
        memset(cache->valid, 0, sizeof(cache->valid));
        full = true;
    }
    uint8_t palette[16];
    for (unsigned i = 0; i < 16; ++i) palette[i] = indexed_color(t->palette[i]);
    if (!full && memcmp(palette, cache->palette, sizeof(palette))) full = true;
    bool cursor_changed = full || cache->cursor_address != t->cursor_address ||
        cache->cursor_start != t->cursor_start || cache->cursor_end != t->cursor_end ||
        cache->cursor_visible != t->cursor_visible;
    bool checked[512] = {false}, glyph_changed[512] = {false};
    cache->changed = full;
    if (full) memset(pixels, 0, width * height);
    for (unsigned row = 0; row < t->rows; ++row) {
        for (unsigned col = 0; col < t->columns; ++col) {
            unsigned address = t->start_address + row * t->line_offset + col * 4;
            unsigned character = t->ram[address & 0x1fffe];
            unsigned attr = t->ram[(address & 0x1fffe) + 1];
            unsigned bank = (attr >> 3) & 1, key = bank * 256 + character;
            uint16_t *glyph = cache->glyphs[key];
            if (!checked[key]) {
                const uint8_t *font = t->font[bank] + character * 128;
                bool dirty = !cache->valid[key];
                for (unsigned sy = 0; sy < t->glyph_height; ++sy) {
                    uint8_t bits = font[sy * 4];
                    if (!dirty && cache->fonts[key][sy] != bits) dirty = true;
                    cache->fonts[key][sy] = bits;
                }
                checked[key] = true;
                glyph_changed[key] = dirty;
                if (dirty) {
                    /* Area coverage preserves narrow strokes that nearest-neighbor
                     * sampling loses. Coordinates use exact integer overlap weights. */
                    for (unsigned y = 0; y < ch; ++y) {
                        unsigned y0 = y * t->glyph_height, y1 = (y + 1) * t->glyph_height;
                        glyph[y] = 0;
                        for (unsigned x = 0; x < cw; ++x) {
                            unsigned x0 = x * 8, x1 = (x + 1) * 8, ink = 0;
                            for (unsigned sy = y0 / ch; sy < (y1 + ch - 1) / ch; ++sy) {
                                unsigned wy = minimum(y1, (sy + 1) * ch) - maximum(y0, sy * ch);
                                uint8_t bits = font[sy * 4];
                                for (unsigned sx = x0 / cw; sx < (x1 + cw - 1) / cw; ++sx) {
                                    unsigned wx = minimum(x1, (sx + 1) * cw) - maximum(x0, sx * cw);
                                    if (bits & (0x80 >> sx)) ink += wx * wy;
                                }
                            }
                            if (ink * 3 >= 8 * t->glyph_height) glyph[y] |= 1U << x;
                        }
                    }
                    cache->valid[key] = true;
                }
            }
            unsigned cell = row * t->columns + col;
            uint16_t value = character | attr << 8;
            bool cursor_cell_changed = cursor_changed &&
                (address == t->cursor_address || (!full && address == cache->cursor_address));
            if (!full && cell < 4000 && cache->cells[cell] == value &&
                !glyph_changed[key] && !cursor_cell_changed) continue;
            cache->changed = true;
            if (cell < 4000) cache->cells[cell] = value;
            uint8_t fg = palette[attr & 15];
            uint8_t bg = palette[attr >> 4];
            bool cursor = t->cursor_visible && address == t->cursor_address;
            for (unsigned y = 0; y < ch; ++y) {
                unsigned sy = y * t->glyph_height / ch;
                bool underline = cursor && sy >= t->cursor_start && sy <= t->cursor_end;
                uint8_t *dst = pixels + (top + row * ch + y) * width + left + col * cw;
                for (unsigned x = 0; x < cw; ++x) dst[x] = underline || (glyph[y] & (1U << x)) ? fg : bg;
            }
        }
    }
    cache->width = width; cache->height = height;
    cache->columns = t->columns; cache->rows = t->rows;
    cache->cw = cw; cache->ch = ch; cache->glyph_height = t->glyph_height;
    cache->start_address = t->start_address; cache->line_offset = t->line_offset;
    cache->cursor_address = t->cursor_address; cache->cursor_start = t->cursor_start;
    cache->cursor_end = t->cursor_end; cache->cursor_visible = t->cursor_visible;
    memcpy(cache->palette, palette, sizeof(palette));
    return true;
}
