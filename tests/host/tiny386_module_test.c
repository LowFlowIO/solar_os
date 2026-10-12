#define TINY386_PORT_IMPLEMENTATION
#include "port.h"
#include "text.h"
#include "vendor/pc.h"
#include <assert.h>
#include <sys/time.h>

int tiny386_main(int argc, char **argv);
uint32_t get_uticks(void);
static uint8_t disk[1440 * 1024];
static uint8_t harddisk[1024];
static bool open_disks[2], fail_open, fail_write;
static unsigned keys_sent, writes;
static bool booted, keyboard_ok, fail_allocation, fail_read, fail_save;
static unsigned allocations, frames, suspends, saved;
static int exit_code;
static uint64_t boot_time, wall_offset;
static uint32_t guest_before_suspend;
static bool fixed_clock;
static uint64_t fixed_time;

static uint64_t now(void)
{
    if (fixed_clock) return fixed_time;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000 + wall_offset;
}
static void *allocate(size_t n)
{
    if (fail_allocation) return NULL;
    void *p = malloc(n); if (p) ++allocations; return p;
}
static void release(void *p) { assert(p && allocations); --allocations; free(p); }
static int write_text(const char *text, size_t n)
{
    printf("%.*s\n", (int)n, text);
    if (n == strlen("SOLAROS BOOT OK") && !memcmp(text, "SOLAROS BOOT OK", n)) {
        booted = true; boot_time = now();
    }
    if (n == strlen("KEYBOARD OK") && !memcmp(text, "KEYBOARD OK", n)) keyboard_ok = true;
    return 0;
}
static int size_of_file(const char *path, uint32_t *n)
{
    if (!strcmp(path, "/test.img")) *n = sizeof(disk);
    else if (!strcmp(path, "/hard.img")) *n = 32U * 1024U * 1024U;
    else return -5;
    return 0;
}
static int read_image(const char *path, uint32_t offset, void *out, size_t n, size_t *got)
{
    if (fail_read || strcmp(path, "/test.img") || offset > sizeof(disk) || n > sizeof(disk) - offset) return -1;
    memcpy(out, disk + offset, n); *got = n; return 0;
}
static int replace_image(const char *path, const void *in, size_t n)
{ assert(!strcmp(path, "/test.img") && n == sizeof(disk)); ++saved; if (fail_save) return -1; memcpy(disk, in, n); return 0; }
static int disk_open(const char *path, uint32_t *handle, uint32_t *size)
{
    if (fail_open) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (size_of_file(path, size)) return -5;
    unsigned index = strcmp(path, "/test.img") ? 1 : 0;
    if (open_disks[index]) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    open_disks[index] = true; *handle = index + 1; return 0;
}
static int disk_read(uint32_t handle, uint32_t offset, void *out, size_t n)
{
    assert(handle >= 1 && handle <= 2 && open_disks[handle - 1] && n <= 65536);
    if (fail_read) return -1;
    if (handle == 1) { assert(offset <= sizeof(disk) && n <= sizeof(disk) - offset); memcpy(out, disk + offset, n); }
    else {
        assert(offset <= 32U * 1024U * 1024U && n <= 32U * 1024U * 1024U - offset);
        memset(out, 0, n);
        if (offset < sizeof(harddisk)) {
            size_t count = n < sizeof(harddisk) - offset ? n : sizeof(harddisk) - offset;
            memcpy(out, harddisk + offset, count);
        }
    }
    return 0;
}
static int disk_write(uint32_t handle, uint32_t offset, const void *in, size_t n)
{
    assert(handle >= 1 && handle <= 2 && open_disks[handle - 1] && n <= 65536);
    if (fail_write) return -1;
    if (handle == 1) { assert(offset <= sizeof(disk) && n <= sizeof(disk) - offset); memcpy(disk + offset, in, n); }
    else { assert(offset <= sizeof(harddisk) && n <= sizeof(harddisk) - offset); memcpy(harddisk + offset, in, n); }
    ++writes; return 0;
}
static int disk_sync(uint32_t handle)
{ assert(handle >= 1 && handle <= 2 && open_disks[handle - 1]); ++saved; return fail_save ? -1 : 0; }
static int disk_close(uint32_t handle)
{ assert(handle >= 1 && handle <= 2 && open_disks[handle - 1]); open_disks[handle - 1] = false; return fail_save ? -1 : 0; }
static int resolve(const char *path, char *out, size_t n)
{ if (strlen(path) >= n) return -1; strcpy(out, path); return 0; }
static int open_video(uint16_t w, uint16_t h, const uint16_t *pal, uint16_t fps, uint8_t bg)
{
    assert(pal[0] == 0 && pal[255] == 0xffff && fps == 15 && bg == 0);
    assert(w == 480 && h == 304); return 0;
}
static int video_dimensions(uint16_t *w, uint16_t *h) { *w = 480; *h = 304; return 0; }
static int present_frame(const uint8_t *pixels, size_t n)
{
    assert(pixels && n == 480 * 304); ++frames;
    if (suspends) assert((uint32_t)(get_uticks() - guest_before_suspend) < 1000000);
    const char *path = getenv("TINY386_TEST_FRAME");
    static bool captured;
    if (path && booted && !captured) {
        FILE *image = fopen(path, "wb"); assert(image);
        fprintf(image, "P6\n480 304\n255\n");
        for (size_t i = 0; i < n; ++i) {
            uint8_t c = pixels[i], rgb[] = {(c >> 5) * 255 / 7, ((c >> 2) & 7) * 255 / 7, (c & 3) * 255 / 3};
            assert(fwrite(rgb, 1, 3, image) == 3);
        }
        fclose(image); captured = true;
    }
    return 0;
}
static bool stop(void) { return booted && keyboard_ok && suspends && now() - boot_time > 150000; }
static bool suspend_requested(void) { return booted && !suspends; }
static int suspend_app(void) { guest_before_suspend = get_uticks(); ++suspends; wall_offset += 5000000; boot_time += 5000000; return 0; }
static int enable_suspend(void) { return 0; }
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    const uint8_t keys[] = {'A', '>', 3};
    *available = booted && keys_sent < sizeof(keys);
    if (*available) *key = (solar_os_native_foreground_key_v1_t){.key = keys[keys_sent++], .action = SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS};
    return 0;
}
static void sleep_ms(uint32_t n) { usleep(n * 1000); }
static void set_exit(int n) { exit_code = n; }
static const solar_os_native_foreground_api_v1_t app = {
    .abi_version = 1, .struct_size = sizeof(app), .time_us = now, .sleep_ms = sleep_ms,
    .video_open_ex = open_video, .present = present_frame, .read_key = read_key,
    .file_size = size_of_file, .read_file = read_image, .replace_file = replace_image,
    .resolve_path = resolve, .stop_requested = stop, .enable_suspend = enable_suspend,
    .suspend_requested = suspend_requested, .suspend = suspend_app, .set_exit_code = set_exit,
    .video_dimensions = video_dimensions,
    .disk_open = disk_open, .disk_read = disk_read, .disk_write = disk_write,
    .disk_sync = disk_sync, .disk_close = disk_close,
};
static const solar_os_native_memory_api_v1_t memory = {
    .abi_version = 1, .struct_size = sizeof(memory), .alloc = allocate, .free = release,
};
static const void *service(const char *name, uint32_t abi, uint32_t size)
{
    assert(abi == 1);
    if (!strcmp(name, "foreground") && size <= sizeof(app)) return &app;
    if (!strcmp(name, "memory") && size <= sizeof(memory)) return &memory;
    return NULL;
}
static const solar_os_native_host_api_v1_t host = {
    .abi_version = 1, .struct_size = sizeof(host), .write_utf8 = write_text, .get_service = service,
};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }

