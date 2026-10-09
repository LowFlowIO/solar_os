#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_abi.h"
#include "machine.h"
#include "vendor/puce6502.h"

extern int apple2_vm_entry(int argc, char **argv);
static unsigned allocations, frees, reads, frames, closes;
static int failure, exit_code;
static uint64_t time_us;
static bool key_sent;
static bool stats_sent;
static char last_report[256];
static const char *assets;
static bool booted;
static uint32_t sleep_quantum_us = 1000U;
static uint64_t observed_ticks, last_poll_us, longest_poll_gap_us;
static unsigned asset_frames;
static bool model_cpu_cost;
static unsigned stop_case, audio_writes;
static bool audio_live;
static uint16_t text_foreground, text_background;
static FILE *asset_file(const char *path)
{
    char resolved[1024];
    const char *name = strstr(path, "appleII+.rom") ? "rom/appleII+.rom" :
        (strstr(path, "diskII.rom") ? "rom/diskII.rom" : "DOS 3.3.nib");
    assert(snprintf(resolved, sizeof(resolved), "%s/%s", assets, name) < (int)sizeof(resolved));
    return fopen(resolved, "rb");
}
static void *allocate(size_t bytes) { allocations++; return malloc(bytes); }
static void release(void *data) { assert(!audio_live); frees++; free(data); }
static int report(const char *text, size_t length)
{
    assert(text && length && length < sizeof(last_report));
    memcpy(last_report, text, length); last_report[length] = '\0'; return 0;
}
static uint64_t now(void)
{
    if (model_cpu_cost) {
        /* Model an emulator processing ten guest cycles per microsecond. */
        time_us += (ticks - observed_ticks) / 10U;
        observed_ticks = ticks;
    }
    return time_us;
}
static void sleep_ms(uint32_t ms)
{
    assert(ms);
    time_us += ((ms * 1000U + sleep_quantum_us - 1U) / sleep_quantum_us) * sleep_quantum_us;
}
static void set_exit(int code) { exit_code = code; }
static int file_size(const char *path, uint32_t *size)
{
    if (assets) {
        FILE *file = asset_file(path); assert(file && !fseek(file, 0, SEEK_END));
        *size = (uint32_t)ftell(file); fclose(file); return 0;
    }
    if (strstr(path, "appleII+.rom")) {
        if (failure == 1) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        *size = 12288;
    } else if (strstr(path, "diskII.rom")) {
        if (failure == 2) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        *size = 256;
    } else {
        if (failure == 5) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        *size = failure == 3 ? 1 : APPLE2_NIB_SIZE;
    }
    return 0;
}
static int read_file(const char *path, uint32_t offset, void *buffer,
    size_t length, size_t *actual)
{
    assert(length <= 65536U); reads++;
    if (assets) {
        FILE *file = asset_file(path); assert(file && !fseek(file, offset, SEEK_SET));
        *actual = fread(buffer, 1, length, file); assert(!ferror(file));
        fclose(file); return 0;
    }
    if (strstr(path, "disk.nib") && failure == 6) return SOLAR_OS_NATIVE_ERROR_FAILED;
    memset(buffer, 0, length); *actual = length;
    if (strstr(path, "disk.nib") && failure == 7 && offset == 65536U) *actual = length - 1U;
    if (strstr(path, "appleII+.rom")) {
        assert(offset == 0 && length == 12288);
        /* ROM program echoes the keyboard latch to RAM and clears its strobe. */
        static const uint8_t rom[] = {
            0xad, 0x00, 0xc0, 0x8d, 0x00, 0x03,
            0xad, 0x10, 0xc0, 0x4c, 0x00, 0xd0,
        };
        if (failure == 8 || failure == 9 || stop_case == 3) {
            static const uint8_t motor_on[] = {0xa9, 0x00, 0x8d, 0xe9, 0xc0};
            memcpy(buffer, motor_on, sizeof(motor_on));
            memcpy((uint8_t *)buffer + sizeof(motor_on), rom, sizeof(rom));
        } else memcpy(buffer, rom, sizeof(rom));
        ((uint8_t *)buffer)[0x2ffc] = 0;
        ((uint8_t *)buffer)[0x2ffd] = 0xd0;
    }
    return 0;
}
static int video_open(uint16_t width, uint16_t height, const uint16_t *palette, uint16_t fps)
{
    assert(width == 280 && height == 192 && palette[15] == 0xffff && fps == 25);
    assert(palette[0] == 0 && palette[1] == 0x984cU);
    assert(palette[APPLE2_TEXT_FOREGROUND] == text_foreground);
    assert(palette[APPLE2_TEXT_BACKGROUND] == text_background);
    return failure == 4 ? SOLAR_OS_NATIVE_ERROR_NO_MEMORY : 0;
}
static int text_colors(uint16_t *foreground, uint16_t *background)
{
    text_foreground = stop_case > 1 ? 0x07e0U : 0x0000U;
    text_background = stop_case > 1 ? 0x0000U : 0xffffU;
    *foreground = text_foreground; *background = text_background; return 0;
}
static int video_open_ex(uint16_t width, uint16_t height, const uint16_t *palette,
    uint16_t fps, uint8_t background_index)
{
    assert(background_index == APPLE2_TEXT_BACKGROUND);
    return video_open(width, height, palette, fps);
}
static int present(const uint8_t *pixels, size_t length)
{
    assert(pixels && length == 280U * 192U);
    if (stop_case) {
        frames++;
        if (stop_case == 2) assert(apple2_machine->paused);
        if (stop_case == 4) assert(apple2_machine->show_help);
        if (stop_case == 5 && frames >= 3) assert(!apple2_machine->show_help);
        return 0;
    }
    if (assets) {
        asset_frames++;
        char screen[24 * 41 + 1]; size_t n = 0;
        for (unsigned row = 0; row < 24; ++row) {
            unsigned offset = (row & 7U) * 128U + (row >> 3) * 40U;
            for (unsigned col = 0; col < 40; ++col) {
                uint8_t ch = apple2_machine->ram[0x400 + offset + col] & 0x7fU;
                screen[n++] = (char)(ch < 32 ? ch + 64 : ch);
            }
            screen[n++] = '\n';
        }
        screen[n] = '\0';
        if (strstr(screen, "DOS VERSION") && strstr(screen, "\n]")) {
            booted = true;
            printf("Module booted DOS 3.3 to BASIC: %llu guest cycles, %.2f simulated seconds (%u us tick, %u frames)\n",
                ticks, (double)time_us / 1000000U, sleep_quantum_us, asset_frames);
            assert(asset_frames < time_us / 40000U / 2U);
            return SOLAR_OS_NATIVE_ERROR_FAILED;
        }
        assert(time_us < (sleep_quantum_us == 1000U ? 25000000U : 45000000U));
        return 0;
    }
    if (++frames == 4) {
        assert(apple2_machine->ram[0x300] == 'A');
        if (failure == 8) assert(apple2_machine->show_stats);
        if (failure == 9) assert(!apple2_machine->fast_disk);
        return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
    return 0;
}
static int read_key(solar_os_native_vm_key_v1_t *key, bool *available)
{
    memset(key, 0, sizeof(*key));
    if (assets) {
        if (model_cpu_cost && time_us > 1100000U) {
            assert(apple2_machine->cpu_khz >= 9900U && apple2_machine->cpu_khz < 10200U);
            assert(apple2_machine->guest_khz > 0U &&
                apple2_machine->guest_khz <= apple2_machine->cpu_khz);
        }
        uint64_t gap = time_us - last_poll_us;
        if (gap > longest_poll_gap_us) longest_poll_gap_us = gap;
        last_poll_us = time_us;
        *available = false; return 0;
    }
    /* Change visible RAM independently of the keyboard echo program. */
    apple2_machine->ram[0x401] = (uint8_t)(0x80U | (time_us / 40000U & 0x3fU));
    if (stop_case && !key_sent) {
        *available = true;
        key->key = stop_case == 2 ? 0x8f : (stop_case >= 4 ? 0x91 : 'a');
        key_sent = true;
        return 0;
    }
    if (stop_case == 5 && time_us >= 80000U && !stats_sent) {
        *available = true; key->usage = 0x45; stats_sent = true; return 0;
    }
    if ((failure == 8 || failure == 9) && key_sent && !stats_sent) {
        *available = true; key->key = failure == 8 ? 0x87 : 0x88;
        stats_sent = true; return 0;
    }
    *available = !key_sent; key->key = 'a'; key_sent = true;
    return 0;
}
static int audio_open(uint32_t *rate, uint8_t *channels)
{
    if (stop_case) { *rate = 48000; *channels = 1; audio_live = true; return 0; }
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
}
static int audio_write(const int16_t *samples, size_t count)
{
    assert(audio_live && samples && count); audio_writes++; return 0;
}
static void audio_close(void) { audio_live = false; closes++; }
static bool stop_requested(void) { return stop_case && time_us >= 120000U; }
static void restart(void) { assert(!"module must not restart the device"); }
static const solar_os_native_vm_api_v1_t vm_api = {
    .abi_version = 1, .struct_size = sizeof(vm_api),
    .time_us = now, .sleep_ms = sleep_ms, .video_open = video_open,
    .present = present, .read_key = read_key, .file_size = file_size,
    .read_file = read_file, .audio_open = audio_open, .audio_close = audio_close,
    .audio_write = audio_write, .set_exit_code = set_exit,
    .stop_requested = stop_requested, .restart = restart,
    .text_colors = text_colors, .video_open_ex = video_open_ex,
};
static const solar_os_native_memory_api_v1_t memory_api = {
    .abi_version = 1, .struct_size = sizeof(memory_api), .alloc = allocate, .free = release,
};
static const void *get_service(const char *name, uint32_t abi, uint32_t size)
{
    assert(abi == 1);
    if (!strcmp(name, "vm")) { assert(size == sizeof(vm_api)); return &vm_api; }
    assert(!strcmp(name, "memory") && size == sizeof(memory_api)); return &memory_api;
}
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void)
{
    static const solar_os_native_host_api_v1_t host = {
        .abi_version = 1, .struct_size = sizeof(host), .write_utf8 = report,
        .get_service = get_service,
    };
    return &host;
}
int main(int argc, char **test_argv)
{
    char *argv[] = {"apple2", "/assets", "disk.nib"};
    for (failure = 1; failure <= 7; ++failure) {
        exit_code = 0;
        assert(apple2_vm_entry(3, argv) == 1 && exit_code == 1);
        assert(allocations == frees && apple2_machine == NULL);
        if (failure == 3) assert(strstr(last_report, "got 1, expected 232960") &&
            strstr(last_report, "/assets/disk.nib"));
        if (failure == 4) assert(strstr(last_report, "display setup ran out of memory"));
        if (failure == 5) assert(strstr(last_report, "file not found") &&
            strstr(last_report, "/assets/disk.nib"));
        if (failure == 6) assert(strstr(last_report, "read failed") &&
            strstr(last_report, "/assets/disk.nib"));
        if (failure == 7) assert(strstr(last_report, "short read") &&
            strstr(last_report, "got 65535, expected 65536"));
    }
    failure = 0; reads = 0;
    assert(apple2_vm_entry(3, argv) == 1 && exit_code == 1);
    assert(reads == 6 && frames == 4 && key_sent && time_us >= 120000);
    assert(allocations == frees && closes == 8 && apple2_machine == NULL);
    failure = 8; frames = 0; time_us = 0; key_sent = false;
    assert(apple2_vm_entry(3, argv) == 1 && ticks > 500000U);
    assert(allocations == frees && closes == 9 && apple2_machine == NULL);
    failure = 9; frames = 0; time_us = 0; key_sent = stats_sent = false;
    sleep_quantum_us = 10000U;
    assert(apple2_vm_entry(3, argv) == 1 && ticks > 100000U && ticks < 140000U);
    assert(allocations == frees && closes == 10 && apple2_machine == NULL);
    sleep_quantum_us = 1000U;
    /* Repeat real module entry/cleanup with active audio, pause, disk turbo,
     * help open and help dismissed. Every stop must return successfully. */
    failure = 0;
    for (unsigned run = 0; run < 20; ++run) {
        stop_case = run % 5U + 1U;
        frames = 0; time_us = 0; key_sent = stats_sent = false; audio_writes = 0;
        unsigned before_closes = closes;
        exit_code = 1;
        assert(apple2_vm_entry(3, argv) == 0 && exit_code == 0);
        assert(allocations == frees && apple2_machine == NULL && !audio_live);
        assert(closes == before_closes + 1U && frames >= 2U);
        assert(time_us >= 120000U && time_us < 130000U);
        if (stop_case == 1 || stop_case >= 4) assert(audio_writes);
        assert(!strcmp(last_report, "apple2: stopped"));
    }
    stop_case = 0;
    if (argc == 2) {
        assets = test_argv[1]; failure = 0; time_us = 0;
        asset_frames = 0;
        assert(apple2_vm_entry(3, argv) == 1 && booted && exit_code == 1);
        assert(allocations == frees && apple2_machine == NULL);
        assert(time_us < 8000000U);
        for (sleep_quantum_us = 1000U; sleep_quantum_us <= 10000U; sleep_quantum_us *= 10U) {
            time_us = observed_ticks = last_poll_us = longest_poll_gap_us = 0;
            booted = false; model_cpu_cost = true;
            asset_frames = 0;
            assert(apple2_vm_entry(3, argv) == 1 && booted && exit_code == 1);
            assert(time_us < (sleep_quantum_us == 1000U ? 25000000U : 45000000U));
            assert(longest_poll_gap_us < 25000U);
            assert(allocations == frees && apple2_machine == NULL);
        }
    }
    puts("Apple II VM entry, chunked assets, input and cleanup tests passed");
    return 0;
}
