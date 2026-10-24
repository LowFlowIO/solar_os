#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../src/apps/solar_os_rfscan.c"

struct solar_os_radio_scan_worker { solar_os_radio_scan_t view; };
static struct solar_os_radio_scan_worker worker;
static unsigned starts, stops;
static char buttons[5][16];
static unsigned button_count;
static char frequency_line[96];
static bool busy;
static const uint32_t original_hz = 869618000U;

size_t strlcpy(char *out, const char *in, size_t size)
{
    size_t n = strlen(in);
    if (size) { size_t copy = n < size - 1 ? n : size - 1; memcpy(out, in, copy); out[copy] = 0; }
    return n;
}
int64_t esp_timer_get_time(void) { return 1000000; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
void solar_os_context_set_graphics_active(solar_os_context_t *ctx, bool active) { (void)ctx; (void)active; }
int solar_os_context_argc(const solar_os_context_t *ctx) { (void)ctx; return 1; }
const char *solar_os_context_argv(const solar_os_context_t *ctx, int index) { (void)ctx; (void)index; return NULL; }
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)code; (void)message; ctx->exit_requested = true; }

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { (void)gfx; return 320; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { (void)gfx; return 240; }
solar_os_gfx_color_t solar_os_gfx_gray(uint8_t level) { return level; }
void solar_os_gfx_set_color(solar_os_gfx_t *g, solar_os_gfx_color_t c) { (void)g; (void)c; }
void solar_os_gfx_set_font(solar_os_gfx_t *g, solar_os_gfx_font_t f) { (void)g; (void)f; }
void solar_os_gfx_set_line_style(solar_os_gfx_t *g, solar_os_gfx_line_style_t s) { (void)g; (void)s; }
size_t solar_os_gfx_text_width(solar_os_gfx_t *g, const char *s) { (void)g; return strlen(s) * 6; }
void solar_os_gfx_clear(solar_os_gfx_t *g, solar_os_gfx_color_t c) { (void)g; (void)c; button_count = 0; }
void solar_os_gfx_pixel(solar_os_gfx_t *g, int x, int y) { (void)g; (void)x; (void)y; }
void solar_os_gfx_line(solar_os_gfx_t *g, int a, int b, int c, int d) { (void)g; (void)a; (void)b; (void)c; (void)d; }
void solar_os_gfx_rect(solar_os_gfx_t *g, int a, int b, int c, int d) { (void)g; (void)a; (void)b; (void)c; (void)d; }
void solar_os_gfx_fill_rect(solar_os_gfx_t *g, int a, int b, int c, int d) { (void)g; (void)a; (void)b; (void)c; (void)d; }
void solar_os_gfx_present(solar_os_gfx_t *g) { (void)g; }
void solar_os_gfx_text(solar_os_gfx_t *g, int x, int y, const char *text)
{
    (void)g; (void)x;
    if (y == 52) strlcpy(frequency_line, text, sizeof(frequency_line));
    if (y == 230) { assert(button_count < 5); strlcpy(buttons[button_count++], text, sizeof(buttons[0])); }
}

size_t solar_os_radio_count(void) { return 1; }
esp_err_t solar_os_radio_get_info(const char *name, solar_os_radio_info_t *out)
{
    assert(!strcmp(name, "radio0"));
    memset(out, 0, sizeof(*out)); strlcpy(out->name, "radio0", sizeof(out->name));
    out->claimed = busy; if (busy) strlcpy(out->owner, "meshcore:test", sizeof(out->owner));
    return ESP_OK;
}
bool solar_os_radio_get(size_t index, solar_os_radio_info_t *out)
{ return index == 0 && solar_os_radio_get_info("radio0", out) == ESP_OK; }
esp_err_t solar_os_radio_get_status(const char *name, solar_os_radio_status_t *out)
{
    (void)name; memset(out, 0, sizeof(*out));
    out->config.frequency_hz = original_hz; out->config.rx_bandwidth_hz = 62500;
    out->state = SOLAR_OS_RADIO_STATE_RX; return ESP_OK;
}
void solar_os_radio_scan_clear(solar_os_radio_scan_t *scan)
{
    scan->history_count = scan->bins = scan->rows = 0;
    scan->latest = scan->floor = scan->peak = SOLAR_OS_RADIO_SCAN_EMPTY; scan->generation++;
}
void solar_os_radio_scan_init(solar_os_radio_scan_t *scan)
{ memset(scan, 0, sizeof(*scan)); solar_os_radio_scan_clear(scan); }
esp_err_t solar_os_radio_scan_worker_open(solar_os_radio_scan_worker_t **out,
    const char *name, const solar_os_radio_scan_config_t *config)
{
    assert(*out == NULL); starts++;
    if (busy && config->frequency_hz) return ESP_ERR_INVALID_STATE;
    solar_os_radio_scan_init(&worker.view); worker.view.config = *config;
    solar_os_radio_status_t status; solar_os_radio_get_status(name, &status);
    worker.view.receiver = status.config;
    if (config->frequency_hz) worker.view.receiver.frequency_hz = config->frequency_hz;
    worker.view.running = true; *out = &worker; return ESP_OK;
}
esp_err_t solar_os_radio_scan_worker_copy(solar_os_radio_scan_worker_t *w, solar_os_radio_scan_t *out)
{ *out = w->view; return ESP_OK; }
esp_err_t solar_os_radio_scan_worker_close(solar_os_radio_scan_worker_t **w, solar_os_radio_scan_t *out)
{
    if (*w) { stops++; if (out) { *out = (*w)->view; out->running = false; } *w = NULL; }
    return ESP_OK;
}
bool solar_os_radio_scan_worker_done(const solar_os_radio_scan_worker_t *w) { (void)w; return true; }
esp_err_t solar_os_radio_scan_worker_clear(solar_os_radio_scan_worker_t *w)
{ solar_os_radio_scan_clear(&w->view); return ESP_OK; }

static void key(solar_os_context_t *ctx, uint8_t ch)
{
    solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = (char)ch};
    assert(rfscan_event(ctx, &event));
    assert(button_count == 5);
}

