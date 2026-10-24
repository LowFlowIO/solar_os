#include "solar_os_rfscan.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_radio_scan.h"

typedef struct {
    solar_os_radio_scan_t scan;
    solar_os_radio_scan_worker_t *worker;
    solar_os_radio_info_t radio;
    uint32_t center_hz;
    uint32_t window_ms;
    uint32_t next_render;
    uint32_t rendered_generation;
    uint32_t last_input;
    unsigned range;
    unsigned step;
    unsigned bandwidth;
    unsigned focus;
    bool band;
    bool channel_frequency;
    bool suspended;
    int pointer_x;
    int pointer_y;
    char message[80];
} rfscan_state_t;

static void *rfscan_state;
#define rfscan (*(rfscan_state_t *)rfscan_state)

static const uint32_t rfscan_steps[] = {25000, 62500, 125000, 250000};

static uint32_t rfscan_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void rfscan_result(esp_err_t err)
{
    if (err == ESP_OK) {
        rfscan.message[0] = '\0';
    } else if (rfscan.radio.claimed && err == ESP_ERR_INVALID_STATE) {
        snprintf(rfscan.message, sizeof(rfscan.message), "Busy: %s owns %s",
                 rfscan.radio.owner, rfscan.radio.name);
    } else {
        snprintf(rfscan.message, sizeof(rfscan.message), "%s", esp_err_to_name(err));
    }
}

