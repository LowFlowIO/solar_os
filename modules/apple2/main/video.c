/* Raster port of Reinette II Plus video modes, MIT, Arthur Ferreira (2020).
 * SolarOS adapter uses a 280x192 INDEX8 framebuffer instead of SDL drawing. */
#include "machine.h"
#include "font.h"
#include "vendor/puce6502.h"
#include <string.h>

static void hex_digits(char *out, uint32_t value, unsigned count)
{
    static const char digits[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < count; ++i) {
        out[count - i - 1U] = digits[value & 15U]; value >>= 4;
    }
}

static void rate_digits(char *out, uint32_t khz)
{
    if (khz > 99999U) khz = 99999U;
    out[0] = (char)('0' + khz / 10000U);
    out[1] = (char)('0' + khz / 1000U % 10U);
    out[2] = '.';
    out[3] = (char)('0' + khz / 100U % 10U);
    out[4] = (char)('0' + khz / 10U % 10U);
    out[5] = (char)('0' + khz % 10U);
    out[6] = 'M';
}

static void status_row(apple2_machine_t *m, const char text[40], unsigned top)
{
    for (unsigned col = 0; col < 40; ++col)
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 7; ++x)
                m->pixels[(top + y) * APPLE2_WIDTH + col * 7U + x] =
                    (apple2_font[(uint8_t)text[col]][y] >> x) & 1U ?
                    APPLE2_TEXT_FOREGROUND : APPLE2_TEXT_BACKGROUND;
}

static void status_overlay(apple2_machine_t *m)
{
    char text[40];
    memcpy(text, "PC:", 3); hex_digits(text + 3, puce6502Exec(0), 4);
    memcpy(text + 7, " D:", 3); text[10] = (char)('1' + m->curDrv);
    memcpy(text + 11, " T:", 3);
    text[14] = (char)('0' + m->disk[m->curDrv].track / 10);
    text[15] = (char)('0' + m->disk[m->curDrv].track % 10);
    memcpy(text + 16, " R:", 3); hex_digits(text + 19, m->disk_reads, 8);
    memcpy(text + 27, apple2_disk_accelerated() ? " FAST" : "     ", 5);
    memset(text + 32, ' ', 8);
    status_row(m, text, 184U);
    memset(text, ' ', sizeof(text));
    memcpy(text, "SPD:", 4); rate_digits(text + 4, m->guest_khz);
    memcpy(text + 12, "CPU:", 4); rate_digits(text + 16, m->cpu_khz);
    memcpy(text + 24, "VID:", 4);
    text[28] = (char)('0' + m->video_percent / 100U);
    text[29] = (char)('0' + m->video_percent / 10U % 10U);
    text[30] = (char)('0' + m->video_percent % 10U);
    text[31] = '%';
    status_row(m, text, 176U);
}

static void help_overlay(apple2_machine_t *m)
{
    static const char *const lines[] = {
        "REINETTE II PLUS", "", "F1 SAVE DRIVE 1 / ALT+F1 DRIVE 2",
        "F2 STATUS   F3 FAST DISK   F4 MUTE", "F10 PAUSE   F11 WARM RESET",
        "F12 CLOSE HELP", "", "CTRL+ALT+DEL RETURN TO SOLAROS",
        "CTRL+] RETURN TO SOLAROS", "SAVE DISK CHANGES BEFORE EXIT",
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        char text[40];
        memset(text, ' ', sizeof(text));
        memcpy(text, lines[i], strlen(lines[i]));
        status_row(m, text, 48U + i * 8U);
    }
}

static const uint8_t colors[32][3] = {
    {0,0,0}, {157,9,102}, {42,42,205}, {126,110,173},
    {31,129,128}, {137,130,122}, {86,168,228}, {144,178,223},
    {151,88,34}, {234,108,21}, {158,151,143}, {255,206,240},
    {144,192,49}, {255,253,166}, {159,210,213}, {255,255,255},
    {0,0,0}, {144,192,49}, {126,110,173}, {255,255,255},
    {0,0,0}, {234,108,21}, {86,168,228}, {255,255,255},
    {0,0,0}, {63,55,86}, {72,96,25}, {255,255,255},
    {0,0,0}, {43,84,114}, {117,54,10}, {255,255,255},
};

void apple2_palette(uint16_t palette[256])
{
    for (unsigned i = 0; i < 256; ++i) {
        const uint8_t *c = colors[i < 32 ? i : 0];
        palette[i] = (uint16_t)(((uint16_t)(c[0] >> 3) << 11) |
            ((uint16_t)(c[1] >> 2) << 5) | (c[2] >> 3));
    }
    palette[APPLE2_TEXT_FOREGROUND] = 0x0000U;
    palette[APPLE2_TEXT_BACKGROUND] = 0xffffU;
}