static void no_redraw(void *opaque, int x, int y, int w, int h)
{ (void)opaque; (void)x; (void)y; (void)w; (void)h; }

static void test_retrace_without_frequent_rendering(void)
{
    tiny_app = &app; tiny_memory = &memory; tiny_host = &host;
    fixed_time = now(); fixed_clock = true; tiny_clock_reset();
    PCConfig config = {.mem_size = 2 * 1024 * 1024, .vga_mem_size = 256 * 1024,
        .cpu_gen = 3, .width = 640, .height = 400};
    uint8_t *framebuffer = tiny_calloc(640 * 400, 2);
    PC *pc = pc_new(no_redraw, NULL, framebuffer, &config);
    load_bios_and_reset(pc);
    unsigned retraces = 0, renders = 0;
    bool previous_retrace = false;
    for (unsigned elapsed = 0; elapsed < 200000; elapsed += 250) {
        pc_step(pc);
        bool retrace = (vga_ioport_read(pc->vga, 0x3da) & 8) != 0;
        if (retrace && !previous_retrace) ++retraces;
        previous_retrace = retrace;
        if (elapsed >= renders * 66666) { pc_vga_step(pc); ++renders; }
        fixed_time += 250;
    }
    /* Actual guest-visible retrace status progresses at about 60 Hz even
     * though the host only checks for rendering at 15 Hz. */
    assert(retraces >= 10 && retraces <= 13);
    assert(renders == 3);
    assert(tiny_cleanup() == 0 && allocations == 0);
    fixed_clock = false;
}