static bool rfscan_stop(void)
{
    esp_err_t err = solar_os_radio_scan_worker_close(&rfscan.worker, &rfscan.scan);
    rfscan.scan.running = false;
    if (err != ESP_OK) {
        snprintf(rfscan.message, sizeof(rfscan.message), "%s: %s",
            rfscan.worker != NULL ? "Stopping" : "Restore failed", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void rfscan_parameters(void)
{
    static const uint32_t spans[] = {250000, 1000000, 2000000};
    static const uint32_t bandwidths[] = {0, 62500, 125000, 250000};
    solar_os_radio_scan_config_t *config = &rfscan.scan.config;
    config->mode = rfscan.band ? SOLAR_OS_RADIO_SCAN_BAND : SOLAR_OS_RADIO_SCAN_CHANNEL;
    config->interval_ms = rfscan.window_ms / SOLAR_OS_RADIO_SCAN_HISTORY;
    config->bandwidth_hz = rfscan.band ? bandwidths[rfscan.bandwidth] : 0;
    config->step_hz = rfscan_steps[rfscan.step];
    config->frequency_hz = !rfscan.band && rfscan.channel_frequency ? rfscan.center_hz : 0;
    if (!rfscan.band) {
        config->start_hz = config->end_hz = rfscan.center_hz;
        return;
    }
    if (rfscan.range < 3) {
        config->start_hz = rfscan.center_hz > spans[rfscan.range] ?
            rfscan.center_hz - spans[rfscan.range] : rfscan.center_hz;
        config->end_hz = rfscan.center_hz + spans[rfscan.range];
    } else if (rfscan.center_hz < 500000000U) {
        config->start_hz = 433050000U;
        config->end_hz = 434790000U;
    } else if (rfscan.center_hz < 890000000U) {
        config->start_hz = 863000000U;
        config->end_hz = 870000000U;
    } else {
        config->start_hz = 902000000U;
        config->end_hz = 928000000U;
    }
    while ((config->end_hz - config->start_hz) / config->step_hz >= SOLAR_OS_RADIO_SCAN_BINS &&
           rfscan.step < 3) {
        config->step_hz = rfscan_steps[++rfscan.step];
    }
    /* Display the last actual sample frequency rather than an unsampled edge. */
    config->end_hz = config->start_hz +
        (config->end_hz - config->start_hz) / config->step_hz * config->step_hz;
}

static bool rfscan_select(const char *name)
{
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    esp_err_t err = solar_os_radio_get_info(name, &info);
    if (err == ESP_OK) err = solar_os_radio_get_status(name, &status);
    if (err != ESP_OK) { rfscan_result(err); return false; }
    rfscan.radio = info;
    rfscan.center_hz = status.config.frequency_hz;
    rfscan.channel_frequency = false;
    rfscan.scan.receiver = status.config;
    strlcpy(rfscan.scan.name, name, sizeof(rfscan.scan.name));
    rfscan_parameters();
    return true;
}

static void rfscan_begin(void)
{
    if (rfscan.radio.name[0] == '\0') {
        strlcpy(rfscan.message, "No attached radio; use Radio to refresh", sizeof(rfscan.message));
        return;
    }
    if (rfscan.worker != NULL && !rfscan_stop()) return;
    solar_os_radio_status_t status;
    esp_err_t err = solar_os_radio_get_info(rfscan.radio.name, &rfscan.radio);
    if (err == ESP_OK) err = solar_os_radio_get_status(rfscan.radio.name, &status);
    if (err != ESP_OK) { rfscan_result(err); return; }
    rfscan.scan.receiver = status.config;
    if (!rfscan.band && !rfscan.channel_frequency) rfscan.center_hz = status.config.frequency_hz;
    rfscan_parameters();
    solar_os_radio_scan_config_t config = rfscan.scan.config;
    err = solar_os_radio_scan_worker_open(&rfscan.worker, rfscan.radio.name, &config);
    if (err == ESP_OK) (void)solar_os_radio_scan_worker_copy(rfscan.worker, &rfscan.scan);
    rfscan_result(err);
}

static int rfscan_y(int16_t dbm, int top, int height)
{
    int value = dbm;
    if (value < -130) value = -130;
    if (value > -30) value = -30;
    return top + (height - 1) * (-30 - value) / 100;
}

static void rfscan_button(solar_os_gfx_t *gfx, int x, int y, int width,
                           const char *text, bool selected)
{
    /* Match the standard transport controls: outlined when idle, solid inverse
     * when selected. Control states must remain distinct on a 1-bit surface. */
    solar_os_gfx_set_color(gfx, selected ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, width, 21);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, width, 21);
    solar_os_gfx_set_color(gfx, selected ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx, x + (width - (int)solar_os_gfx_text_width(gfx, text)) / 2, y + 14, text);
}

static void rfscan_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || rfscan.suspended) return;
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    solar_os_radio_scan_t *scan = &rfscan.scan;
    char text[96];
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    snprintf(text, sizeof(text), "%s  %s  %s", rfscan.radio.name[0] ? rfscan.radio.name : "No radio",
             rfscan.radio.driver, scan->running ? "LIVE" : "STOPPED");
    solar_os_gfx_text(gfx, 4, 12, text);
    rfscan_button(gfx, 4, 18, width / 2 - 6, "Channel", !rfscan.band);
    rfscan_button(gfx, width / 2 + 2, 18, width / 2 - 6, "Band", rfscan.band);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (rfscan.band) {
        snprintf(text, sizeof(text), "%.3f - %.3f MHz  %s",
                 scan->config.start_hz / 1000000.0, scan->config.end_hz / 1000000.0,
                 scan->running ? "RX paused" : "");
    } else {
        snprintf(text, sizeof(text), "%.6f MHz  %lus  %s",
                 (scan->running ? scan->receiver.frequency_hz : rfscan.center_hz) / 1000000.0,
                 (unsigned long)(rfscan.window_ms / 1000U), scan->running ?
                     (scan->restore ? "owns RX" : "observe RX") : "Start to apply");
    }
    solar_os_gfx_text(gfx, 4, 52, text);
    snprintf(text, sizeof(text), "BW %.1fkHz%s  step %.1fkHz",
             (!scan->running && scan->config.bandwidth_hz ?
                  scan->config.bandwidth_hz : scan->receiver.rx_bandwidth_hz) / 1000.0,
             rfscan.band && rfscan.bandwidth == 0 ? "*" : "", rfscan_steps[rfscan.step] / 1000.0);
    solar_os_gfx_text(gfx, 4, 65, text);
    if (scan->latest != SOLAR_OS_RADIO_SCAN_EMPTY) {
        snprintf(text, sizeof(text), "Now %d  Min %d  Peak %d dBm", scan->latest, scan->floor, scan->peak);
    } else {
        strlcpy(text, "Waiting for receiver energy", sizeof(text));
    }
    solar_os_gfx_text(gfx, 4, 79, text);

    const int left = 29, plot_width = width - left - 5;
    const int top = 88, available = height - top - 45;
    if (plot_width > 1 && available > 12) {
        const int plot_height = rfscan.band ? available / 2 : available;
        for (int level = -130; level <= -30; level += 50) {
            int y = rfscan_y(level, top, plot_height);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DOTTED);
            solar_os_gfx_line(gfx, left, y, width - 5, y);
            solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
            snprintf(text, sizeof(text), "%d", level);
            solar_os_gfx_text(gfx, 1, y + (level == -30 ? 6 : 0), text);
        }
        if (rfscan.band && scan->bins > 1) {
            for (size_t i = 1; i < scan->bins; i++) {
                int x0 = left + (i - 1) * (plot_width - 1) / (scan->bins - 1);
                int x1 = left + i * (plot_width - 1) / (scan->bins - 1);
                if (scan->peaks[i - 1] != SOLAR_OS_RADIO_SCAN_EMPTY && scan->peaks[i] != SOLAR_OS_RADIO_SCAN_EMPTY) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_DASHED);
                    solar_os_gfx_line(gfx, x0, rfscan_y(scan->peaks[i - 1], top, plot_height),
                                     x1, rfscan_y(scan->peaks[i], top, plot_height));
                    solar_os_gfx_set_line_style(gfx, SOLAR_OS_GFX_LINE_SOLID);
                }
                if (scan->levels[i - 1] != SOLAR_OS_RADIO_SCAN_EMPTY && scan->levels[i] != SOLAR_OS_RADIO_SCAN_EMPTY) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_line(gfx, x0, rfscan_y(scan->levels[i - 1], top, plot_height),
                                     x1, rfscan_y(scan->levels[i], top, plot_height));
                }
            }
            const int waterfall_top = top + plot_height + 3;
            const int waterfall_height = available - plot_height - 3;
            const size_t visible_rows = waterfall_height < (int)SOLAR_OS_RADIO_SCAN_ROWS ?
                (size_t)waterfall_height : SOLAR_OS_RADIO_SCAN_ROWS;
            for (size_t row = 0; row < scan->rows && row < visible_rows; row++) {
                const int y0 = waterfall_top + row * waterfall_height / visible_rows;
                const int y1 = waterfall_top + (row + 1) * waterfall_height / visible_rows;
                for (size_t i = 0; i < scan->bins; i++) {
                    int level = scan->waterfall[row][i];
                    int shade = (-30 - level) * 16 / 100;
                    if (shade < 0) shade = 0;
                    if (shade > 16) shade = 16;
                    solar_os_gfx_set_color(gfx, solar_os_gfx_gray(shade));
                    int x0 = left + i * plot_width / scan->bins;
                    int x1 = left + (i + 1) * plot_width / scan->bins;
                    solar_os_gfx_fill_rect(gfx, x0, y0, x1 - x0, y1 - y0);
                }
            }
        } else if (!rfscan.band && scan->history_count) {
            uint32_t now = scan->history_ms[(scan->history_head + SOLAR_OS_RADIO_SCAN_HISTORY - 1U) % SOLAR_OS_RADIO_SCAN_HISTORY];
            int previous_x = -1, previous_y = 0;
            for (size_t i = 0; i < scan->history_count; i++) {
                size_t index = (scan->history_head + SOLAR_OS_RADIO_SCAN_HISTORY - scan->history_count + i) % SOLAR_OS_RADIO_SCAN_HISTORY;
                uint32_t age = now - scan->history_ms[index];
                if (age > rfscan.window_ms) continue;
                int x = left + (uint64_t)(rfscan.window_ms - age) * (plot_width - 1) / rfscan.window_ms;
                int y = rfscan_y(scan->history[index], top, plot_height);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                if (previous_x >= 0) solar_os_gfx_line(gfx, previous_x, previous_y, x, y);
                else solar_os_gfx_pixel(gfx, x, y);
                previous_x = x; previous_y = y;
            }
        }
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    if (rfscan.message[0]) strlcpy(text, rfscan.message, sizeof(text));
    else if (rfscan.band) snprintf(text, sizeof(text), "%lu sweeps  %lums/sweep  Tab mode",
        (unsigned long)scan->sweeps, (unsigned long)scan->last_sweep_ms);
    else snprintf(text, sizeof(text), "%s  Tab mode  Up/Down change", rfscan.radio.claimed ? rfscan.radio.owner : "Channel energy");
    solar_os_gfx_text(gfx, 4, height - 30, text);
    const char *buttons[] = {scan->running ? "Stop" : "Start", rfscan.band ? "Range" : "Time", "Step", rfscan.band ? "BW" : "Freq", "Radio"};
    for (unsigned i = 0; i < 5; i++) {
        rfscan_button(gfx, i * width / 5 + 2, height - 24, width / 5 - 4, buttons[i], rfscan.focus == i);
    }
    solar_os_gfx_present(gfx);
    rfscan.rendered_generation = scan->generation;
}

