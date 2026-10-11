#define TINY386_PORT_IMPLEMENTATION
#include "port.h"
#include <assert.h>
#include <sys/time.h>

int tiny386_main(int argc, char **argv);
uint32_t get_uticks(void);
static uint8_t disk[1440 * 1024];
static bool booted, fail_allocation, fail_read, fail_save;
static unsigned allocations, frames, suspends, saved;
static int exit_code;
static uint64_t boot_time, wall_offset;
static uint32_t guest_before_suspend;

static uint64_t now(void)
{
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
    return 0;
}
static int size_of_file(const char *path, uint32_t *n)
{ if (strcmp(path, "/test.img")) return -5; *n = sizeof(disk); return 0; }
static int read_image(const char *path, uint32_t offset, void *out, size_t n, size_t *got)
{
    if (fail_read || strcmp(path, "/test.img") || offset > sizeof(disk) || n > sizeof(disk) - offset) return -1;
    memcpy(out, disk + offset, n); *got = n; return 0;
}
static int replace_image(const char *path, const void *in, size_t n)
{ assert(!strcmp(path, "/test.img") && n == sizeof(disk)); ++saved; if (fail_save) return -1; memcpy(disk, in, n); return 0; }
static int resolve(const char *path, char *out, size_t n)
{ if (strlen(path) >= n) return -1; strcpy(out, path); return 0; }
static int open_video(uint16_t w, uint16_t h, const uint16_t *pal, uint16_t fps, uint8_t bg)
{ assert(w == 640 && h == 400 && pal[0] == 0 && pal[255] == 0xffff && fps == 15 && bg == 0); return 0; }
static int present_frame(const uint8_t *pixels, size_t n)
{
    assert(pixels && n == 640 * 400); ++frames;
    if (suspends) assert((uint32_t)(get_uticks() - guest_before_suspend) < 1000000);
    return 0;
}
static bool stop(void) { return booted && suspends && now() - boot_time > 150000; }
static bool suspend_requested(void) { return booted && !suspends; }
static int suspend_app(void) { guest_before_suspend = get_uticks(); ++suspends; wall_offset += 5000000; boot_time += 5000000; return 0; }
static int enable_suspend(void) { return 0; }
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{ (void)key; *available = false; return 0; }
static void sleep_ms(uint32_t n) { usleep(n * 1000); }
static void set_exit(int n) { exit_code = n; }
static const solar_os_native_foreground_api_v1_t app = {
    .abi_version = 1, .struct_size = sizeof(app), .time_us = now, .sleep_ms = sleep_ms,
    .video_open_ex = open_video, .present = present_frame, .read_key = read_key,
    .file_size = size_of_file, .read_file = read_image, .replace_file = replace_image,
    .resolve_path = resolve, .stop_requested = stop, .enable_suspend = enable_suspend,
    .suspend_requested = suspend_requested, .suspend = suspend_app, .set_exit_code = set_exit,
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

int main(void)
{
    /* Boot a synthetic floppy through the actual embedded SeaBIOS. The guest
     * writes a marker to the BIOS debug port and halts; no DOS fixture required. */
    size_t n = 0;
    const uint8_t init[] = {0xba, 0x02, 0x04}; /* mov dx, 0x402 */
    memcpy(disk, init, sizeof(init)); n = sizeof(init);
    const char *marker = "SOLAROS BOOT OK\n";
    while (*marker) { disk[n++] = 0xb0; disk[n++] = (uint8_t)*marker++; disk[n++] = 0xee; }
    disk[n++] = 0xfa; disk[n++] = 0xf4; disk[n++] = 0xeb; disk[n++] = 0xfd;
    disk[510] = 0x55; disk[511] = 0xaa;
    char *argv[] = {"tiny386", "/test.img"};
    assert(tiny386_main(2, argv) == 0);
    assert(booted && frames >= 2 && suspends == 1 && allocations == 0 && exit_code == 0);
    assert(tiny386_main(1, argv) == 1 && allocations == 0);
    fail_allocation = true;
    assert(tiny386_main(2, argv) == 1 && allocations == 0 && exit_code == 1);
    fail_allocation = false; fail_read = true;
    assert(tiny386_main(2, argv) == 1 && allocations == 0);
    fail_read = false;
    /* Bounded file operations and retryable save failures. */
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
    fail_save = true; assert(tiny_save_disks() != 0 && saved == 1);
    fail_save = false; assert(tiny_save_disks() == 0 && saved == 2 && disk[100] == 1 && disk[101] == 2);
    assert(tiny_fclose(f) == 0 && allocations == 0);
    tiny_cleanup();
    puts("Tiny386 real BIOS boot, suspend clock, cleanup, allocation and disk failures passed");
    return 0;
}