static void compare_cold_text(const VGATextFrame *text, uint8_t *pixels, uint16_t *scratch)
{
    uint8_t *expected = malloc(480 * 300);
    uint16_t *cold = malloc(TINY_TEXT_SCRATCH_BYTES);
    tiny_text_reset(cold);
    assert(tiny_render_text(text, pixels, 480, 300, scratch));
    assert(tiny_render_text(text, expected, 480, 300, cold));
    assert(!memcmp(pixels, expected, 480 * 300));
    free(cold); free(expected);
}

static void test_native_text(void)
{
    uint8_t *ram = calloc(1, 128 * 1024), *font = calloc(1, 256 * 128);
    uint8_t *pixels = malloc(480 * 300);
    uint16_t *scratch = malloc(TINY_TEXT_SCRATCH_BYTES);
    tiny_text_reset(scratch);
    uint32_t palette[16] = {0}; palette[15] = 0xffff;
    VGATextFrame text = {.ram = ram, .font = {font, font}, .palette = palette,
        .columns = 80, .rows = 25, .glyph_height = 16, .line_offset = 320,
        .start_address = 8, .cursor_address = 20, .cursor_start = 14,
        .cursor_end = 15, .cursor_visible = true};
    for (unsigned i = 0; i < 16; ++i) font['A' * 128 + i * 4] = 0x40;
    font['B' * 128 + 4] = 0xff;
    ram[8] = 'A'; ram[9] = 15; ram[12] = 'B'; ram[13] = 15;
    ram[21] = 15; ram[8 + 320 + 1] = 0xf0;
    assert(tiny_render_text(&text, pixels, 480, 300, scratch));
    assert(tiny_text_changed(scratch));
    assert(tiny_render_text(&text, pixels, 480, 300, scratch));
    assert(!tiny_text_changed(scratch));
    /* A one-pixel vertical stroke survives in every native cell row. */
    for (unsigned y = 0; y < 12; ++y) {
        unsigned ink = 0;
        for (unsigned x = 0; x < 6; ++x) ink += pixels[y * 480 + x] != 0;
        assert(ink > 0 && ink < 6);
    }
    unsigned horizontal = 0;
    for (unsigned y = 0; y < 12; ++y) {
        unsigned ink = 0;
        for (unsigned x = 6; x < 12; ++x) ink += pixels[y * 480 + x] != 0;
        if (ink == 6) ++horizontal;
    }
    assert(horizontal > 0 && horizontal < 12);
    for (unsigned x = 18; x < 24; ++x) assert(pixels[11 * 480 + x] == 255);
    for (unsigned x = 0; x < 6; ++x) assert(pixels[12 * 480 + x] == 255);
    text.cursor_visible = false;
    memset(font, 0, 256 * 128); /* Guest font replacement must invalidate glyphs. */
    assert(tiny_render_text(&text, pixels, 480, 300, scratch));
    for (unsigned y = 0; y < 12; ++y)
        for (unsigned x = 0; x < 24; ++x) assert(pixels[y * 480 + x] == 0);
    /* A cached frame must match a fresh render after every kind of guest
     * text change, including fonts, palette, cursor, scrolling and geometry. */
    font['A' * 128] = 0xff;
    compare_cold_text(&text, pixels, scratch);
    palette[15] = 0xff0000;
    compare_cold_text(&text, pixels, scratch);
    text.cursor_visible = true; text.cursor_address = 8;
    compare_cold_text(&text, pixels, scratch);
    text.cursor_address = 12; text.cursor_start = 0; text.cursor_end = 2;
    compare_cold_text(&text, pixels, scratch);
    text.cursor_visible = false;
    ram[8] = 'B'; ram[9] = 0xf0;
    compare_cold_text(&text, pixels, scratch);
    text.start_address += 320;
    compare_cold_text(&text, pixels, scratch);
    text.columns = 40; text.rows = 50; text.glyph_height = 8;
    compare_cold_text(&text, pixels, scratch);
    uint8_t *second_font = calloc(1, 256 * 128);
    text.font[1] = second_font; palette[8] = 0x00ff00;
    ram[text.start_address] = 'A'; ram[text.start_address + 1] = 8;
    second_font['A' * 128] = 0xf0;
    compare_cold_text(&text, pixels, scratch);
    second_font['A' * 128 + 4] = 0x0f;
    compare_cold_text(&text, pixels, scratch);
    text.columns = 80; text.rows = 100;
    compare_cold_text(&text, pixels, scratch);
    /* Returning from graphics overwrites the complete output, including bars. */
    memset(pixels, 255, 480 * 300); tiny_text_reset(scratch);
    compare_cold_text(&text, pixels, scratch);
    text.columns = 0; assert(!tiny_render_text(&text, pixels, 480, 300, scratch));
    free(second_font); free(ram); free(font); free(pixels); free(scratch);
}

