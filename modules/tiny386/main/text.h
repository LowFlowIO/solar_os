#ifndef TINY386_TEXT_H
#define TINY386_TEXT_H
#include "vendor/vga.h"
/* Scratch stores up to 32 rows of 16-bit pixels for each of two 256-glyph fonts. */
#define TINY_TEXT_SCRATCH_BYTES (2U * 256U * 32U * sizeof(uint16_t))
bool tiny_render_text(const VGATextFrame *text, uint8_t *pixels,
                      unsigned width, unsigned height, uint16_t *scratch);
#endif