static unsigned rfscan_cycle(unsigned value, unsigned count, int direction)
{
    return direction < 0 ? (value + count - 1U) % count : (value + 1U) % count;
}

static void rfscan_action(unsigned action, int direction)
{
    if (action == 0) {
        if ((direction > 0 && rfscan.scan.running) ||
            (direction < 0 && !rfscan.scan.running)) return;
        if (rfscan.scan.running) (void)rfscan_stop();
        else rfscan_begin();
        return;
    }
    if (!rfscan.band && action == 2) {
        rfscan.step = rfscan_cycle(rfscan.step, 4, direction);
        rfscan_parameters();
        return;
    }
    if (!rfscan.band && !rfscan.channel_frequency) rfscan.center_hz = rfscan.scan.receiver.frequency_hz;
    if (!rfscan_stop()) return;
    if (action == 1) {
        if (rfscan.band) rfscan.range = rfscan_cycle(rfscan.range, 4, direction);
        else {
            static const uint32_t windows[] = {10000, 60000, 600000, 3600000};
            for (size_t i = 0; i < 4; i++) {
                if (rfscan.window_ms == windows[i]) {
                    rfscan.window_ms = windows[rfscan_cycle(i, 4, direction)];
                    break;
                }
            }
        }
    } else if (action == 3 && !rfscan.band) {
        const int64_t frequency = (int64_t)rfscan.center_hz +
            (direction < 0 ? -(int64_t)rfscan_steps[rfscan.step] : rfscan_steps[rfscan.step]);
        if (frequency <= 0 || frequency > UINT32_MAX) {
            strlcpy(rfscan.message, "Frequency out of range", sizeof(rfscan.message));
            return;
        }
        rfscan.center_hz = (uint32_t)frequency;
        rfscan.channel_frequency = true;
    } else if (action == 2) rfscan.step = rfscan_cycle(rfscan.step, 4, direction);
    else if (action == 3) rfscan.bandwidth = rfscan_cycle(rfscan.bandwidth, 4, direction);
    else if (action == 4) {
        const size_t count = solar_os_radio_count();
        size_t selected = 0;
        for (size_t i = 0; i < count; i++) {
            solar_os_radio_info_t info;
            if (solar_os_radio_get(i, &info) && strcmp(info.name, rfscan.radio.name) == 0) {
                selected = rfscan_cycle(i, count, direction); break;
            }
        }
        solar_os_radio_info_t info;
        if (count && solar_os_radio_get(selected, &info)) (void)rfscan_select(info.name);
        else strlcpy(rfscan.message, "No attached radio", sizeof(rfscan.message));
    }
    rfscan_parameters();
    solar_os_radio_scan_clear(&rfscan.scan);
}

