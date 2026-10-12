#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "solar_os_log.h"
#include "esp_log.h"
#undef ESP_LOGI
#define ESP_LOGI(tag, ...) do { (void)(tag); snprintf(last_log, sizeof(last_log), __VA_ARGS__); } while (0)
static char last_log[256];
static char error_log[256];
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...) {
    assert(strcmp(tag, "epd_ssd1683") == 0);
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, args);
    va_end(args);
    if (level == SOLAR_OS_LOG_LEVEL_ERROR)
        snprintf(error_log, sizeof(error_log), "%s", last_log);
    return ESP_OK;
}
#include "../../src/drivers/epd_ssd1683.c"

typedef struct { uint8_t cmd; size_t size; uint8_t data[14000]; } capture_t;
static capture_t captures[256];
static size_t capture_count;
static int dc_level, resets;
static int64_t time_us, busy_until;
static bool no_busy, stuck_busy;
static uint8_t fail_command;
static uint8_t frame[SSD1683_DUAL_BUFFER_BYTES], shadow[SSD1683_DUAL_BUFFER_BYTES];
static uint8_t line[SSD1683_DUAL_HEIGHT];

void vTaskDelay(TickType_t ticks) { assert(ticks); time_us += ticks * 1000LL; }
int64_t esp_timer_get_time(void) { return time_us; }
int gpio_get_level(gpio_num_t pin) {
    (void)pin;
    return stuck_busy || time_us < busy_until;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value) {
    if (pin == 46) dc_level = (int)value;
    if (pin == 47 && value == 0) { ++resets; busy_until = 0; }
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, int mode) { (void)pin; (void)mode; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode) { (void)pin; (void)mode; return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *t) {
    assert(device);
    const uint8_t *data = t->flags & SPI_TRANS_USE_TXDATA ? t->tx_data : t->tx_buffer;
    const size_t size = t->length / 8U;
    if (!dc_level) {
        assert(size == 1 && capture_count < 256);
        if (*data == fail_command) { fail_command = 0; return ESP_FAIL; }
        captures[capture_count++] = (capture_t){.cmd = *data};
        if (*data == 0x20 && !no_busy) busy_until = time_us + 20000;
    } else {
        assert(capture_count);
        capture_t *c = &captures[capture_count - 1];
        assert(c->size + size <= sizeof(c->data));
        memcpy(c->data + c->size, data, size);
        c->size += size;
    }
    return ESP_OK;
}

static epd_ssd1683_t display(void) {
    memset(frame, 0xff, sizeof(frame));
    memset(shadow, 0xff, sizeof(shadow));
    capture_count = 0; resets = 0; busy_until = 0;
    no_busy = stuck_busy = false; fail_command = 0;
    return (epd_ssd1683_t){.spi = (void *)1, .buffer = frame, .shadow = shadow,
        .line_buffer = line, .buffer_size = sizeof(frame), .shadow_size = sizeof(shadow),
        .line_buffer_size = sizeof(line), .dc_pin = 46, .reset_pin = 47,
        .busy_pin = 48, .busy_level = 1, .power_pin = 7, .powered = true,
        .panel_variant = EPD_SSD1683_PANEL_ELECROW_579_DUAL,
        .controller_ready = true, .shadow_valid = true,
        .pulse_tuning = ssd1683_default_pulse_tuning(EPD_SSD1683_PANEL_ELECROW_579_DUAL)};
}
static capture_t *find(uint8_t cmd, unsigned occurrence) {
    for (size_t i = 0; i < capture_count; ++i)
        if (captures[i].cmd == cmd && occurrence-- == 0) return &captures[i];
    assert(!"missing command"); return NULL;
}
static unsigned count(uint8_t cmd) {
    unsigned n = 0;
    for (size_t i = 0; i < capture_count; ++i) n += captures[i].cmd == cmd;
    return n;
}
static void pixel(unsigned x, unsigned y, bool white) {
    const size_t p = (y / 8U) * SSD1683_DUAL_BUFFER_ROW_BYTES + x;
    const uint8_t bit = 1U << (y % 8U);
    frame[p] = white ? frame[p] | bit : frame[p] & (uint8_t)~bit;
}
static bool plane_pixel(uint8_t cmd, unsigned x, unsigned y) {
    /* Slave transport columns start at padded x=400; 8-pixel seam at 396. */
    unsigned padded_x = x < 396 ? x : x + 8;
    if (cmd & 0x80) padded_x -= 400;
    capture_t *c = find(cmd, 0);
    assert(c->size == 50U * 272U);
    return (c->data[(padded_x / 8U) * 272U + y] & (0x80U >> (padded_x % 8U))) != 0;
}
static void free_budget(epd_ssd1683_t *d) {
    heap_caps_free(d->pixel_budget.pixels);
    memset(&d->pixel_budget, 0, sizeof(d->pixel_budget));
}

