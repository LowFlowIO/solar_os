#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "vendor/puce6502.h"

static size_t audio_frames;
static bool heard_sound;
static int audio_write(const int16_t *samples, size_t frames)
{
    assert(frames == 512U);
    for (size_t i = 0; i < frames * 2U; ++i) {
        assert(samples[i] >= -12000 && samples[i] <= 12000);
        if (samples[i]) heard_sound = true;
    }
    audio_frames += frames;
    return 0;
}

static void test_core(apple2_machine_t *m)
{
    /* Decimal 15 + 27 = 42; binary 15 + 27 = 42. */
    static const uint8_t program[] = {
        0xf8, 0x18, 0xa9, 0x15, 0x69, 0x27, 0x8d, 0x00, 0x03,
        0xd8, 0x18, 0xa9, 15, 0x69, 27, 0x8d, 0x01, 0x03,
        0x4c, 0x12, 0x02,
    };
    memcpy(m->ram + 0x200, program, sizeof(program));
    m->rom[0x2ffc] = 0; m->rom[0x2ffd] = 2;
    puce6502RST();
    assert(puce6502Exec(200) == 0x212);
    assert(m->ram[0x300] == 0x42 && m->ram[0x301] == 42);
    m->rom[0] = 0xaa;
    (void)readMem(0xc082); /* ROM visible, LC write disabled. */
    writeMem(0xd000, 0x55);
    assert(readMem(0xd000) == 0xaa && !m->bk2[0]);
    (void)readMem(0xc083); (void)readMem(0xc083);
    writeMem(0xd000, 0x55); writeMem(0xe000, 0x66);
    assert(readMem(0xd000) == 0x55 && readMem(0xe000) == 0x66);
    (void)readMem(0xc08b); (void)readMem(0xc08b);
    writeMem(0xd000, 0x77);
    assert(readMem(0xd000) == 0x77 && readMem(0xe000) == 0x66);
    (void)readMem(0xc080);
    assert(readMem(0xd000) == 0x55);
    m->KBD = 0xc1;
    assert(readMem(0xc000) == 0xc1 && readMem(0xc010) == 0x41);
    m->paddle[0] = 255; m->paddle[1] = 0;
    (void)readMem(0xc070);
    assert(readMem(0xc064) == 0x80 && readMem(0xc065) == 0);
    ticks += 500000;
    assert(readMem(0xc064) == 0);
}

static void test_disk(apple2_machine_t *m)
{
    (void)readMem(0xc0ea);
    assert(readMem(0xc0ec) == 0xff);
    m->disk[0].mounted = true; m->disk[0].nibble = 0;
    writeMem(0xc0ed, 0xa5); (void)readMem(0xc0ef);
    m->disk[0].readOnly = true;
    (void)readMem(0xc0ec);
    assert(!m->disk[0].dirty && !m->disk[0].data[0]);
    m->disk[0].readOnly = false; m->disk[0].nibble = 0;
    (void)readMem(0xc0ec);
    assert(m->disk[0].dirty && m->disk[0].data[0] == 0xa5);
    (void)readMem(0xc0e9); (void)readMem(0xc0eb);
    assert(m->curDrv == 1 && !m->disk[0].motorOn && m->disk[1].motorOn);
    uint32_t random = 1;
    for (unsigned i = 0; i < 500000; ++i) {
        random = random * 1664525U + 1013904223U;
        uint16_t address = (uint16_t)(0xc0e0U + ((random >> 16) & 15U));
        if (random & 1U) (void)readMem(address);
        else writeMem(address, (uint8_t)random);
        assert(m->disk[m->curDrv].track < 35 && m->disk[m->curDrv].nibble < 6656);
    }
}