static void rfscan_set_mode(bool band)
{
    if (rfscan.band == band || !rfscan_stop()) return;
    rfscan.band = band;
    rfscan_parameters();
    solar_os_radio_scan_clear(&rfscan.scan);
    if (!rfscan.band) rfscan_begin();
}

static esp_err_t rfscan_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) return ESP_ERR_NOT_SUPPORTED;
    memset(&rfscan, 0, sizeof(rfscan));
    solar_os_radio_scan_init(&rfscan.scan);
    rfscan.window_ms = 10000;
    rfscan.range = 2;
    rfscan.step = 2;
    rfscan.last_input = rfscan_now();
    if (solar_os_context_argc(ctx) > 1) {
        if (!rfscan_select(solar_os_context_argv(ctx, 1))) return ESP_ERR_NOT_FOUND;
    } else {
        solar_os_radio_info_t info;
        if (solar_os_radio_get(0, &info)) (void)rfscan_select(info.name);
    }
    solar_os_context_set_graphics_active(ctx, true);
    if (rfscan.radio.name[0]) rfscan_begin();
    rfscan_render(ctx);
    return ESP_OK;
}

static void rfscan_close(solar_os_context_t *ctx)
{
    (void)rfscan_stop();
    solar_os_context_set_graphics_active(ctx, false);
}