static unsigned text_offset(unsigned row)
{
    return (row & 7U) * 128U + (row >> 3) * 40U;
}

bool apple2_video_refresh(uint64_t cycle, uint64_t now_us)
{
    apple2_machine_t *m = apple2_machine;
    uint8_t mode = m->TEXT | (m->MIXED << 1) | (m->HIRES << 2) |
        (m->PAGE2 << 3) | (m->show_stats << 4) | (m->show_help << 5);
    unsigned graphics_base = m->HIRES ? (m->PAGE2 ? 0x4000 : 0x2000) :
        (m->PAGE2 ? 0x800 : 0x400);
    size_t graphics_size = m->TEXT ? 0U : (m->HIRES ? 8192U : 1024U);
    unsigned text_base = m->PAGE2 ? 0x800 : 0x400;
    size_t text_size = m->TEXT || m->MIXED ? 1024U : 0U;
    bool flash = (cycle / (APPLE2_CLOCK_HZ / 4U)) & 1U;
    bool changed = !m->video_valid || mode != m->video_mode ||
        memcmp(m->video_graphics, m->ram + graphics_base, graphics_size) ||
        memcmp(m->video_text, m->ram + text_base, text_size);
    if (!changed && text_size && flash != m->video_flash) {
        unsigned first_row = m->TEXT ? 0U : 20U;
        for (unsigned row = first_row; row < 24U && !changed; ++row)
            for (unsigned col = 0; col < 40U; ++col) {
                uint8_t value = m->ram[text_base + text_offset(row) + col];
                if (value >= 0x40U && value < 0x80U) { changed = true; break; }
            }
    }
    if (m->show_stats && now_us - m->video_status_us >= 200000U) changed = true;
    m->video_flash = flash;
    if (!changed) return false;
    memcpy(m->video_graphics, m->ram + graphics_base, graphics_size);
    memcpy(m->video_text, m->ram + text_base, text_size);
    m->video_valid = true; m->video_mode = mode; m->video_status_us = now_us;
    apple2_video(cycle);
    return true;
}

void apple2_video(uint64_t cycle)
{
    apple2_machine_t *m = apple2_machine;
    unsigned text_start = m->TEXT ? 0 : (m->MIXED ? 20 : 24);
    if (!m->TEXT && m->HIRES) {
        unsigned base = m->PAGE2 ? 0x4000 : 0x2000;
        for (unsigned y = 0; y < text_start * 8U; ++y) {
            unsigned offset = ((y & 7U) << 10) + ((y & 0x38U) << 4) + (y >> 6) * 40U;
            unsigned previous = 0;
            for (unsigned col = 0; col < 40; ++col) {
                uint8_t value = m->ram[base + offset + col];
                for (unsigned bit = 0; bit < 7; ++bit) {
                    unsigned x = col * 7 + bit, dot = (value >> bit) & 1U;
                    unsigned color = ((x & 1U) ? 8 : 0) + ((value & 0x80U) ? 4 : 0) + dot * 2 + previous;
                    m->pixels[y * APPLE2_WIDTH + x] = (uint8_t)(16 + color);
                    previous = dot;
                }
            }
        }
    } else if (!m->TEXT) {
        unsigned base = m->PAGE2 ? 0x800 : 0x400;
        for (unsigned row = 0; row < text_start; ++row)
            for (unsigned col = 0; col < 40; ++col) {
                uint8_t value = m->ram[base + text_offset(row) + col];
                for (unsigned y = 0; y < 8; ++y)
                    for (unsigned x = 0; x < 7; ++x)
                        m->pixels[(row * 8 + y) * APPLE2_WIDTH + col * 7 + x] =
                            (uint8_t)(y < 4 ? value & 15U : value >> 4);
            }
    }
    unsigned base = m->PAGE2 ? 0x800 : 0x400;
    bool flash = (cycle / (APPLE2_CLOCK_HZ / 4U)) & 1U;
    for (unsigned row = text_start; row < 24; ++row)
        for (unsigned col = 0; col < 40; ++col) {
            uint8_t value = m->ram[base + text_offset(row) + col];
            bool inverse = value < 0x40U || (value < 0x80U && flash);
            uint8_t glyph = value & 0x7fU;
            if (glyph > 0x5fU) glyph &= 0x3fU;
            if (glyph < 0x20U) glyph |= 0x40U;
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 7; ++x) {
                    bool dot = ((apple2_font[glyph][y] >> x) & 1U) != 0;
                    m->pixels[(row * 8 + y) * APPLE2_WIDTH + col * 7 + x] =
                        (dot != inverse) ? APPLE2_TEXT_FOREGROUND : APPLE2_TEXT_BACKGROUND;
                }
        }
    if (m->show_stats) status_overlay(m);
    if (m->show_help) help_overlay(m);
}