int main(void)
{
    rfscan_state = calloc(1, sizeof(rfscan_state_t)); assert(rfscan_state);
    solar_os_context_t ctx = {.gfx = (solar_os_gfx_t *)&ctx};
    assert(rfscan_start(&ctx) == ESP_OK && rfscan.scan.running);
    assert(!strcmp(buttons[2], "Step") && !strcmp(buttons[3], "Freq"));
    assert(worker.view.config.frequency_hz == 0 && worker.view.config.bandwidth_hz == 0);
    key(&ctx, SOLAR_OS_KEY_RIGHT); key(&ctx, SOLAR_OS_KEY_RIGHT);
    assert(rfscan.focus == 2);
    key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.step == 3 && rfscan.scan.running && starts == 1 && stops == 0);
    key(&ctx, SOLAR_OS_KEY_RIGHT); key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.center_hz == original_hz + 250000U && !rfscan.scan.running);
    assert(strstr(frequency_line, "869.868000") && strstr(frequency_line, "Start to apply"));
    rfscan.focus = 0; key(&ctx, SOLAR_OS_KEY_UP);
    assert(worker.view.config.frequency_hz == original_hz + 250000U);
    assert(worker.view.config.bandwidth_hz == 0 && rfscan.scan.running);
    rfscan.focus = 3; key(&ctx, SOLAR_OS_KEY_DOWN);
    assert(rfscan.center_hz == original_hz);
    key(&ctx, SOLAR_OS_KEY_UP); busy = true;
    rfscan.focus = 0; key(&ctx, SOLAR_OS_KEY_UP);
    assert(!rfscan.scan.running && strstr(rfscan.message, "meshcore:test"));
    assert(rfscan.center_hz == original_hz + 250000U);
    busy = false; key(&ctx, '\t');
    assert(rfscan.band && !strcmp(buttons[3], "BW"));
    rfscan.focus = 3; key(&ctx, SOLAR_OS_KEY_UP);
    assert(rfscan.scan.config.bandwidth_hz == 62500);
    key(&ctx, '\t');
    assert(!rfscan.band && !strcmp(buttons[3], "Freq"));
    assert(worker.view.config.bandwidth_hz == 0);
    key(&ctx, SOLAR_OS_KEY_APP_EXIT); assert(ctx.exit_requested);
    rfscan_close(&ctx); free(rfscan_state);
    puts("rfscan controls: channel step/frequency, pending tuning, busy owner and band mapping: ok");
}