int main(void)
{
    test_retrace_without_frequent_rendering();
    test_native_text();
    /* Boot a synthetic floppy through the actual embedded SeaBIOS. The guest
     * writes a marker to the BIOS debug port and halts; no DOS fixture required. */
    size_t n = 0;
    const uint8_t init[] = {0xba, 0x02, 0x04}; /* mov dx, 0x402 */
    memcpy(disk, init, sizeof(init)); n = sizeof(init);
    /* Actual VGA BIOS font, including narrow letters and punctuation. */
    const uint8_t video_init[] = {0xb8,0x03,0x00,0xcd,0x10,0xbb,0x07,0x00};
    memcpy(disk + n, video_init, sizeof(video_init)); n += sizeof(video_init);
    const char *screen = "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789\r\nilft [] <> + -=/\\|\r\n";
    while (*screen) {
        disk[n++] = 0xb4; disk[n++] = 0x0e; disk[n++] = 0xb0;
        disk[n++] = (uint8_t)*screen++; disk[n++] = 0xcd; disk[n++] = 0x10;
    }
    const char *marker = "SOLAROS BOOT OK\n";
    while (*marker) { disk[n++] = 0xb0; disk[n++] = (uint8_t)*marker++; disk[n++] = 0xee; }
    const uint8_t keys[] = {'A', '>', 3};
    for (unsigned i = 0; i < sizeof(keys); ++i) {
        const uint8_t wait_key[] = {0x31,0xc0,0xcd,0x16,0x3c,keys[i],0x75,0xf8};
        memcpy(disk + n, wait_key, sizeof(wait_key)); n += sizeof(wait_key);
    }
    marker = "KEYBOARD OK\n";
    while (*marker) { disk[n++] = 0xb0; disk[n++] = (uint8_t)*marker++; disk[n++] = 0xee; }
    /* Guest INT 13h writes its boot sector into the second floppy sector. */
    const uint8_t write_sector[] = {0x31,0xc0,0x8e,0xc0,0xbb,0x00,0x7c,
        0xb8,0x01,0x03,0xb9,0x02,0x00,0x31,0xd2,0xcd,0x13};
    memcpy(disk + n, write_sector, sizeof(write_sector)); n += sizeof(write_sector);
    disk[n++] = 0xfa; disk[n++] = 0xf4; disk[n++] = 0xeb; disk[n++] = 0xfd;
    assert(n < 510);
    disk[510] = 0x55; disk[511] = 0xaa;
    const char *fixture_path = getenv("TINY386_TEST_DISK");
    if (fixture_path) {
        FILE *fixture = fopen(fixture_path, "wb"); assert(fixture);
        assert(fwrite(disk, 1, sizeof(disk), fixture) == sizeof(disk)); fclose(fixture);
    }
    char *argv[] = {"tiny386", "/test.img"};
    assert(tiny386_main(2, argv) == 0);
    assert(booted && keyboard_ok && frames >= 2 && suspends == 1 && allocations == 0 && exit_code == 0);
    assert(saved >= 1 && writes == 1 && !memcmp(disk, disk + 512, 512));
    assert(!open_disks[0] && !open_disks[1]);
    /* The same boot sector runs through SeaBIOS's real IDE path. DL=80h
     * selects the hard disk for the guest INT 13h sector write. */
    memcpy(harddisk, disk, 512);
    size_t write_offset = n - 4 - sizeof(write_sector);
    assert(!memcmp(harddisk + write_offset, write_sector, sizeof(write_sector)));
    memmove(harddisk + write_offset + 16, harddisk + write_offset + 15, 510 - write_offset - 16);
    harddisk[write_offset + 13] = 0xba; harddisk[write_offset + 14] = 0x80; harddisk[write_offset + 15] = 0;
    booted = keyboard_ok = false; suspends = frames = keys_sent = writes = 0;
    char *hard_argv[] = {"tiny386", "--hdd", "/hard.img"};
    assert(tiny386_main(3, hard_argv) == 0);
    assert(booted && keyboard_ok && suspends == 1 && writes == 1);
    assert(!memcmp(harddisk, harddisk + 512, 512) && allocations == 0);
    assert(!open_disks[0] && !open_disks[1]);
    /* A floppy can boot with an attached IDE disk as well. */
    booted = keyboard_ok = false; suspends = frames = keys_sent = writes = 0;
    char *both_argv[] = {"tiny386", "/test.img", "--hdd", "/hard.img"};
    assert(tiny386_main(4, both_argv) == 0);
    assert(booted && keyboard_ok && writes == 1 && allocations == 0);
    assert(!open_disks[0] && !open_disks[1]);
    saved = 0;
    assert(tiny386_main(1, argv) == 1 && allocations == 0);
    fail_allocation = true;
    assert(tiny386_main(2, argv) == 1 && allocations == 0 && exit_code == 1);
    fail_allocation = false; fail_read = true; booted = keyboard_ok = false;
    suspends = frames = keys_sent = 0;
    assert(tiny386_main(2, argv) == 1 && allocations == 0);
    fail_read = false;
    fail_open = true;
    assert(tiny386_main(3, hard_argv) == 1 && allocations == 0);
    assert(!strcmp(tiny_failure, "disk image is in use; unmount it or close the other guest first"));
    fail_open = false;
    /* Bounded file operations and retryable sync failures. */
    tiny_app = &app; tiny_memory = &memory; tiny_host = &host;
    assert(!setjmp(tiny_trap));
    tiny_file *f = tiny_fopen("/test.img", "r+b"); assert(f);
    assert(tiny_fseek(f, -1, SEEK_SET) == -1);
    assert(tiny_fseek(f, sizeof(disk) + 1, SEEK_SET) == -1);
    assert(tiny_fseek(f, sizeof(disk) - 1, SEEK_SET) == 0);
    uint8_t pair[] = {1, 2};
    assert(tiny_fwrite(pair, 1, 2, f) == 0);
    assert(tiny_fseek(f, 100, SEEK_SET) == 0);
    assert(tiny_fwrite(pair, 1, 2, f) == 2);
    assert(disk[100] == 1 && disk[101] == 2); /* Write-through, before close. */
    fail_save = true; assert(tiny_save_disks() != 0);
    fail_save = false; assert(tiny_save_disks() == 0);
    assert(tiny_fclose(f) == 0 && allocations == 0);
    f = tiny_fopen("/hard.img", "r+b"); assert(f);
    assert(tiny_fseek(f, 32U * 1024U * 1024U - 1, SEEK_SET) == 0);
    assert(tiny_fread(pair, 1, 2, f) == 1 && pair[0] == 0);
    assert(tiny_fseek(f, 0, SEEK_SET) == 0);
    fail_write = true;
    if (!setjmp(tiny_trap)) { (void)tiny_fwrite(pair, 1, 2, f); assert(0); }
    fail_write = false;
    tiny_cleanup();
    assert(!open_disks[0] && !open_disks[1] && allocations == 0);
    puts("Tiny386 real floppy and IDE BIOS boot, guest writes, suspend and disk failures passed");
    return 0;
}