static void test_video(apple2_machine_t *m)
{
    uint16_t palette[256]; apple2_palette(palette);
    assert(palette[0] == 0 && palette[15] == 0xffff);
    memset(m->ram, 0xa0, sizeof(m->ram));
    m->TEXT = true; m->PAGE2 = false;
    apple2_video(0);
    for (size_t i = 0; i < sizeof(m->pixels); ++i) assert(m->pixels[i] == APPLE2_TEXT_BACKGROUND);
    uint8_t glyph[56];
    m->ram[0x400] = 0xc1; apple2_video(0);
    for (unsigned y = 0; y < 8; ++y) memcpy(glyph + y * 7, m->pixels + y * APPLE2_WIDTH, 7);
    for (unsigned variant = 0; variant < 3; ++variant) {
        m->ram[0x400] = variant == 0 ? 0x01 : 0x41;
        apple2_video(variant == 2 ? APPLE2_CLOCK_HZ / 4U : 0);
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 7; ++x) {
                uint8_t normal = glyph[y * 7 + x];
                uint8_t expected = variant == 1 ? normal :
                    (normal == APPLE2_TEXT_FOREGROUND ? APPLE2_TEXT_BACKGROUND : APPLE2_TEXT_FOREGROUND);
                assert(m->pixels[y * APPLE2_WIDTH + x] == expected);
            }
    }
    m->ram[0x400] = 0; apple2_video(0); assert(m->pixels[0] == APPLE2_TEXT_FOREGROUND);
    m->TEXT = false; m->HIRES = false; m->MIXED = false;
    m->ram[0x400] = 0xf2; apple2_video(0);
    assert(m->pixels[0] == 2 && m->pixels[4 * APPLE2_WIDTH] == 15);
    m->PAGE2 = true; m->ram[0x800] = 0x31; apple2_video(0);
    assert(m->pixels[0] == 1 && m->pixels[4 * APPLE2_WIDTH] == 3);
    m->HIRES = true; m->MIXED = true;
    memset(m->ram + 0x4000, 0, 8192); apple2_video(0);
    assert(m->pixels[0] == 16 && m->pixels[160 * APPLE2_WIDTH] >= APPLE2_TEXT_FOREGROUND);
    for (size_t i = 0; i < 160U * APPLE2_WIDTH; ++i) assert(m->pixels[i] < 32U);
    for (size_t i = 160U * APPLE2_WIDTH; i < sizeof(m->pixels); ++i)
        assert(m->pixels[i] == APPLE2_TEXT_FOREGROUND || m->pixels[i] == APPLE2_TEXT_BACKGROUND);
    uint64_t before = ticks;
    m->show_stats = true; apple2_video(0);
    assert(ticks == before);
    for (size_t i = 184U * APPLE2_WIDTH; i < sizeof(m->pixels); ++i)
        assert(m->pixels[i] == APPLE2_TEXT_FOREGROUND || m->pixels[i] == APPLE2_TEXT_BACKGROUND);
}

static void test_video_refresh(apple2_machine_t *m)
{
    apple2_machine_init(m);
    memset(m->ram + 0x400, 0xa0, 2048U);
    assert(apple2_video_refresh(0, 0));
    assert(!apple2_video_refresh(0, 40000U));
    m->ram[0x1000] = 123;
    assert(!apple2_video_refresh(0, 80000U));
    m->ram[0x400] = 0xc1;
    assert(apple2_video_refresh(0, 120000U));
    assert(!apple2_video_refresh(APPLE2_CLOCK_HZ / 4U, 160000U));
    m->ram[0x400] = 0x41;
    assert(apple2_video_refresh(APPLE2_CLOCK_HZ / 4U, 200000U));
    assert(apple2_video_refresh(APPLE2_CLOCK_HZ / 2U, 240000U));
    assert(!apple2_video_refresh(APPLE2_CLOCK_HZ / 2U, 280000U));
    m->PAGE2 = true;
    assert(apple2_video_refresh(0, 320000U));
    assert(!apple2_video_refresh(0, 360000U));
    m->TEXT = false; m->HIRES = true;
    assert(apple2_video_refresh(0, 400000U));
    m->ram[0x4000] = 0x7f;
    assert(apple2_video_refresh(0, 440000U));
    m->ram[0x800] = 0xc2;
    assert(!apple2_video_refresh(0, 480000U));
    m->MIXED = true;
    assert(apple2_video_refresh(0, 520000U));
    m->ram[0x800 + 20U % 8U * 128U + 20U / 8U * 40U] = 0xc3;
    assert(apple2_video_refresh(0, 560000U));
    m->HIRES = false;
    assert(apple2_video_refresh(0, 600000U));
    assert(!apple2_video_refresh(0, 640000U));
    m->show_stats = true; m->guest_khz = 1234; m->cpu_khz = 5678; m->video_percent = 100;
    assert(apple2_video_refresh(0, 680000U));
    assert(!apple2_video_refresh(0, 720000U));
    assert(apple2_video_refresh(0, 880000U));
    for (size_t i = 176U * APPLE2_WIDTH; i < sizeof(m->pixels); ++i)
        assert(m->pixels[i] == APPLE2_TEXT_FOREGROUND || m->pixels[i] == APPLE2_TEXT_BACKGROUND);
    m->show_stats = false;
    assert(apple2_video_refresh(0, 920000U));
    assert(!apple2_video_refresh(0, 960000U));
    /* Help toggles redraw even with unchanged guest video, then restore it. */
    uint8_t *guest = malloc(sizeof(m->pixels)); assert(guest);
    memcpy(guest, m->pixels, sizeof(m->pixels));
    m->show_help = true;
    assert(apple2_video_refresh(0, 1000000U));
    assert(memcmp(guest, m->pixels, sizeof(m->pixels)));
    assert(!apple2_video_refresh(0, 1040000U));
    m->show_help = false;
    assert(apple2_video_refresh(0, 1080000U));
    assert(!memcmp(guest, m->pixels, sizeof(m->pixels)));
    free(guest);
}