static void rfscan_suspend(solar_os_context_t *ctx)
{
    (void)rfscan_stop();
    rfscan.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void rfscan_resume(solar_os_context_t *ctx)
{
    if (rfscan.worker != NULL) (void)rfscan_stop();
    rfscan.suspended = false;
    solar_os_context_set_graphics_active(ctx, true);
    rfscan_render(ctx);
}

static bool rfscan_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL || rfscan.suspended) return false;
    uint32_t now = rfscan_now();
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (rfscan.band && rfscan.scan.running && (uint32_t)(now - rfscan.last_input) >= 180000U) {
            if (rfscan_stop()) strlcpy(rfscan.message, "Stopped after 3 minutes idle", sizeof(rfscan.message));
        }
        if (rfscan.worker != NULL) {
            esp_err_t err = solar_os_radio_scan_worker_copy(rfscan.worker, &rfscan.scan);
            if (err == ESP_OK && !rfscan.band && !rfscan.channel_frequency)
                rfscan.center_hz = rfscan.scan.receiver.frequency_hz;
            if (err == ESP_OK && !rfscan.scan.running) {
                esp_err_t sample_error = rfscan.scan.error;
                (void)rfscan_stop();
                rfscan_result(sample_error);
            }
        }
        if (rfscan.scan.generation == rfscan.rendered_generation) return false;
        if ((int32_t)(now - rfscan.next_render) < 0) return false;
        rfscan.next_render = now + 400U;
    } else if (event->type == SOLAR_OS_EVENT_CHAR) {
        uint8_t ch = (uint8_t)event->data.ch;
        rfscan.last_input = now;
        if (ch == SOLAR_OS_KEY_APP_EXIT || ch == 'q' || ch == SOLAR_OS_KEY_ESCAPE) {
            solar_os_context_finish(ctx, 0, NULL);
            return true;
        }
        if (ch == SOLAR_OS_KEY_LEFT) rfscan.focus = (rfscan.focus + 4) % 5;
        else if (ch == SOLAR_OS_KEY_RIGHT) rfscan.focus = (rfscan.focus + 1) % 5;
        else if (ch == '\t') rfscan_set_mode(!rfscan.band);
        else if (ch == SOLAR_OS_KEY_UP) rfscan_action(rfscan.focus, 1);
        else if (ch == SOLAR_OS_KEY_DOWN) rfscan_action(rfscan.focus, -1);
        else if (ch == '\n' || ch == '\r') rfscan_action(rfscan.focus, 0);
        else if (ch == ' ') rfscan_action(0, 0);
        else if (ch == 'r') rfscan_action(1, 1);
        else if (ch == 'f' && !rfscan.band) rfscan_action(3, 1);
        else if (ch == 's') rfscan_action(2, 1);
        else if (ch == 'b' && rfscan.band) rfscan_action(3, 1);
        else if (ch == 'n') rfscan_action(4, 1);
        else if (ch == 'c') {
            if (rfscan.worker != NULL) {
                (void)solar_os_radio_scan_worker_clear(rfscan.worker);
                (void)solar_os_radio_scan_worker_copy(rfscan.worker, &rfscan.scan);
            } else solar_os_radio_scan_clear(&rfscan.scan);
        }
        else return false;
    } else if (event->type == SOLAR_OS_EVENT_POINTER) {
        const solar_os_input_pointer_event_t *p = &event->data.pointer;
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        const int width = (int)solar_os_gfx_width(gfx), height = (int)solar_os_gfx_height(gfx);
        if (p->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) { rfscan.pointer_x = p->x; rfscan.pointer_y = p->y; }
        else { rfscan.pointer_x += p->delta_x; rfscan.pointer_y += p->delta_y; }
        if (rfscan.pointer_x < 0) rfscan.pointer_x = 0;
        if (rfscan.pointer_x >= width) rfscan.pointer_x = width - 1;
        if (rfscan.pointer_y < 0) rfscan.pointer_y = 0;
        if (rfscan.pointer_y >= height) rfscan.pointer_y = height - 1;
        if (p->action != SOLAR_OS_INPUT_POINTER_PRESS) return false;
        rfscan.last_input = now;
        if (rfscan.pointer_y >= height - 24) {
            rfscan.focus = rfscan.pointer_x * 5 / width;
            rfscan_action(rfscan.focus, 0);
        } else if (rfscan.pointer_y >= 18 && rfscan.pointer_y < 39) {
            rfscan_set_mode(rfscan.pointer_x >= width / 2);
        }
    } else return false;
    rfscan_render(ctx);
    return true;
}

static bool rfscan_release_ready(void)
{
    return rfscan_state == NULL || solar_os_radio_scan_worker_done(rfscan.worker);
}

static void rfscan_release_cleanup(void)
{
    if (rfscan_state != NULL) (void)solar_os_radio_scan_worker_close(&rfscan.worker, NULL);
}

const solar_os_app_t solar_os_rfscan_app = {
    .name = "rfscan",
    .summary = "radio energy graph and swept waterfall",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
    .start = rfscan_start,
    .stop = rfscan_close,
    .suspend = rfscan_suspend,
    .resume = rfscan_resume,
    .event = rfscan_event,
    .state_slot = &rfscan_state,
    .state_size = sizeof(rfscan_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = rfscan_release_ready,
    .state_release_cleanup = rfscan_release_cleanup,
    .worker_stack_bytes = 4096U,
    /* Full-frame presentation on a streamed color display takes up to ~100 ms
     * on T-Deck. Radio sampling runs independently in the scan worker. */
    .tick_deadline_ms = 125U,
};
