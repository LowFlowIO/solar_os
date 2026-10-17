// SPDX-License-Identifier: MIT
#include "port.h"
#include "a500.h"
#include "rtc.h"
#include "keyboard.h"
#include <atomic>
extern "C" {
#include "m68k.h"
}
extern uint8_t *slow_ram;
extern void cia_tod_hsync(void), cia_tod_vsync(void);
extern void solar_os_amiga_drive_reset(void), solar_os_amiga_input_reset(void);
extern void solar_os_amiga_mouse_move(int dx, int dy);
extern void solar_os_amiga_audio_rate(uint32_t rate);

static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_foreground_api_v1_t *app;
static const solar_os_native_memory_api_v1_t *memory;
static void *allocations[16];
static unsigned allocation_count;
static uint8_t *pixels, *frame;
static bool paused, mouse_mode, held[7], audio, snapshot_requested;
static bool keyboard_mode;
static uint16_t fit_x[1024], fit_width, fit_height, fit_source_width, fit_source_height;
static int fit_left, fit_top, fit_start;
static bool canvas_started;
static uint64_t pulse_until[7];
static char snapshot_path[256];
static int16_t *ring;
static std::atomic<uint32_t> ring_read, ring_write, audio_dropped, audio_underruns;
static uint32_t audio_fraction, audio_step;
static int audio_last_left, audio_last_right;
static unsigned audio_fade, audio_start_fade;
static bool audio_ready;
static uint32_t display_frames, display_replaced;
static constexpr unsigned ring_size = 4096;
static uint16_t width, height;
static uint64_t guest_clock_us;
static uint64_t started_at, measured_cpu, measured_video, measured_chips, measured_audio;
static unsigned measured_frames;
struct video_pacing {
    uint32_t cost[2] = {};
    unsigned interval = 2, remaining = 0;
    bool begin(void) {
        if (remaining) { --remaining; return false; }
        remaining = interval - 1;
        return true;
    }
    void end(uint32_t elapsed, bool drawn) {
        uint32_t &average = cost[drawn];
        average = average ? (average * 3 + elapsed) / 4 : elapsed;
        if (!cost[0] || !cost[1]) return;
        uint32_t extra = cost[1] > cost[0] ? cost[1] - cost[0] : 0;
        uint32_t available = cost[0] < 20000 ? 20000 - cost[0] : 1;
        interval = (extra + available - 1) / available;
        if (interval < 2) interval = 2;
        if (interval > 8) interval = 8;
    }
};
struct pointer_state_t {
    bool used, anchored, moved, dragging;
    uint8_t source, id, buttons;
    int16_t x, y, start_x, start_y;
    uint64_t pressed_at;
};
static pointer_state_t pointers[4];
static int pointer_dx, pointer_dy;
static unsigned touch_clicks, touch_click_phase;
int cur_frame;

SOLAR_OS_NATIVE_APP_MANIFEST = {
    SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC, SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    sizeof(solar_os_native_app_manifest_v2_t), SOLAR_OS_NATIVE_APP_CLASS_GUI,
    SOLAR_OS_NATIVE_APP_RESUMABLE | SOLAR_OS_NATIVE_APP_POINTER_EVENTS, 16U * 1024U, 1, 8U * 1024U,
    3U * 1024U * 1024U, 5, "amiga", "Amiga 500 (OCS, 1 MiB RAM)", ".rom"
};

static void report(const char *text) { host->write_utf8(text, strlen(text)); }
/* Workbench 1.3 probes seconds across a 75-tick guest delay. At reduced
 * emulation speed this can take exactly ten host seconds and falsely report
 * no clock. Keep the running RTC and guest timer on the same PAL timebase;
 * while paused/suspended, account for elapsed host time instead. */