static void test_native(void) {
    epd_ssd1683_t d = display();
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0);
    pixel(395, 0, false); pixel(396, 271, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(find(0x22, 0)->data[0] == 0xdc);
    assert(!plane_pixel(0x24, 395, 0) && !plane_pixel(0xa4, 396, 271));
    assert(count(0x32) == 0 && count(0xb2) == 0);
}
static void test_pulse_and_restore(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=3") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK); /* Native full baseline, even unchanged. */
    assert(find(0x22, 0)->data[0] == 0xf7 && d.pixel_budget.pending == 0);
    capture_count = 0;
    pixel(0, 0, false); pixel(395, 100, false); pixel(396, 271, false); pixel(791, 10, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(count(0x32) == 1 && count(0xb2) == 1);
    /* Published panel power setup must precede its first custom activation,
     * for both controllers, rather than relying on previous OTP state.
     */
    assert(find(0x0c, 0)->size == 4 && find(0x8c, 0)->size == 4);
    assert(memcmp(find(0x0c, 0)->data, (uint8_t[]){0x8b,0x9c,0xa4,0x0f}, 4) == 0);
    assert(memcmp(find(0x8c, 0)->data, find(0x0c, 0)->data, 4) == 0);
    assert(find(0x03, 0)->data[0] == 0x17 && find(0x83, 0)->data[0] == 0x17);
    assert(find(0x04, 0)->size == 3 && find(0x84, 0)->size == 3);
    assert(memcmp(find(0x04, 0)->data, (uint8_t[]){0x41,0xa8,0x32}, 3) == 0);
    assert(memcmp(find(0x84, 0)->data, find(0x04, 0)->data, 3) == 0);
    capture_t *lut = find(0x32, 0), *slave_lut = find(0xb2, 0);
    assert(lut->size == 227 && memcmp(lut->data, slave_lut->data, 227) == 0);
    assert(lut->data[85] == 0x48 && lut->data[127] == 0x88);
    assert(lut->data[43] == 8 && lut->data[169] == 8 && lut->data[224] == 4);
    assert(count(0x24) == 3 && count(0x26) == 3 && count(0xa4) == 3 && count(0xa6) == 3);
    assert(find(0x21, 0)->size == 2 && find(0x21, 0)->data[1] == 0x10);
    assert(find(0x37, 0)->size == 10 && find(0xb7, 0)->size == 10);
    assert(find(0x37, 0)->data[5] == 0 && find(0xb7, 0)->data[5] == 0);
    assert(find(0x2c, 0)->data[0] == 0 && find(0xac, 0)->data[0] == 0);
    assert(plane_pixel(0x26, 0, 0) && plane_pixel(0x26, 395, 100));
    assert(plane_pixel(0xa6, 396, 271) && plane_pixel(0xa6, 791, 10));
    assert(!plane_pixel(0x24, 0, 0) && !plane_pixel(0xa4, 396, 271));
    assert(!plane_pixel(0x26, 1, 0)); /* Unchanged white is HOLD, not driven. */
    for (unsigned y = 0; y < 272; ++y) {
        assert((find(0x26, 0)->data[49U * 272U + y] & 0x0fU) == 0);
        assert((find(0xa6, 0)->data[y] & 0xf0U) == 0);
    }
    assert(d.pixel_budget.pending == 0 && d.pulse_updates == 1);
    assert(strstr(last_log, "pixels=4 cycles=3") != NULL);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK && capture_count == 0);
    pixel(395, 100, true); pixel(396, 271, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(plane_pixel(0x24, 395, 100) && plane_pixel(0xa4, 396, 271));
    assert(!plane_pixel(0x26, 395, 100) && !plane_pixel(0xa6, 396, 271));
    capture_count = 0;
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=auto") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(resets > 0 && !d.pulse_lut_loaded && d.pixel_budget.pixels == NULL);
    assert(count(0x32) == 0 && find(0x22, count(0x22) - 1)->data[0] == 0xf7);
}
static void test_default_drive_budget(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    capture_count = 0;
    pixel(0, 0, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(count(0x20) == 6 && d.pixel_budget.pending == 0);
    capture_count = 0;
    pixel(0, 0, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(count(0x20) == 6 && d.pixel_budget.pending == 0);
    free_budget(&d);
}
static void test_cleanup_failure_and_capability(void) {
    epd_ssd1683_t d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "cleanup-every=1") == ESP_OK);
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    pixel(0, 0, false);
    assert(ssd1683_refresh(&d) == ESP_OK);
    capture_count = 0;
    pixel(0, 0, true);
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(d.pulse_updates == 0 && !d.pulse_lut_loaded);
    assert(find(0x22, count(0x22) - 1)->data[0] == 0xf7);
    capture_count = 0;
    pixel(0, 0, false); fail_command = 0xa6;
    assert(ssd1683_refresh(&d) == ESP_FAIL);
    assert(d.pixel_budget.pending == 1 && !d.shadow_valid && !d.controller_ready);
    assert(d.refresh_mode == EPD_SSD1683_REFRESH_AUTO);
    capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_OK);
    assert(d.pixel_budget.pixels == NULL && d.shadow_valid);

    d = display();
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_OK);
    assert(ssd1683_refresh(&d) == ESP_OK);
    pixel(1, 0, false); no_busy = true; capture_count = 0;
    assert(ssd1683_refresh(&d) == ESP_ERR_TIMEOUT);
    assert(strstr(error_log, "did not assert BUSY") != NULL);
    assert(d.pixel_budget.pending == 1 && d.refresh_mode == EPD_SSD1683_REFRESH_AUTO);
    free_budget(&d);
    d = display();
    d.panel_variant = EPD_SSD1683_PANEL_WAVESHARE_V2;
    assert(epd_ssd1683_set_controller_mode(&d, "refresh=pulse") == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1683_set_controller_mode(&d, "drive-cycles=3") == ESP_ERR_INVALID_ARG);
    assert(strcmp(epd_ssd1683_controller_mode_values(&d), "refresh=<auto,fast,full>") == 0);
}
int main(void) {
    test_native(); test_pulse_and_restore(); test_default_drive_budget();
    test_cleanup_failure_and_capability();
    puts("SSD1683 dual native/pulse transport tests: ok");
}