static void test_audio(apple2_machine_t *m)
{
    static const solar_os_native_foreground_api_v1_t api = {.audio_write = audio_write};
    apple2_app = &api;
    m->audio_active = true; m->channels = 2; m->sample_rate = 44100;
    m->sample_step = APPLE2_CLOCK_HZ / m->sample_rate;
    m->sample_remainder = APPLE2_CLOCK_HZ % m->sample_rate;
    ticks = APPLE2_CLOCK_HZ; apple2_audio_until(ticks);
    assert(!heard_sound && audio_frames == (44101U / 512U) * 512U);
    (void)readMem(0xc030); ticks += 20000; apple2_audio_until(ticks);
    assert(heard_sound);
    ticks += APPLE2_CLOCK_HZ; apple2_audio_until(ticks); heard_sound = false;
    ticks += APPLE2_CLOCK_HZ; apple2_audio_until(ticks);
    assert(!heard_sound);
    m->disk[0].mounted = m->disk[0].motorOn = true;
    size_t before = audio_frames;
    ticks += 100U * APPLE2_CLOCK_HZ;
    apple2_audio_until(ticks);
    assert(audio_frames == before + 512U && m->pcm_frames == 0 && !heard_sound);
    ticks += 100U * APPLE2_CLOCK_HZ;
    apple2_audio_until(ticks);
    assert(audio_frames == before + 512U);
    m->fast_disk = false;
    apple2_audio_until(ticks);
    assert(m->pcm_frames == 1U && !m->audio_suspended);
    m->audio_active = false;
}

static void load_asset(const char *directory, const char *name, void *data, size_t size)
{
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s", directory, name) < (int)sizeof(path));
    FILE *file = fopen(path, "rb");
    assert(file && fread(data, 1, size, file) == size && fgetc(file) == EOF);
    assert(!fclose(file));
}
static void screen_text(apple2_machine_t *m, char text[24 * 41 + 1])
{
    size_t n = 0;
    for (unsigned row = 0; row < 24; ++row) {
        unsigned offset = (row & 7U) * 128U + (row >> 3) * 40U;
        for (unsigned col = 0; col < 40; ++col) {
            uint8_t ch = m->ram[0x400 + offset + col] & 0x7fU;
            text[n++] = (char)(ch < 32 ? ch + 64 : ch);
        }
        text[n++] = '\n';
    }
    text[n] = '\0';
}
static void test_boot(apple2_machine_t *m, const char *directory)
{
    apple2_machine_init(m);
    load_asset(directory, "rom/appleII+.rom", m->rom, sizeof(m->rom));
    load_asset(directory, "rom/diskII.rom", m->sl6, sizeof(m->sl6));
    load_asset(directory, "DOS 3.3.nib", m->disk[0].data, sizeof(m->disk[0].data));
    m->disk[0].mounted = true; puce6502RST();
    /* System Master also loads Integer BASIC into the language card. */
    for (unsigned i = 0; i < 300000; ++i) puce6502Exec(2046);
    char screen[24 * 41 + 1]; screen_text(m, screen); puts(screen);
    assert(strstr(screen, "DOS VERSION") && strstr(screen, "3.3"));
    const char *command = "PRINT 123+456\r";
    for (size_t i = 0; command[i]; ++i) {
        m->KBD = (uint8_t)command[i] | 0x80U;
        for (unsigned j = 0; j < 1000; ++j) {
            puce6502Exec(2046);
            if (!(m->KBD & 0x80U)) break;
        }
        if (m->KBD & 0x80U) {
            fprintf(stderr, "key %zu not read, PC=%04x track=%u LC=%u/%u/%u\n", i,
                puce6502Exec(1), m->disk[0].track, m->LCRD, m->LCWR, m->LCBK2);
        }
        assert(!(m->KBD & 0x80U));
    }
    puce6502Exec(APPLE2_CLOCK_HZ); screen_text(m, screen); puts(screen);
    assert(strstr(screen, "579"));
}

int main(int argc, char **argv)
{
    apple2_machine_t *m = calloc(1, sizeof(*m)); assert(m);
    apple2_machine_init(m); test_core(m); test_disk(m); test_video(m);
    test_video_refresh(m);
    apple2_machine_init(m); test_audio(m);
    if (argc == 2) test_boot(m, argv[1]);
    free(m); puts("Apple II app core tests passed"); return 0;
}