uint64_t solar_os_amiga_clock_us(void) { return guest_clock_us; }
bool solar_os_amiga_clock_calendar(uint16_t *year, uint8_t *fields)
{
    solar_os_native_foreground_calendar_v1_t c = {};
    if (app->calendar_read(&c) || c.year < 1978 || c.month < 1 || c.month > 12 ||
        c.day < 1 || c.day > 31 || c.weekday > 6 || c.hour > 23 || c.minute > 59 || c.second > 59) return false;
    *year = c.year;
    fields[0] = c.month; fields[1] = c.day; fields[2] = c.weekday;
    fields[3] = c.hour; fields[4] = c.minute; fields[5] = c.second;
    return true;
}
extern "C" void *solar_os_amiga_alloc(size_t bytes)
{
    if (allocation_count == sizeof(allocations) / sizeof(allocations[0])) return nullptr;
    void *p = memory->alloc(bytes);
    if (p) allocations[allocation_count++] = p;
    return p;
}
extern "C" void *solar_os_amiga_alloc_internal(size_t bytes)
{
    if (allocation_count == sizeof(allocations) / sizeof(allocations[0]) ||
        memory->struct_size < offsetof(solar_os_native_memory_api_v1_t, alloc_internal) + sizeof(memory->alloc_internal) ||
        !memory->alloc_internal) return nullptr;
    void *p = memory->alloc_internal(bytes);
    if (p) allocations[allocation_count++] = p;
    return p;
}
extern "C" void solar_os_amiga_audio_sample(int16_t left, int16_t right)
{
    if (!audio) return;
    uint32_t write = ring_write.load(std::memory_order_relaxed);
    if (write - ring_read.load(std::memory_order_acquire) == ring_size) {
        audio_dropped.store(audio_dropped.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        return;
    }
    unsigned i = (write & (ring_size - 1)) * 2;
    ring[i] = left; ring[i + 1] = right;
    ring_write.store(write + 1, std::memory_order_release);
}
static void render_audio(int16_t *out, size_t frames, uint32_t rate, void *)
{
    (void)rate;
    // This is a single-producer/single-consumer queue. Only its published
    // indices cross tasks; the emulator never waits on the audio callback.
    uint32_t read = ring_read.load(std::memory_order_relaxed);
    uint32_t write = ring_write.load(std::memory_order_acquire);
    constexpr unsigned target = 1536;
    if (!audio_ready && write - read >= target) {
        audio_ready = true; audio_fraction = 0; audio_step = 65536;
        audio_start_fade = 32;
    }
    if (audio_ready) {
        // A slower guest produces fewer samples per host second. Follow that
        // rate smoothly instead of cutting each fragment off with silence.
        int desired = (write - read) * 65536U / target;
        if (desired < 4096) desired = 4096;
        if (desired > 131072) desired = 131072;
        audio_step += (desired - (int)audio_step) / 4;
    }
    for (size_t i = 0; i < frames; ++i) {
        int16_t l = 0, r = 0;
        if (audio_ready && write - read < 2) {
            audio_ready = false; audio_fade = 32;
            audio_underruns.store(audio_underruns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        if (audio_ready) {
            unsigned a = (read & (ring_size - 1)) * 2, b = ((read + 1) & (ring_size - 1)) * 2;
            unsigned fraction = audio_fraction >> 8;
            l = ring[a] + ((int)ring[b] - ring[a]) * (int)fraction / 256;
            r = ring[a + 1] + ((int)ring[b + 1] - ring[a + 1]) * (int)fraction / 256;
            audio_fraction += audio_step;
            unsigned advance = audio_fraction >> 16;
            audio_fraction &= 65535;
            if (advance >= write - read) advance = write - read - 1;
            read += advance;
            if (audio_start_fade) {
                l = (int)l * (32 - audio_start_fade) / 32;
                r = (int)r * (32 - audio_start_fade) / 32;
                --audio_start_fade;
            }
            audio_last_left = l; audio_last_right = r;
        } else if (audio_fade) {
            l = audio_last_left * (int)audio_fade / 32;
            r = audio_last_right * (int)audio_fade / 32;
            --audio_fade;
        }
        out[2 * i] = l; out[2 * i + 1] = r;
    }
    ring_read.store(read, std::memory_order_release);
}
static void reset_audio_queue(void)
{
    ring_read = ring_write = 0;
    audio_dropped = audio_underruns = 0;
    audio_ready = false; audio_fraction = audio_fade = audio_start_fade = 0;
    audio_last_left = audio_last_right = 0; audio_step = 65536;
}
static void open_audio(void)
{
    reset_audio_queue();
    audio = false;
    if (!ring) return;
    uint32_t rate = 0; uint8_t channels = 0;
    if (app->audio_open(&rate, &channels)) return;
    app->audio_close();
    if (rate < 8000 || rate > 96000 || !channels || channels > 2) return;
    solar_os_amiga_audio_rate(rate);
    audio = !app->synth_open(render_audio, nullptr, 256, rate);
}
static uint8_t color_index(uint16_t rgb)
{
    return ((rgb >> 4) & 0xe0) | ((rgb >> 3) & 0x1c) | ((rgb >> 2) & 3);
}
static void fit_geometry(int count)
{
    if (fit_source_width == count) return;
    // Fit the PAL monitor raster, not the moving Workbench playfield. A screen
    // dragged down changes its position inside this fixed 256-row viewport;
    // neither DIW writes nor the mouse can change the vertical scale.
    fit_source_width = count; fit_source_height = 256; fit_start = 44;
    fit_width = width; fit_height = (unsigned)width * 3 / 4;
    if (fit_height > height) { fit_height = height; fit_width = (unsigned)height * 4 / 3; }
    fit_left = (width - fit_width) / 2; fit_top = (height - fit_height) / 2;
    for (unsigned x = 0; x < fit_width; ++x) fit_x[x] = x * count / fit_width;
}
static void begin_canvas(void)
{
    if (canvas_started) return;
    memcpy(pixels, frame, (size_t)width * height);
    canvas_started = true;
}
static void render_line(int line, const uint16_t *row, int count)
{
    fit_geometry(count);
    int source_y = line - fit_start;
    if (source_y < 0 || source_y >= fit_source_height) return;
    int first = (source_y * fit_height + fit_source_height - 1) / fit_source_height;
    int last = ((source_y + 1) * fit_height + fit_source_height - 1) / fit_source_height;
    if (first == last) return;
    begin_canvas();
    uint8_t *out = pixels + (size_t)(first + fit_top) * width + fit_left;
    for (unsigned x = 0; x < fit_width; ++x) out[x] = color_index(row[fit_x[x]]);
    for (int y = first + 1; y < last; ++y) memcpy(out + (size_t)(y - first) * width, out, fit_width);
}
static void put16(uint8_t *p, uint16_t value) { p[0] = value >> 8; p[1] = value; }
static void put32(uint8_t *p, uint32_t value) { put16(p, value >> 16); put16(p + 2, value); }
static void put64(uint8_t *p, uint64_t value) { put32(p, value >> 32); put32(p + 4, value); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static bool valid_rom(void)
{
    uint32_t checksum = 0;
    for (unsigned i = 0; i < ROM_SIZE; i += 4) {
        uint32_t previous = checksum;
        checksum += get32(kick_rom + i);
        if (checksum < previous) ++checksum;
    }
    /* Kickstart uses the reset SSP longword for ROM ID + JMP ($11114ef9),
     * then initializes its real stack in ColdStart. It is deliberately not
     * a RAM pointer. Validate the ROM checksum and mapped reset PC only. */
    uint32_t pc = get32(kick_rom + 4);
    return checksum == UINT32_MAX && !(pc & 1) && pc >= 0xf80000 && pc < 0x1000000;
}
static void diagnostic_rom(void)
{
    /* Original 68000 program: leave overlay, count in Chip RAM, display a
     * changing COLOR00, and record JOY1DAT. No Commodore firmware is used. */
    memset(kick_rom, 0, ROM_SIZE);
    put32(kick_rom, 0x40000); put32(kick_rom + 4, 0xfc0010);
    static const uint16_t code[] = {
        0x13fc,0x0003,0x00bf,0xe201, 0x13fc,0x0002,0x00bf,0xe001,
        0x7000,                         // moveq #0,d0
        0x5240,                         // addq.w #1,d0
        0x33c0,0x0000,0x0400,          // move.w d0,$400
        0x33c0,0x00df,0xf180,          // move.w d0,COLOR00
        0x3239,0x00df,0xf00c,          // move.w JOY1DAT,d1
        0x33c1,0x0000,0x0402,          // move.w d1,$402
        0x60e4                         // bra.s loop
    };
    for (unsigned i = 0; i < sizeof(code) / sizeof(code[0]); ++i) put16(kick_rom + 16 + 2 * i, code[i]);
}
static bool read_media(const char *path, uint8_t *buffer, uint32_t expected)
{
    uint32_t size = 0;
    if (app->file_size(path, &size) || size != expected) return false;
    for (uint32_t offset = 0; offset < size;) {
        if (app->stop_requested()) return false;
        size_t length = size - offset < 32768U ? size - offset : 32768U, actual = 0;
        if (app->read_file(path, offset, buffer + offset, length, &actual) || actual != length) return false;
        offset += length;
        app->sleep_ms(1);
    }
    return true;
}
static void reset_input(void)
{
    memset(held, 0, sizeof(held)); memset(pulse_until, 0, sizeof(pulse_until));
    solar_os_amiga_input_reset();
    solar_os_amiga_keyboard_release();
    memset(pointers, 0, sizeof(pointers)); pointer_dx = pointer_dy = 0;
    touch_clicks = touch_click_phase = 0;
}
static int bounded_motion(int n) { return n < -32767 ? -32767 : n > 32767 ? 32767 : n; }
static void pointer_input(const solar_os_native_foreground_pointer_v1_t &event)
{
    if (event.reset) {
        memset(pointers, 0, sizeof(pointers)); pointer_dx = pointer_dy = 0;
        touch_clicks = touch_click_phase = 0;
        input_set_lmb(0); input_set_rmb(0);
        return;
    }
    pointer_state_t *p = nullptr;
    for (auto &candidate : pointers)
        if (candidate.used && candidate.source == event.source && candidate.id == event.pointer_id) { p = &candidate; break; }
    if (!p) {
        for (auto &candidate : pointers) if (!candidate.used) { p = &candidate; break; }
        if (!p) p = &pointers[0];
        *p = {}; p->used = true; p->source = event.source; p->id = event.pointer_id;
    }
    int dx = event.delta_x, dy = event.delta_y;
    if (event.mode == SOLAR_OS_NATIVE_POINTER_ABSOLUTE) {
        // Touch is a touchpad: movement hovers, tap clicks, stationary hold
        // followed by movement drags. Contact alone must not grab the screen.
        if (event.action == SOLAR_OS_NATIVE_POINTER_RELEASE) {
            if (p->anchored && !p->moved && !p->dragging &&
                app->time_us() - p->pressed_at < 350000 && touch_clicks < 8) ++touch_clicks;
            *p = {};
            return;
        }
        if (!p->anchored || event.action == SOLAR_OS_NATIVE_POINTER_PRESS) {
            p->anchored = true; p->moved = p->dragging = false;
            p->start_x = p->x = event.x; p->start_y = p->y = event.y;
            p->pressed_at = app->time_us();
        }
        dx = p->anchored ? event.x - p->x : 0;
        dy = p->anchored ? event.y - p->y : 0;
        p->x = event.x; p->y = event.y; p->anchored = true;
        if (abs(p->x - p->start_x) > 4 || abs(p->y - p->start_y) > 4) p->moved = true;
        p->buttons = p->dragging ? 1 : 0;
    } else {
        p->buttons = event.buttons & 3;
        if (event.action == SOLAR_OS_NATIVE_POINTER_RELEASE && !p->buttons) *p = {};
    }
    pointer_dx = bounded_motion(pointer_dx + dx); pointer_dy = bounded_motion(pointer_dy + dy);
}
static void snapshot(void)
{
    if (!snapshot_path[0]) return;
    // Capture timing and queue state before snapshot allocation/storage can
    // stall the producer. The snapshot itself is outside normal playback.
    uint8_t perf[64] = {'A','M','G','P'};
    put32(perf + 4, cur_frame); put64(perf + 8, app->time_us() - started_at);
    put32(perf + 16, measured_frames);
    put32(perf + 20, audio_dropped.load(std::memory_order_relaxed));
    put64(perf + 24, measured_cpu); put64(perf + 32, measured_video);
    put64(perf + 40, measured_chips); put64(perf + 48, measured_audio);
    put32(perf + 56, audio_underruns.load(std::memory_order_relaxed));
    put32(perf + 60, ring_write.load(std::memory_order_acquire) - ring_read.load(std::memory_order_acquire));
    char header[32] = "P6\n", *end = header + 3;
    auto decimal = [](char *out, unsigned value) {
        char digits[5]; unsigned n = 0;
        do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
        while (n) *out++ = digits[--n];
        return out;
    };
    end = decimal(end, width); *end++ = ' '; end = decimal(end, height);
    memcpy(end, "\n255\n", 5); end += 5;
    size_t prefix = end - header;
    uint8_t *picture = (uint8_t *)memory->alloc(prefix + width * height * 3);
    if (!picture) { report("amiga: snapshot allocation failed"); return; }
    memcpy(picture, header, prefix);
    for (unsigned i = 0; i < width * height; ++i) {
        unsigned c = frame[i];
        picture[prefix + 3 * i] = (c >> 5) * 255 / 7;
        picture[prefix + 3 * i + 1] = ((c >> 2) & 7) * 255 / 7;
        picture[prefix + 3 * i + 2] = (c & 3) * 255 / 3;
    }
    int result = app->replace_file(snapshot_path, picture, prefix + width * height * 3);
    memory->free(picture);
    /* Fixed-width diagnostic sidecar, big-endian. Capture live guest and
     * input state together with the picture, including on direct displays. */
    if (!result) {
        uint8_t state[32] = {'A','M','G','A'};
        put32(state + 4, cur_frame); put32(state + 8, m68k_get_reg(nullptr, M68K_REG_PC));
        put16(state + 12, (uint16_t)(chip_ram[0x400] << 8 | chip_ram[0x401]));
        put16(state + 14, input_joy1dat()); put16(state + 16, input_joy0dat());
        state[18] = input_lmb(); state[19] = input_rmb();
        state[20] = paused; state[21] = audio; state[22] = mouse_mode;
        solar_os_native_foreground_video_stats_v1_t stats = {};
        if (!app->video_stats(&stats)) {
            display_frames += stats.frames; display_replaced += stats.replaced_frames;
            state[23] = 1; put32(state + 24, display_frames); put32(state + 28, display_replaced);
        }
        char path[sizeof(snapshot_path) + 6];
        size_t length = strlen(snapshot_path);
        memcpy(path, snapshot_path, length); memcpy(path + length, ".state", 7);
        result = app->replace_file(path, state, sizeof(state));
        memcpy(path + length, ".perf", 6);
        app->replace_file(path, perf, sizeof(perf));
    }
    report(result ? "amiga: snapshot write failed" : "amiga: guest snapshot saved (.ppm)");
}
static void input(const solar_os_native_foreground_key_v1_t &key)
{
    if (key.reset) { reset_input(); return; }
    bool release = key.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    bool press = key.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS;
    if (key.key == SOLAR_OS_KEY_F2 || key.usage == 0x3b) { if (press) snapshot_requested = true; return; }
    if ((key.key >= SOLAR_OS_KEY_F6 && key.key <= SOLAR_OS_KEY_F8) ||
        (key.usage >= 0x3f && key.usage <= 0x41)) {
        if (press) {
            reset_input();
            keyboard_mode = key.key == SOLAR_OS_KEY_F6 || key.usage == 0x3f;
            mouse_mode = key.key == SOLAR_OS_KEY_F7 || key.usage == 0x40;
            report(keyboard_mode ? "amiga: keyboard mode" : mouse_mode ? "amiga: keyboard controls mouse" : "amiga: keyboard controls joystick");
        }
        return;
    }
    if (key.key == SOLAR_OS_KEY_F10 || key.usage == 0x43) {
        if (press) {
            paused = !paused;
            app->synth_close(); audio = false;
            if (!paused) open_audio();
        }
        return;
    }
    if (keyboard_mode) { solar_os_amiga_keyboard_input(key); return; }
    int index = -1;
    if (key.key >= SOLAR_OS_KEY_UP && key.key <= SOLAR_OS_KEY_RIGHT) index = key.key - SOLAR_OS_KEY_UP;
    else if (key.usage >= 0x4f && key.usage <= 0x52) {
        static const int arrows[] = {3,2,1,0}; index = arrows[key.usage - 0x4f];
    } else if (key.key == ' ' || key.usage == 0x2c) index = 4;
    else if (key.key == '\n' || key.usage == 0x28) index = 5;
    if (index >= 0) {
        held[index] = !release;
        pulse_until[index] = !key.usage && !release ? app->time_us() + 120000 : 0;
    }
}
static void input_frame_tick(bool guest_tick = true)
{
    uint64_t now = app->time_us();
    for (unsigned i = 0; i < 7; ++i)
        if (pulse_until[i] && now >= pulse_until[i]) { held[i] = false; pulse_until[i] = 0; }
    int dx = pointer_dx < -64 ? -64 : pointer_dx > 64 ? 64 : pointer_dx;
    int dy = pointer_dy < -64 ? -64 : pointer_dy > 64 ? 64 : pointer_dy;
    pointer_dx -= dx; pointer_dy -= dy; solar_os_amiga_mouse_move(dx, dy);
    if (guest_tick) {
        if (touch_click_phase) --touch_click_phase;
        if (!touch_click_phase && touch_clicks) { --touch_clicks; touch_click_phase = 3; }
    }
    uint8_t buttons = touch_click_phase >= 2 ? 1 : 0;
    for (auto &p : pointers) {
        if (p.used && p.anchored && !p.moved && now - p.pressed_at >= 350000) {
            p.dragging = true; p.buttons = 1;
        }
        buttons |= p.buttons;
    }
    if (mouse_mode) {
        solar_os_amiga_mouse_move(6 * ((int)held[3] - held[2]), 6 * ((int)held[1] - held[0]));
        input_set_joy(0,0,0,0,0); input_set_lmb(held[4] || (buttons & 1)); input_set_rmb(held[5] || (buttons & 2));
    } else if (!keyboard_mode) {
        input_set_joy(held[0],held[1],held[2],held[3],held[4]); input_set_lmb(buttons & 1); input_set_rmb(held[5] || (buttons & 2));
    } else {
        input_set_joy(0,0,0,0,0); input_set_lmb(buttons & 1); input_set_rmb(buttons & 2);
    }
}
static int irq_ack(int) { return M68K_INT_ACK_AUTOVECTOR; }
static const char *run(int argc, char **argv)
{
    if (argc < 2 || argc > 3) return "usage: amiga <kick13.rom> [disk.adf] | amiga --self-test [snapshot.ppm]";
    bool diagnostic = strlen(argv[1]) == 11 && !memcmp(argv[1], "--self-test", 11);
    /* Normal mode: ROM and optional ADF. Diagnostic mode accepts a snapshot
     * destination, making hardware validation independent of licensed files. */
    char rom_path[256], disk_path[256];
    if (!diagnostic && app->resolve_path(argv[1], rom_path, sizeof(rom_path))) return "amiga: invalid ROM path";
    if (diagnostic && argc == 3) {
        if (app->resolve_path(argv[2], snapshot_path, sizeof(snapshot_path))) return "amiga: invalid snapshot path";
    } else if (!diagnostic) {
        if (strlen(rom_path) + 5 > sizeof(snapshot_path)) return "amiga: ROM path too long";
        memcpy(snapshot_path, rom_path, strlen(rom_path)); memcpy(snapshot_path + strlen(rom_path), ".ppm", 5);
    }
    if (app->video_dimensions(&width, &height) || width < 64 || height < 64 || width > 1024 || height > 1024)
        return "amiga: unsupported display dimensions (64..1024 required)";
    chip_ram = (uint8_t *)solar_os_amiga_alloc(CHIP_SIZE);
    slow_ram = (uint8_t *)solar_os_amiga_alloc(SLOW_SIZE);
    kick_rom = (uint8_t *)solar_os_amiga_alloc(ROM_SIZE);
    pixels = (uint8_t *)solar_os_amiga_alloc(width * height);
    frame = (uint8_t *)solar_os_amiga_alloc(width * height);
    ring = (int16_t *)solar_os_amiga_alloc(ring_size * 2 * sizeof(int16_t));
    if (!chip_ram || !slow_ram || !kick_rom || !pixels || !frame) return "amiga: not enough PSRAM";
    if (diagnostic) diagnostic_rom();
    else if (!read_media(rom_path, kick_rom, ROM_SIZE) || !valid_rom())
        return "amiga: need a raw 256 KiB Kickstart 1.3 ROM with valid checksum and reset entry";
    solar_os_amiga_drive_reset();
    if (!diagnostic && argc == 3) {
        if (app->resolve_path(argv[2], disk_path, sizeof(disk_path))) return "amiga: invalid ADF path";
        uint8_t *adf = drive_alloc_adf();
        if (!adf || !read_media(disk_path, adf, 901120)) return "amiga: need a raw 901120-byte ADF image";
        drive_mount_ready();
    }
    memset(pixels, 0, (size_t)width * height);
    memset(frame, 0, (size_t)width * height);
    solar_os_amiga_cpu_prepare();
    video_prepare();
    mem_reset(); cia_reset(); custom_reset(); paula_reset(); reset_input();
    m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(irq_ack); m68k_pulse_reset();
    denise_line_cb = render_line;
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; ++i) {
        unsigned r = (i >> 5) * 31 / 7, g = ((i >> 2) & 7) * 63 / 7, b = (i & 3) * 31 / 3;
        palette[i] = (r << 11) | (g << 5) | b;
    }
    if (app->video_open_ex(width, height, palette, 25, 0)) return "amiga: foreground video unavailable";
    if (app->enable_suspend()) return "amiga: foreground switching unavailable";
    open_audio();
    report(diagnostic ? "amiga: diagnostic 68000 guest running" : "amiga: A500 running; F6 keyboard; F7 mouse; F8 joystick; F2 snapshot; Ctrl+] exit");
    uint64_t deadline = app->time_us();
    started_at = deadline;
    unsigned line = 0, colorclocks = 0;
    uint64_t frame_started = deadline;
    video_pacing video_budget;
    while (!app->stop_requested()) {
        if (app->suspend_requested()) {
            reset_input(); app->synth_close(); audio = false;
            uint64_t suspended_at = app->time_us();
            if (app->suspend()) return app->stop_requested() ? nullptr : "amiga: resume failed";
            uint64_t elapsed = app->time_us() - suspended_at;
            guest_clock_us += elapsed; frame_started += elapsed;
            if (app->stop_requested()) break;
            reset_input(); if (!paused) open_audio();
            deadline = app->time_us();
            if (app->present(frame, width * height)) return "amiga: resume presentation failed";
        }
        for (unsigned i = 0; i < 64; ++i) {
            solar_os_native_foreground_key_v1_t key; bool available = false;
            if (app->read_key(&key, &available)) return "amiga: input failed";
            if (!available) break;
            input(key);
        }
        for (unsigned i = 0; i < 32; ++i) {
            solar_os_native_foreground_pointer_v1_t pointer; bool available = false;
            if (app->read_pointer(&pointer, &available)) return "amiga: pointer input failed";
            if (!available) break;
            pointer_input(pointer);
        }
        if (snapshot_requested) {
            uint64_t snapshot_at = app->time_us();
            input_frame_tick(false); snapshot_requested = false; snapshot();
            frame_started += app->time_us() - snapshot_at;
        }
        if (paused) {
            uint64_t paused_at = app->time_us();
            app->sleep_ms(5);
            uint64_t elapsed = app->time_us() - paused_at;
            guest_clock_us += elapsed; frame_started += elapsed;
            continue;
        }
        uint64_t batch_end = app->time_us() + 12000;
        do {
            if (line == 0) {
                frame_started = app->time_us();
                ++cur_frame; input_frame_tick(); copper_vblank(); sprite_vblank();
                if (!(cur_frame % 17)) ++measured_frames;
                denise_draw = video_budget.begin();
                if (denise_draw) {
                    canvas_started = false;
                }
            }
            vpos = line;
            if (blit_irq_pending) { blit_irq_pending = 0; intreq_set(6); }
            if (disk_irq_pending) { disk_irq_pending = 0; intreq_set(1); }
            bool measure = !(cur_frame % 17);
            uint64_t stamp = measure ? app->time_us() : 0;
            copper_run_line();
            if (measure) { uint64_t now = app->time_us(); measured_chips += now - stamp; stamp = now; }
            denise_render_line();
            if (measure) { uint64_t now = app->time_us(); measured_video += now - stamp; stamp = now; }
            m68k_execute(454);
            if (measure) { uint64_t now = app->time_us(); measured_cpu += now - stamp; stamp = now; }
            colorclocks += 454;
            paula_step(colorclocks >> 1);
            colorclocks &= 1;
            if (measure) { uint64_t now = app->time_us(); measured_audio += now - stamp; stamp = now; }
            cia_tick(45); cia_tod_hsync();
            if (measure) measured_chips += app->time_us() - stamp;
            if (++line == 312) {
                guest_clock_us += 20000;
                line = 0; cia_tod_vsync(); intreq_set(5);
                if (denise_draw && canvas_started) {
                    uint8_t *previous = frame; frame = pixels; pixels = previous;
                    if (app->present(frame, width * height)) return "amiga: display submission failed";
                }
                uint64_t now = app->time_us();
                video_budget.end(now - frame_started, denise_draw);
                deadline += 20000;
                if (deadline > now) app->sleep_ms((deadline - now) / 1000);
                // Let a skipped frame recover time spent rendering the last
                // one. Drop accumulated debt only after a prolonged stall.
                else if (now - deadline > 100000) deadline = now;
                break;
            }
        } while (app->time_us() < batch_end && !app->stop_requested() && !app->suspend_requested());
        app->sleep_ms(1);
    }
    return nullptr;
}
extern "C" int main(int argc, char **argv)
{
    host = solar_os_native_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host) || !host->get_service) return 1;
    app = (const solar_os_native_foreground_api_v1_t *)host->get_service("foreground", 1,
        offsetof(solar_os_native_foreground_api_v1_t, calendar_read) + sizeof(app->calendar_read));
    memory = (const solar_os_native_memory_api_v1_t *)host->get_service("memory", 1,
        offsetof(solar_os_native_memory_api_v1_t, alloc_internal));
    if (!app || !memory) { report("amiga: compatible foreground and memory services required"); return 1; }
    allocation_count = 0; snapshot_path[0] = 0; cur_frame = 0;
    display_frames = display_replaced = 0;
    guest_clock_us = 0;
    measured_cpu = measured_video = measured_chips = measured_audio = 0; measured_frames = 0;
    chip_ram = slow_ram = kick_rom = pixels = frame = nullptr; ring = nullptr;
    audio = paused = snapshot_requested = false; mouse_mode = false; keyboard_mode = true;
    fit_source_width = 0; canvas_started = false;
    const char *error = run(argc, argv);
    app->synth_close(); audio = false; denise_line_cb = nullptr;
    solar_os_amiga_cpu_release();
    video_release();
    while (allocation_count) memory->free(allocations[--allocation_count]);
    chip_ram = slow_ram = kick_rom = pixels = frame = nullptr; ring = nullptr;
    solar_os_amiga_drive_reset();
    if (error) report(error);
    app->set_exit_code(error ? 1 : 0);
    return error ? 1 : 0;
}
