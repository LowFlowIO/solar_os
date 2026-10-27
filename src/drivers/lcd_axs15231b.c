#include "lcd_axs15231b.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pwm_port.h"
#include "solar_os_memory.h"
#include "solar_os_rgb565.h"
#include "solar_os_vector.h"

/* QSPI framing used by the JC3248W535 vendor driver (esp_lcd_axs15231b.c)
 * and ESPHome's mipi_spi quad bus: 8-bit opcode + 24-bit address. */
#define AXS_OPCODE_WRITE_CMD 0x02U
#define AXS_OPCODE_WRITE_COLOR 0x32U

#define AXS_CMD_SLPOUT 0x11U
#define AXS_CMD_INVOFF 0x20U
#define AXS_CMD_INVON 0x21U
#define AXS_CMD_DISPOFF 0x28U
#define AXS_CMD_DISPON 0x29U
#define AXS_CMD_CASET 0x2aU
#define AXS_CMD_RASET 0x2bU
#define AXS_CMD_RAMWR 0x2cU
#define AXS_CMD_MADCTL 0x36U
#define AXS_CMD_COLMOD 0x3aU

#define AXS_CMD_RAMWRC 0x3cU

#define AXS_RGB565_BLACK 0x0000U
#define AXS_RGB565_WHITE 0xffffU
#define AXS_RGB565_RED 0xf800U
#define AXS_RGB565_GREEN 0x07e0U
#define AXS_RGB565_BLUE 0x001fU
/* ESPHome spi_esp_idf MAX_TRANSFER_SIZE: largest single SPI transaction. */
#define AXS_MAX_TRANSFER_BYTES 4092U
/* Verbose logging for the first few flushes after init. */
#define AXS_LOG_FLUSHES 4U
/* ESPHome's AXS15231 model uses draw_rounding = 8. */
#define AXS_ROW_ROUNDING 8U

/* INDEX2/INDEX8 raster helpers. INDEX2 packs the first pixel into the low two
 * bits, matching solar_os_display_raster_t. */
static inline uint8_t axs_indexed_pixel(const uint8_t *row, uint16_t x,
                                        solar_os_display_format_t format)
{
    return format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? row[x] :
        (uint8_t)((row[x >> 2U] >> ((x & 3U) * 2U)) & 3U);
}

static inline void axs_indexed_set(uint8_t *row, uint16_t x, uint8_t index,
                                   solar_os_display_format_t format)
{
    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8) {
        row[x] = index;
    } else {
        const unsigned shift = (x & 3U) * 2U;
        row[x >> 2U] = (uint8_t)((row[x >> 2U] & ~(3U << shift)) |
                                 ((index & 3U) << shift));
    }
}

static void axs_indexed_rotate(const uint8_t *source, uint16_t width, uint16_t height,
                               size_t source_stride, uint8_t *output,
                               size_t output_stride, solar_os_display_format_t format,
                               solar_os_display_rotation_t rotation)
{
    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint16_t dx = x, dy = y;
            if (rotation == SOLAR_OS_DISPLAY_ROTATION_90) {
                dx = (uint16_t)(height - 1U - y);
                dy = x;
            } else if (rotation == SOLAR_OS_DISPLAY_ROTATION_180) {
                dx = (uint16_t)(width - 1U - x);
                dy = (uint16_t)(height - 1U - y);
            } else if (rotation == SOLAR_OS_DISPLAY_ROTATION_270) {
                dx = y;
                dy = (uint16_t)(width - 1U - x);
            }
            axs_indexed_set(output + (size_t)dy * output_stride, dx,
                            axs_indexed_pixel(source + (size_t)y * source_stride, x,
                                              format),
                            format);
        }
    }
}

static void axs_indexed_palette(const uint16_t *palette, solar_os_display_format_t format,
                                bool inverted, uint16_t *colors)
{
    const size_t count = format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 256U : 4U;
    for (size_t i = 0; i < count; i++) {
        /* INDEX2 is an ordinal shade ramp; INDEX8 holds arbitrary colors. */
        colors[i] = format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 ?
            palette[inverted ? 3U - i : i] :
            (inverted ? (uint16_t)~palette[i] : palette[i]);
    }
}

/* Palette colors are native uint16_t values; output uses LCD wire byte order. */
static void axs_indexed_scale_row(const uint8_t *source, uint16_t source_width,
                                  solar_os_display_format_t format,
                                  const uint16_t *colors, uint8_t *output,
                                  uint16_t output_width)
{
    uint16_t source_x = 0U;
    uint32_t remainder = 0U;
    for (uint16_t x = 0; x < output_width; x++) {
        const uint16_t color = colors[axs_indexed_pixel(source, source_x, format)];
        output[x * 2U] = (uint8_t)(color >> 8U);
        output[x * 2U + 1U] = (uint8_t)color;
        remainder += source_width;
        while (remainder >= output_width) {
            remainder -= output_width;
            source_x++;
        }
    }
}

static const char *TAG = "lcd_axs15231b";
static lcd_axs15231b_t *active_display;

static const u8x8_display_info_t axs_display_info_template = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 150,
    .post_reset_wait_ms = 120,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = 40000000,
    .spi_mode = 3,
    .i2c_bus_clock_100kHz = 4,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = 0,
    .tile_height = 0,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = 0,
    .pixel_height = 0,
};

static bool gpio_valid(int pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

static uint16_t axs_rgb888_to_rgb565(uint32_t rgb888)
{
    return (uint16_t)(((rgb888 >> 8) & 0xf800U) | ((rgb888 >> 5) & 0x07e0U) |
                      ((rgb888 >> 3) & 0x001fU));
}

/* ---------------------------------------------------------------- transport */

static void axs_cs(const lcd_axs15231b_t *display, bool active)
{
    if (display->config.software_cs) {
        gpio_set_level((gpio_num_t)display->config.cs_pin, active ? 0 : 1);
    }
}

/* One SPI transaction shaped like ESPHome's SPIDelegateHw::write_cmd_addr_data:
 * variable command/address/dummy phases on one line, data on one or four
 * lines, synchronous polling. */
static esp_err_t axs_write(lcd_axs15231b_t *display, uint8_t cmd_bits, uint32_t cmd,
                           uint8_t addr_bits, uint32_t addr, const uint8_t *data,
                           size_t length, bool quad, bool keep_cs)
{
    spi_transaction_ext_t transaction;
    memset(&transaction, 0, sizeof(transaction));
    transaction.base.flags =
        SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    if (quad) {
        transaction.base.flags |= SPI_TRANS_MODE_QIO;
    }
    if (keep_cs && !display->config.software_cs) {
        transaction.base.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
    }
    transaction.command_bits = cmd_bits;
    transaction.address_bits = addr_bits;
    transaction.dummy_bits = 0;
    transaction.base.cmd = (uint16_t)cmd;
    transaction.base.addr = addr;
    transaction.base.length = length * 8U;
    transaction.base.rxlength = 0;
    transaction.base.tx_buffer = length > 0U ? data : NULL;
    esp_err_t err = spi_device_polling_start(display->spi, &transaction.base, portMAX_DELAY);
    if (err == ESP_OK) {
        err = spi_device_polling_end(display->spi, portMAX_DELAY);
    }
    return err;
}

/* Register write: opcode 0x02, address = cmd << 8, parameters on one line. */
static esp_err_t axs_tx_param(lcd_axs15231b_t *display, uint8_t command,
                              const uint8_t *data, size_t length)
{
    if (length > display->dma_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (length > 0U) {
        memcpy(display->dma[0], data, length);
    }
    axs_cs(display, true);
    const esp_err_t err = axs_write(display, 8U, AXS_OPCODE_WRITE_CMD, 24U,
                                    (uint32_t)command << 8U, display->dma[0], length,
                                    false, false);
    axs_cs(display, false);
    return err;
}

static bool axs_checked_param(lcd_axs15231b_t *display, uint8_t command,
                              const uint8_t *data, size_t length)
{
    const esp_err_t err = axs_tx_param(display, command, data, length);
    if (err != ESP_OK) {
        display->last_error = err;
        ESP_LOGE(TAG, "command 0x%02x failed: %s", command, esp_err_to_name(err));
        return false;
    }
    return true;
}

static void axs_mark_dirty(lcd_axs15231b_t *display, uint16_t y0, uint16_t y1)
{
    if (y1 >= display->config.height) {
        y1 = (uint16_t)(display->config.height - 1U);
    }
    if (y0 > y1) {
        return;
    }
    if (!display->dirty) {
        display->dirty_y0 = y0;
        display->dirty_y1 = y1;
        display->dirty = true;
        return;
    }
    if (y0 < display->dirty_y0) display->dirty_y0 = y0;
    if (y1 > display->dirty_y1) display->dirty_y1 = y1;
}

/* Stream native rows [y0, y1] at full panel width, like ESPHome mipi_spi's
 * quad path: RASET + CASET as register writes, then one chip-select assertion
 * carrying opcode 0x32 + RAMWR (0x2C << 8) and all pixel bytes on four lines.
 * Pixel data is RGB565 big-endian (high byte first), as ESPHome sends it and
 * as the vendor demo produces with LV_COLOR_16_SWAP=1. */
static esp_err_t axs_push_rows(lcd_axs15231b_t *display, uint16_t y0, uint16_t y1)
{
    const uint16_t width = display->config.width;
    const size_t row_bytes = (size_t)width * 2U;
    const size_t rows_per_chunk = display->dma_size / row_bytes;
    if (rows_per_chunk == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t caset[] = {0x00, 0x00, (uint8_t)((width - 1U) >> 8),
                             (uint8_t)((width - 1U) & 0xffU)};
    const uint8_t raset[] = {(uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xffU),
                             (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xffU)};
    const bool log_this = display->flush_count < AXS_LOG_FLUSHES;

    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(display->spi, portMAX_DELAY),
                        TAG, "acquire bus failed");
    esp_err_t err = axs_tx_param(display, AXS_CMD_RASET, raset, sizeof(raset));
    if (err == ESP_OK) {
        err = axs_tx_param(display, AXS_CMD_CASET, caset, sizeof(caset));
    }

    size_t chunks = 0U;
    size_t sent = 0U;
    uint16_t row = y0;
    bool first = true;
    if (err == ESP_OK) {
        axs_cs(display, true);
    }
    while (err == ESP_OK && row <= y1) {
        size_t rows = (size_t)(y1 - row) + 1U;
        if (rows > rows_per_chunk) rows = rows_per_chunk;
        const size_t bytes = rows * row_bytes;
        memcpy(display->dma[0], display->frame + (size_t)row * row_bytes, bytes);
        const bool last = (size_t)row + rows > y1;
        if (first) {
            err = axs_write(display, 8U, AXS_OPCODE_WRITE_COLOR, 24U,
                            (uint32_t)AXS_CMD_RAMWR << 8U, display->dma[0], bytes,
                            true, !last);
        } else {
            err = axs_write(display, 0U, 0U, 0U, 0U, display->dma[0], bytes, true, !last);
        }
        first = false;
        chunks++;
        sent += bytes;
        row = (uint16_t)(row + rows);
    }
    axs_cs(display, false);
    spi_device_release_bus(display->spi);
    if (log_this || err != ESP_OK) {
        const uint8_t *px = display->frame + (size_t)y0 * row_bytes;
        ESP_LOGI(TAG, "flush #%lu rows %u..%u: %u bytes in %u chunks, first px %02x%02x %02x%02x: %s",
                 (unsigned long)display->flush_count, (unsigned)y0, (unsigned)y1,
                 (unsigned)sent, (unsigned)chunks, px[0], px[1], px[2], px[3],
                 esp_err_to_name(err));
    }
    display->flush_count++;
    return err;
}

static esp_err_t axs_flush(lcd_axs15231b_t *display)
{
    if (!display->dirty) {
        return ESP_OK;
    }
    uint16_t y0 = display->dirty_y0;
    uint16_t y1 = display->dirty_y1;
    if (display->config.full_frame_only) {
        y0 = 0U;
        y1 = (uint16_t)(display->config.height - 1U);
    } else {
        y0 = (uint16_t)(y0 - (y0 % AXS_ROW_ROUNDING));
        y1 = (uint16_t)(y1 + (AXS_ROW_ROUNDING - 1U - (y1 % AXS_ROW_ROUNDING)));
        if (y1 >= display->config.height) {
            y1 = (uint16_t)(display->config.height - 1U);
        }
    }
    const esp_err_t err = axs_push_rows(display, y0, y1);
    if (err == ESP_OK) {
        display->dirty = false;
    } else {
        display->last_error = err;
        ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---------------------------------------------------------------- backlight */

static esp_err_t axs_apply_backlight(lcd_axs15231b_t *display, uint8_t percent)
{
    if (!lcd_axs15231b_backlight_supported(display)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (percent > 100U) percent = 100U;
    if (display->config.backlight_pwm) {
        const uint8_t duty = display->config.backlight_active_high ?
            percent : (uint8_t)(100U - percent);
        return pwm_port_set((gpio_num_t)display->config.backlight_pin,
                            display->config.backlight_pwm_hz, duty);
    }
    const int active = display->config.backlight_active_high ? 1 : 0;
    return gpio_set_level((gpio_num_t)display->config.backlight_pin,
                          percent > 0U ? active : !active);
}

static void axs_set_backlight_power(lcd_axs15231b_t *display, bool on)
{
    display->backlight_power = on;
    const esp_err_t err =
        axs_apply_backlight(display, on ? display->backlight_percent : 0U);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        display->last_error = err;
        ESP_LOGW(TAG, "backlight set failed: %s", esp_err_to_name(err));
    }
}

/* ---------------------------------------------------------------- panel init */

static esp_err_t axs_configure_control_pins(lcd_axs15231b_t *display)
{
    uint64_t mask = 0ULL;
    if (gpio_valid(display->config.reset_pin)) {
        mask |= 1ULL << (unsigned)display->config.reset_pin;
    }
    if (gpio_valid(display->config.backlight_pin)) {
        mask |= 1ULL << (unsigned)display->config.backlight_pin;
    }
    if (display->config.software_cs && gpio_valid(display->config.cs_pin)) {
        mask |= 1ULL << (unsigned)display->config.cs_pin;
    }
    if (mask == 0ULL) {
        return ESP_OK;
    }
    const gpio_config_t io_config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_config);
}

/* One-shot boot reset on the JC3248W535 GPIO38 line: LOW, hold, then HIGH.
 * Matches the user's working ESPHome on_boot lambda (150 ms low). */
static void axs_hardware_reset(lcd_axs15231b_t *display)
{
    if (display->reset_done || !gpio_valid(display->config.reset_pin)) {
        return;
    }
    const gpio_num_t pin = (gpio_num_t)display->config.reset_pin;
    ESP_LOGI(TAG, "reset pulse on GPIO%d: low %lu ms", (int)pin,
             (unsigned long)display->config.reset_low_ms);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(display->config.reset_low_ms));
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    display->reset_done = true;
}

typedef struct {
    uint8_t cmd;
    uint8_t length;
    uint16_t delay_ms;
    const uint8_t *data;
} axs_init_cmd_t;

#define AXS_INIT(cmd_, delay_, ...) \
    {cmd_, sizeof((const uint8_t[]){__VA_ARGS__}), delay_, (const uint8_t[]){__VA_ARGS__}}

/* JC3248W535EN demo, DEMO_LVGL/esp_bsp.c lcd_init_cmds (trailing SLPOUT and
 * RAMWR entries are sent by axs_full_init itself). */
static const axs_init_cmd_t axs_vendor_init[] = {
    AXS_INIT(0xBB, 0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5),
    AXS_INIT(0xA0, 0, 0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00),
    AXS_INIT(0xA2, 0, 0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A),
    AXS_INIT(0xD0, 0, 0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12),
    AXS_INIT(0xA3, 0, 0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55),
    AXS_INIT(0xC1, 0, 0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40),
    AXS_INIT(0xC3, 0, 0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01),
    AXS_INIT(0xC4, 0, 0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50),
    AXS_INIT(0xC5, 0, 0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00),
    AXS_INIT(0xC6, 0, 0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22),
    AXS_INIT(0xC7, 0, 0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f),
    AXS_INIT(0xC9, 0, 0x33, 0x44, 0x44, 0x01),
    AXS_INIT(0xCF, 0, 0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08),
    AXS_INIT(0xD5, 0, 0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00),
    AXS_INIT(0xD6, 0, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00),
    AXS_INIT(0xD7, 0, 0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F),
    AXS_INIT(0xD8, 0, 0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19),
    AXS_INIT(0xD9, 0, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F),
    AXS_INIT(0xDD, 0, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F),
    AXS_INIT(0xDF, 0, 0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90),
    AXS_INIT(0xE0, 0, 0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D),
    AXS_INIT(0xE1, 0, 0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F),
    AXS_INIT(0xE2, 0, 0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D),
    AXS_INIT(0xE3, 0, 0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F),
    AXS_INIT(0xE4, 0, 0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D),
    AXS_INIT(0xE5, 0, 0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F),
    AXS_INIT(0xA4, 0, 0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30),
    AXS_INIT(0xA4, 0, 0x85, 0x85, 0x95, 0x85),
    AXS_INIT(0xBB, 0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
};

/* ESPHome mipi_spi AXS15231 model (esphome/components/mipi_spi/models/jc.py). */
static const axs_init_cmd_t axs_esphome_init[] = {
    AXS_INIT(0xBB, 0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5),
    AXS_INIT(0xC1, 0, 0x33),
    AXS_INIT(0xBB, 0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
};

static esp_err_t axs_fill_and_flush(lcd_axs15231b_t *display, uint16_t rgb565)
{
    solar_os_vector_fill_rgb565_be(display->frame, rgb565,
                                   (size_t)display->config.width * display->config.height);
    display->shadow_valid = false;
    display->indexed_surface_valid = false;
    display->dirty = false;
    axs_mark_dirty(display, 0U, (uint16_t)(display->config.height - 1U));
    return axs_flush(display);
}

static esp_err_t axs_full_init(lcd_axs15231b_t *display)
{
    const lcd_axs15231b_config_t *c = &display->config;
    display->panel_ready = false;
    display->flush_count = 0U;
    ESP_LOGI(TAG,
             "init: clk=%d d0-3=%d/%d/%d/%d cs=%d (%s) reset=%d bl=%d spi_mode=%u "
             "clock=%lu Hz madctl=0x%02x invert=%d init=%s full_frame=%d",
             c->sclk_pin, c->data0_pin, c->data1_pin, c->data2_pin, c->data3_pin,
             c->cs_pin, c->software_cs ? "gpio" : "spi", c->reset_pin, c->backlight_pin,
             (unsigned)c->spi_mode, (unsigned long)c->spi_clock_hz, c->madctl,
             c->invert_color ? 1 : 0, c->vendor_init ? "vendor" : "esphome",
             c->full_frame_only ? 1 : 0);

    /* Backlight stays off until the panel holds a known (black) frame. */
    axs_set_backlight_power(display, false);
    axs_hardware_reset(display);

    const axs_init_cmd_t *table = c->vendor_init ? axs_vendor_init : axs_esphome_init;
    const size_t count = c->vendor_init ?
        sizeof(axs_vendor_init) / sizeof(axs_vendor_init[0]) :
        sizeof(axs_esphome_init) / sizeof(axs_esphome_init[0]);
    if (c->vendor_init) {
        /* Vendor esp_lcd_axs15231b panel_init: SLPOUT first, then the table. */
        if (!axs_checked_param(display, AXS_CMD_SLPOUT, NULL, 0)) {
            return display->last_error;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    for (size_t i = 0; i < count; i++) {
        if (!axs_checked_param(display, table[i].cmd, table[i].data, table[i].length)) {
            return display->last_error;
        }
        if (table[i].delay_ms > 0U) {
            vTaskDelay(pdMS_TO_TICKS(table[i].delay_ms));
        }
    }
    const uint8_t colmod[] = {0x55};
    const uint8_t madctl[] = {c->madctl};
    if (!axs_checked_param(display, AXS_CMD_COLMOD, colmod, sizeof(colmod)) ||
        !axs_checked_param(display, AXS_CMD_MADCTL, madctl, sizeof(madctl)) ||
        !axs_checked_param(display, c->invert_color ? AXS_CMD_INVON : AXS_CMD_INVOFF,
                           NULL, 0) ||
        !axs_checked_param(display, AXS_CMD_SLPOUT, NULL, 0)) {
        return display->last_error;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "init: %u register writes OK, sleep out", (unsigned)(count + 4U));

    /* Never show uninitialised GRAM: write a full black frame first. */
    esp_err_t err = axs_fill_and_flush(display, AXS_RGB565_BLACK);
    ESP_LOGI(TAG, "init: clear-to-black full frame: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        display->last_error = err;
        return err;
    }
    if (!axs_checked_param(display, AXS_CMD_DISPON, NULL, 0)) {
        return display->last_error;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    display->panel_ready = true;
    axs_set_backlight_power(display, true);
    ESP_LOGI(TAG, "init: display on, backlight %u%%", (unsigned)display->backlight_percent);

    if (c->self_test) {
        static const struct { uint16_t color; const char *name; } steps[] = {
            {AXS_RGB565_RED, "red"},
            {AXS_RGB565_GREEN, "green"},
            {AXS_RGB565_BLUE, "blue"},
            {AXS_RGB565_WHITE, "white"},
        };
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            err = axs_fill_and_flush(display, steps[i].color);
            ESP_LOGI(TAG, "self-test: %s (0x%04x): %s", steps[i].name, steps[i].color,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        err = axs_fill_and_flush(display, AXS_RGB565_BLACK);
        if (err != ESP_OK) {
            display->last_error = err;
            return err;
        }
    }
    display->last_error = ESP_OK;
    return ESP_OK;
}

/* ---------------------------------------------------------------- u8g2 tiles */

static void axs_render_tile_run(lcd_axs15231b_t *display, const uint8_t *tile_data,
                                uint8_t x_pos, uint8_t y_pos, uint8_t count)
{
    const uint16_t x = (uint16_t)x_pos * 8U;
    const uint16_t y = (uint16_t)y_pos * 8U;
    uint16_t width = (uint16_t)count * 8U;
    uint16_t height = 8U;
    if (x + width > display->config.width) width = (uint16_t)(display->config.width - x);
    if (y + height > display->config.height) height = (uint16_t)(display->config.height - y);
    if (width == 0U || height == 0U) {
        return;
    }
    const size_t row_bytes = (size_t)display->config.width * 2U;
    for (uint16_t row = 0; row < height; row++) {
        solar_os_vector_expand_1bpp_to_rgb565_be(
            display->frame + (size_t)(y + row) * row_bytes + (size_t)x * 2U,
            tile_data, row, display->foreground_rgb565,
            display->background_rgb565, width);
    }
    axs_mark_dirty(display, y, (uint16_t)(y + height - 1U));
    display->indexed_surface_valid = false;
}

static void axs_draw_tile(lcd_axs15231b_t *display, const u8x8_tile_t *tile)
{
    if (tile == NULL || tile->tile_ptr == NULL || tile->cnt == 0U ||
        tile->x_pos >= display->tile_width || tile->y_pos >= display->tile_height) {
        return;
    }
    uint8_t count = tile->cnt;
    if (tile->x_pos + count > display->tile_width) {
        count = (uint8_t)(display->tile_width - tile->x_pos);
    }
    const size_t shadow_row = (size_t)tile->y_pos * display->buffer_row_bytes;
    uint8_t offset = 0U;
    while (offset < count) {
        const size_t pos = shadow_row + ((size_t)tile->x_pos + offset) * 8U;
        const uint8_t *data = tile->tile_ptr + (size_t)offset * 8U;
        if (display->shadow_valid && memcmp(&display->shadow[pos], data, 8U) == 0) {
            offset++;
            continue;
        }
        const uint8_t first = offset;
        while (offset < count) {
            const size_t p = shadow_row + ((size_t)tile->x_pos + offset) * 8U;
            const uint8_t *d = tile->tile_ptr + (size_t)offset * 8U;
            if (display->shadow_valid && memcmp(&display->shadow[p], d, 8U) == 0) {
                break;
            }
            memcpy(&display->shadow[p], d, 8U);
            offset++;
        }
        axs_render_tile_run(display, tile->tile_ptr + (size_t)first * 8U,
                            (uint8_t)(tile->x_pos + first), tile->y_pos,
                            (uint8_t)(offset - first));
    }
    /* After every tile row has been seen once, the shadow is authoritative. */
    if (!display->shadow_valid && tile->x_pos == 0U && count == display->tile_width &&
        tile->y_pos + 1U >= display->tile_height) {
        display->shadow_valid = true;
    }
}

static uint8_t axs_u8x8_byte_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int,
                                void *arg_ptr)
{
    (void)u8x8;
    (void)message;
    (void)arg_int;
    (void)arg_ptr;
    return 1;
}

static uint8_t axs_u8x8_display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg_int,
                                   void *arg_ptr)
{
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        if (active_display == NULL) {
            return 0;
        }
        u8x8_d_helper_display_setup_memory(u8x8, &active_display->display_info);
        return 1;
    }
    lcd_axs15231b_t *display = active_display;
    if (display == NULL) {
        return 0;
    }
    switch (message) {
        case U8X8_MSG_DISPLAY_INIT:
            return axs_full_init(display) == ESP_OK ? 1 : 0;
        case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
            return lcd_axs15231b_set_power_save(display, arg_int != 0) == ESP_OK ? 1 : 0;
        case U8X8_MSG_DISPLAY_DRAW_TILE:
            axs_draw_tile(display, (const u8x8_tile_t *)arg_ptr);
            return 1;
        case U8X8_MSG_DISPLAY_REFRESH:
            return axs_flush(display) == ESP_OK ? 1 : 0;
        default:
            return 0;
    }
}

/* ---------------------------------------------------------------- surfaces */

static void axs_surface_native_to_logical(const solar_os_display_surface_t *surface,
                                          uint16_t native_x, uint16_t native_y,
                                          uint16_t *x, uint16_t *y)
{
    switch (surface->rotation) {
        case SOLAR_OS_DISPLAY_ROTATION_90:
            *x = native_y;
            *y = (uint16_t)(surface->native_width - 1U - native_x);
            break;
        case SOLAR_OS_DISPLAY_ROTATION_180:
            *x = (uint16_t)(surface->native_width - 1U - native_x);
            *y = (uint16_t)(surface->native_height - 1U - native_y);
            break;
        case SOLAR_OS_DISPLAY_ROTATION_270:
            *x = (uint16_t)(surface->native_height - 1U - native_y);
            *y = native_x;
            break;
        case SOLAR_OS_DISPLAY_ROTATION_0:
        default:
            *x = native_x;
            *y = native_y;
            break;
    }
}

static bool axs_surface_tile_dirty(const solar_os_display_surface_t *surface,
                                   uint16_t tile_x, uint16_t tile_y)
{
    const size_t offset = (size_t)tile_y * surface->dirty_stride + (size_t)(tile_x >> 3U);
    return offset < surface->dirty_size &&
           (surface->dirty_tiles[offset] & (uint8_t)(1U << (tile_x & 7U))) != 0;
}

static uint32_t axs_surface_tile_hash(const solar_os_display_surface_t *surface,
                                      uint16_t tile_x, uint16_t tile_y)
{
    const uint16_t x_start = (uint16_t)(tile_x * 8U);
    const uint16_t y_start = (uint16_t)(tile_y * 8U);
    uint16_t x_end = (uint16_t)(x_start + 8U);
    uint16_t y_end = (uint16_t)(y_start + 8U);
    if (x_end > surface->native_width) x_end = surface->native_width;
    if (y_end > surface->native_height) y_end = surface->native_height;
    uint32_t hash = UINT32_C(2166136261);
    for (uint16_t y = y_start; y < y_end; y++) {
        for (uint16_t x = x_start; x < x_end; x++) {
            uint16_t lx = 0, ly = 0;
            axs_surface_native_to_logical(surface, x, y, &lx, &ly);
            const uint16_t color =
                surface->palette_rgb565[surface->data[(size_t)ly * surface->stride + lx]];
            hash = (hash ^ (uint8_t)(color >> 8U)) * UINT32_C(16777619);
            hash = (hash ^ (uint8_t)color) * UINT32_C(16777619);
        }
    }
    return hash != 0U ? hash : 1U;
}

static void axs_render_surface_tile(lcd_axs15231b_t *display,
                                    const solar_os_display_surface_t *surface,
                                    uint16_t tile_x, uint16_t tile_y)
{
    const uint16_t x_start = (uint16_t)(tile_x * 8U);
    const uint16_t y_start = (uint16_t)(tile_y * 8U);
    uint16_t x_end = (uint16_t)(x_start + 8U);
    uint16_t y_end = (uint16_t)(y_start + 8U);
    if (x_end > surface->native_width) x_end = surface->native_width;
    if (y_end > surface->native_height) y_end = surface->native_height;
    const size_t row_bytes = (size_t)display->config.width * 2U;
    for (uint16_t y = y_start; y < y_end; y++) {
        uint8_t *out = display->frame + (size_t)y * row_bytes;
        for (uint16_t x = x_start; x < x_end; x++) {
            uint16_t lx = 0, ly = 0;
            axs_surface_native_to_logical(surface, x, y, &lx, &ly);
            const uint16_t color =
                surface->palette_rgb565[surface->data[(size_t)ly * surface->stride + lx]];
            out[(size_t)x * 2U] = (uint8_t)(color >> 8U);
            out[(size_t)x * 2U + 1U] = (uint8_t)color;
        }
    }
    axs_mark_dirty(display, y_start, (uint16_t)(y_end - 1U));
}

esp_err_t lcd_axs15231b_present_surface(lcd_axs15231b_t *display,
                                        const solar_os_display_surface_t *surface)
{
    if (display == NULL || display->spi == NULL || surface == NULL ||
        surface->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8 ||
        surface->data == NULL || surface->palette_rgb565 == NULL ||
        surface->palette_size < 256U || surface->dirty_tiles == NULL ||
        surface->presented_hashes == NULL || surface->tile_size != 8U ||
        surface->native_width != display->config.width ||
        surface->native_height != display->config.height ||
        surface->stride < surface->width) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool rotated = surface->rotation == SOLAR_OS_DISPLAY_ROTATION_90 ||
                         surface->rotation == SOLAR_OS_DISPLAY_ROTATION_270;
    if (surface->width != (rotated ? surface->native_height : surface->native_width) ||
        surface->height != (rotated ? surface->native_width : surface->native_height)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint16_t tile_columns = (uint16_t)((surface->native_width + 7U) / 8U);
    const uint16_t tile_rows = (uint16_t)((surface->native_height + 7U) / 8U);
    if (surface->dirty_stride < (tile_columns + 7U) / 8U ||
        surface->dirty_size < (size_t)surface->dirty_stride * tile_rows ||
        surface->hash_stride < tile_columns ||
        surface->presented_hash_count < (size_t)surface->hash_stride * tile_rows) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!display->indexed_surface_valid ||
        display->indexed_surface_data != surface->data) {
        /* Something else drew over the framebuffer: repaint every dirty tile. */
        memset(surface->presented_hashes, 0,
               surface->presented_hash_count * sizeof(surface->presented_hashes[0]));
        display->indexed_surface_data = surface->data;
        display->indexed_surface_valid = true;
    }
    for (uint16_t tile_y = 0; tile_y < tile_rows; tile_y++) {
        for (uint16_t tile_x = 0; tile_x < tile_columns; tile_x++) {
            if (!axs_surface_tile_dirty(surface, tile_x, tile_y)) {
                continue;
            }
            const size_t index = (size_t)tile_y * surface->hash_stride + tile_x;
            const uint32_t hash = axs_surface_tile_hash(surface, tile_x, tile_y);
            if (surface->presented_hashes[index] == hash) {
                continue;
            }
            axs_render_surface_tile(display, surface, tile_x, tile_y);
            surface->presented_hashes[index] = hash;
        }
    }
    /* u8g2 tiles no longer describe the framebuffer contents. */
    display->shadow_valid = false;
    return axs_flush(display);
}

/* ---------------------------------------------------------------- raster frames */

static void axs_logical_to_native(const lcd_axs15231b_t *display, uint16_t lx,
                                  uint16_t ly, uint16_t *nx, uint16_t *ny)
{
    const u8g2_cb_t *rotation = display->u8g2.cb;
    if (rotation == U8G2_R1) {
        *nx = (uint16_t)(display->config.width - 1U - ly);
        *ny = lx;
    } else if (rotation == U8G2_R2) {
        *nx = (uint16_t)(display->config.width - 1U - lx);
        *ny = (uint16_t)(display->config.height - 1U - ly);
    } else if (rotation == U8G2_R3) {
        *nx = ly;
        *ny = (uint16_t)(display->config.height - 1U - lx);
    } else {
        *nx = lx;
        *ny = ly;
    }
}

static esp_err_t axs_prepare_native_frame(lcd_axs15231b_t *display,
                                          const solar_os_display_raster_t *frame,
                                          solar_os_display_raster_t *native_frame)
{
    *native_frame = *frame;
    if (display->u8g2.cb == U8G2_R0) {
        return ESP_OK;
    }
    const bool quarter = display->u8g2.cb == U8G2_R1 || display->u8g2.cb == U8G2_R3;
    const bool rgb565 = frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565;
    if (rgb565 && quarter && frame->source_height > UINT16_MAX / 2U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint16_t width = quarter ? frame->source_height : frame->source_width;
    const uint16_t height = quarter ? frame->source_width : frame->source_height;
    const uint16_t stride = rgb565 ? (uint16_t)(width * 2U) :
        (frame->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? width :
         (uint16_t)((width + 3U) / 4U));
    const size_t size = (size_t)stride * height;
    if (display->frame_scratch_size < size) {
        uint8_t *scratch = solar_os_memory_alloc(size,
            rgb565 ? SOLAR_OS_MEMORY_EXTERNAL_REQUIRED : SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "display.rotate");
        if (scratch == NULL) {
            return ESP_ERR_NO_MEM;
        }
        solar_os_memory_free(display->frame_scratch);
        display->frame_scratch = scratch;
        display->frame_scratch_size = size;
    }
    const solar_os_display_rotation_t rotation =
        display->u8g2.cb == U8G2_R1 ? SOLAR_OS_DISPLAY_ROTATION_90 :
        display->u8g2.cb == U8G2_R2 ? SOLAR_OS_DISPLAY_ROTATION_180 :
                                      SOLAR_OS_DISPLAY_ROTATION_270;
    if (rgb565) {
        solar_os_rgb565_rotate(frame->data, frame->source_width, frame->source_height,
                               frame->source_stride, display->frame_scratch, stride,
                               rotation);
    } else {
        memset(display->frame_scratch, 0, size);
        axs_indexed_rotate(frame->data, frame->source_width, frame->source_height,
                           frame->source_stride, display->frame_scratch, stride,
                           frame->format, rotation);
    }
    native_frame->data = display->frame_scratch;
    native_frame->data_size = size;
    native_frame->source_width = width;
    native_frame->source_height = height;
    native_frame->source_stride = stride;
    return ESP_OK;
}

static void axs_frame_line(const solar_os_display_raster_t *frame,
                           uint16_t output_width, uint16_t output_height,
                           uint16_t output_y, const uint16_t *colors, uint8_t *out)
{
    const uint16_t source_y =
        (uint16_t)((uint32_t)output_y * frame->source_height / output_height);
    const uint8_t *source = frame->data + (size_t)source_y * frame->source_stride;
    if (frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565) {
        solar_os_rgb565_scale_row(source, frame->source_width, out, output_width);
        return;
    }
    axs_indexed_scale_row(source, frame->source_width, frame->format, colors, out,
                          output_width);
}

esp_err_t lcd_axs15231b_present_frame(lcd_axs15231b_t *display,
                                      const solar_os_display_raster_t *frame)
{
    if (display == NULL || display->spi == NULL || frame == NULL ||
        (frame->format != SOLAR_OS_DISPLAY_FORMAT_INDEX2 &&
         frame->format != SOLAR_OS_DISPLAY_FORMAT_INDEX8 &&
         frame->format != SOLAR_OS_DISPLAY_FORMAT_RGB565) || frame->data == NULL ||
        (frame->format != SOLAR_OS_DISPLAY_FORMAT_RGB565 &&
         (frame->palette_rgb565 == NULL || frame->palette_size <
          (frame->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 256U : 4U))) ||
        frame->source_width == 0 || frame->source_height == 0 ||
        frame->width == 0 || frame->height == 0 ||
        frame->source_stride < (frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 ?
            (size_t)frame->source_width * 2U :
            (frame->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? frame->source_width :
             (frame->source_width + 3U) / 4U)) ||
        frame->source_height > SIZE_MAX / frame->source_stride ||
        frame->data_size < (size_t)frame->source_height * frame->source_stride ||
        (frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 && frame->palette_inverted) ||
        (frame->clear_background && frame->format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 &&
         frame->background_index >= 4U)) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool quarter = display->u8g2.cb == U8G2_R1 || display->u8g2.cb == U8G2_R3;
    const uint16_t logical_width = quarter ? display->config.height : display->config.width;
    const uint16_t logical_height = quarter ? display->config.width : display->config.height;
    if ((uint32_t)frame->x + frame->width > logical_width ||
        (uint32_t)frame->y + frame->height > logical_height) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t cx[4], cy[4];
    const uint16_t lx1 = (uint16_t)(frame->x + frame->width - 1U);
    const uint16_t ly1 = (uint16_t)(frame->y + frame->height - 1U);
    axs_logical_to_native(display, frame->x, frame->y, &cx[0], &cy[0]);
    axs_logical_to_native(display, lx1, frame->y, &cx[1], &cy[1]);
    axs_logical_to_native(display, frame->x, ly1, &cx[2], &cy[2]);
    axs_logical_to_native(display, lx1, ly1, &cx[3], &cy[3]);
    uint16_t nx0 = cx[0], ny0 = cy[0], nx1 = cx[0], ny1 = cy[0];
    for (size_t i = 1; i < 4U; i++) {
        if (cx[i] < nx0) nx0 = cx[i];
        if (cy[i] < ny0) ny0 = cy[i];
        if (cx[i] > nx1) nx1 = cx[i];
        if (cy[i] > ny1) ny1 = cy[i];
    }
    const uint16_t native_width = (uint16_t)(nx1 - nx0 + 1U);
    const uint16_t native_height = (uint16_t)(ny1 - ny0 + 1U);

    solar_os_display_raster_t native_frame;
    ESP_RETURN_ON_ERROR(axs_prepare_native_frame(display, frame, &native_frame), TAG,
                        "frame rotation failed");
    uint16_t colors[256];
    if (frame->format != SOLAR_OS_DISPLAY_FORMAT_RGB565) {
        axs_indexed_palette(frame->palette_rgb565, frame->format,
                            frame->palette_inverted, colors);
    }
    const size_t row_bytes = (size_t)display->config.width * 2U;
    if (frame->clear_background) {
        const uint16_t background = frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 ?
            frame->background_rgb565 : colors[frame->background_index];
        solar_os_vector_fill_rgb565_be(display->frame, background,
                                       (size_t)display->config.width * display->config.height);
        axs_mark_dirty(display, 0U, (uint16_t)(display->config.height - 1U));
    }
    for (uint16_t row = 0; row < native_height; row++) {
        axs_frame_line(&native_frame, native_width, native_height, row, colors,
                       display->frame + (size_t)(ny0 + row) * row_bytes + (size_t)nx0 * 2U);
    }
    axs_mark_dirty(display, ny0, ny1);
    display->shadow_valid = false;
    display->indexed_surface_valid = false;
    return axs_flush(display);
}

/* ---------------------------------------------------------------- lifecycle */

esp_err_t lcd_axs15231b_init(lcd_axs15231b_t *display,
                             const lcd_axs15231b_config_t *config)
{
    if (display == NULL || config == NULL || !gpio_valid(config->sclk_pin) ||
        !gpio_valid(config->data0_pin) || !gpio_valid(config->data1_pin) ||
        !gpio_valid(config->data2_pin) || !gpio_valid(config->data3_pin) ||
        !gpio_valid(config->cs_pin) || config->width == 0 || config->height == 0 ||
        config->width > 480 || config->height > 480) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(display, 0, sizeof(*display));
    display->config = *config;
    if (display->config.spi_clock_hz == 0U) display->config.spi_clock_hz = 40000000U;
    if (display->config.backlight_pwm_hz == 0U) display->config.backlight_pwm_hz = 20000U;
    if (display->config.reset_low_ms == 0U) display->config.reset_low_ms = 150U;
    if (display->config.rotation == NULL) display->config.rotation = U8G2_R0;
    display->tile_width = (uint16_t)((display->config.width + 7U) / 8U);
    display->tile_height = (uint16_t)((display->config.height + 7U) / 8U);
    display->buffer_row_bytes = (size_t)display->tile_width * 8U;
    display->display_info = axs_display_info_template;
    display->display_info.sck_clock_hz = display->config.spi_clock_hz;
    display->display_info.tile_width = (uint8_t)display->tile_width;
    display->display_info.tile_height = (uint8_t)display->tile_height;
    display->display_info.pixel_width = display->config.width;
    display->display_info.pixel_height = display->config.height;
    display->backlight_percent = 100U;
    display->foreground_rgb565 = AXS_RGB565_BLACK;
    display->background_rgb565 = AXS_RGB565_WHITE;
    if (display->config.spi_mode > 3U) display->config.spi_mode = 0U;

    ESP_RETURN_ON_ERROR(axs_configure_control_pins(display), TAG, "control pins failed");
    if (gpio_valid(display->config.reset_pin)) {
        gpio_set_level((gpio_num_t)display->config.reset_pin, 1);
    }
    axs_cs(display, false);
    axs_set_backlight_power(display, false);

    /* Whole rows per transaction, each transaction <= 4092 bytes. */
    const size_t init_row_bytes = (size_t)display->config.width * 2U;
    display->dma_size = (AXS_MAX_TRANSFER_BYTES / init_row_bytes) * init_row_bytes;
    if (display->dma_size == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    const spi_bus_config_t bus_config = {
        .data0_io_num = display->config.data0_pin,
        .data1_io_num = display->config.data1_pin,
        .sclk_io_num = display->config.sclk_pin,
        .data2_io_num = display->config.data2_pin,
        .data3_io_num = display->config.data3_pin,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = (int)AXS_MAX_TRANSFER_BYTES,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK |
                 SPICOMMON_BUSFLAG_QUAD,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)display->config.spi_host,
                                       &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "qspi bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    display->bus_initialized = true;

    const spi_device_interface_config_t device_config = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = display->config.spi_mode,
        .clock_speed_hz = (int)display->config.spi_clock_hz,
        .spics_io_num = display->config.software_cs ? -1 : display->config.cs_pin,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    err = spi_bus_add_device((spi_host_device_t)display->config.spi_host,
                             &device_config, &display->spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "qspi device add failed: %s", esp_err_to_name(err));
        lcd_axs15231b_deinit(display);
        return err;
    }
    int actual_khz = 0;
    if (spi_device_get_actual_freq(display->spi, &actual_khz) == ESP_OK) {
        ESP_LOGI(TAG, "qspi host %d ready: %d kHz, mode %u, %u-byte chunks",
                 display->config.spi_host, actual_khz, (unsigned)display->config.spi_mode,
                 (unsigned)display->dma_size);
    }

    display->dma[0] = heap_caps_malloc(display->dma_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    display->dma[1] = heap_caps_malloc(display->dma_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    display->frame_size = (size_t)display->config.width * display->config.height * 2U;
    display->frame = heap_caps_malloc(display->frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    display->buffer_size = display->buffer_row_bytes * display->tile_height;
    display->buffer = heap_caps_malloc(display->buffer_size, MALLOC_CAP_8BIT);
    display->shadow = heap_caps_malloc(display->buffer_size, MALLOC_CAP_8BIT);
    if (display->dma[0] == NULL || display->dma[1] == NULL || display->frame == NULL ||
        display->buffer == NULL || display->shadow == NULL) {
        lcd_axs15231b_deinit(display);
        return ESP_ERR_NO_MEM;
    }
    memset(display->buffer, 0, display->buffer_size);
    memset(display->shadow, 0, display->buffer_size);
    /* PSRAM is not zeroed at boot; never stream it uninitialised. */
    memset(display->frame, 0, display->frame_size);

    active_display = display;
    u8g2_SetupDisplay(&display->u8g2, axs_u8x8_display_cb, u8x8_dummy_cb,
                      axs_u8x8_byte_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&display->u8g2, display->buffer, (uint8_t)display->tile_height,
                     u8g2_ll_hvline_vertical_top_lsb, display->config.rotation);
    display->last_error = ESP_OK;
    u8g2_InitDisplay(&display->u8g2);
    if (display->last_error != ESP_OK || !display->panel_ready) {
        /* Do not turn the backlight on over a panel that never initialised. */
        const esp_err_t err = display->last_error != ESP_OK ?
            display->last_error : ESP_FAIL;
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        lcd_axs15231b_deinit(display);
        return err;
    }
    u8g2_SetPowerSave(&display->u8g2, 0);
    ESP_LOGI(TAG, "panel ready (%ux%u native)", (unsigned)display->config.width,
             (unsigned)display->config.height);
    return display->last_error;
}

esp_err_t lcd_axs15231b_set_power_save(lcd_axs15231b_t *display, bool enabled)
{
    if (display == NULL || display->spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        if (!display->panel_ready) {
            return ESP_ERR_INVALID_STATE;
        }
        const esp_err_t err = axs_tx_param(display, AXS_CMD_DISPON, NULL, 0);
        if (err == ESP_OK) {
            axs_set_backlight_power(display, true);
        }
        return err;
    }
    axs_set_backlight_power(display, false);
    return axs_tx_param(display, AXS_CMD_DISPOFF, NULL, 0);
}

esp_err_t lcd_axs15231b_shutdown(lcd_axs15231b_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    u8g2_SetPowerSave(&display->u8g2, 1);
    return ESP_OK;
}

esp_err_t lcd_axs15231b_resume(lcd_axs15231b_t *display)
{
    if (display == NULL || display->spi == NULL || display->buffer == NULL ||
        display->frame == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(axs_configure_control_pins(display), TAG, "resume pins failed");
    if (gpio_valid(display->config.reset_pin)) {
        gpio_set_level((gpio_num_t)display->config.reset_pin, 1);
    }
    active_display = display;
    axs_cs(display, false);
    display->last_error = ESP_OK;
    display->shadow_valid = false;
    display->indexed_surface_valid = false;
    u8g2_InitDisplay(&display->u8g2);
    if (display->last_error != ESP_OK || !display->panel_ready) {
        return display->last_error != ESP_OK ? display->last_error : ESP_FAIL;
    }
    u8g2_SetPowerSave(&display->u8g2, 0);
    return display->last_error;
}

esp_err_t lcd_axs15231b_set_colors(lcd_axs15231b_t *display, uint32_t foreground_rgb888,
                                   uint32_t background_rgb888)
{
    if (display == NULL || foreground_rgb888 > 0xffffffU || background_rgb888 > 0xffffffU) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t fg = axs_rgb888_to_rgb565(foreground_rgb888);
    const uint16_t bg = axs_rgb888_to_rgb565(background_rgb888);
    if (fg != display->foreground_rgb565 || bg != display->background_rgb565) {
        display->foreground_rgb565 = fg;
        display->background_rgb565 = bg;
        display->shadow_valid = false;
    }
    return ESP_OK;
}

void lcd_axs15231b_deinit(lcd_axs15231b_t *display)
{
    if (display == NULL) {
        return;
    }
    if (lcd_axs15231b_backlight_supported(display)) {
        axs_set_backlight_power(display, false);
    }
    display->panel_ready = false;
    if (display->spi != NULL) {
        spi_bus_remove_device(display->spi);
        display->spi = NULL;
    }
    if (display->bus_initialized) {
        spi_bus_free((spi_host_device_t)display->config.spi_host);
        display->bus_initialized = false;
    }
    heap_caps_free(display->dma[0]);
    heap_caps_free(display->dma[1]);
    heap_caps_free(display->frame);
    heap_caps_free(display->buffer);
    heap_caps_free(display->shadow);
    display->dma[0] = display->dma[1] = NULL;
    display->frame = display->buffer = display->shadow = NULL;
    if (display->frame_scratch != NULL) {
        solar_os_memory_free(display->frame_scratch);
        display->frame_scratch = NULL;
        display->frame_scratch_size = 0U;
    }
    if (active_display == display) {
        active_display = NULL;
    }
}

u8g2_t *lcd_axs15231b_get_u8g2(lcd_axs15231b_t *display)
{
    return display == NULL ? NULL : &display->u8g2;
}

bool lcd_axs15231b_backlight_supported(const lcd_axs15231b_t *display)
{
    return display != NULL && gpio_valid(display->config.backlight_pin);
}

esp_err_t lcd_axs15231b_get_backlight(const lcd_axs15231b_t *display, uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (display == NULL) {
        *percent = 0;
        return ESP_ERR_INVALID_STATE;
    }
    if (!lcd_axs15231b_backlight_supported(display)) {
        *percent = 100;
        return ESP_ERR_NOT_SUPPORTED;
    }
    *percent = display->backlight_percent;
    return ESP_OK;
}

esp_err_t lcd_axs15231b_set_backlight(lcd_axs15231b_t *display, uint8_t percent)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lcd_axs15231b_backlight_supported(display)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    display->backlight_percent = percent;
    return display->backlight_power ? axs_apply_backlight(display, percent) : ESP_OK;
}
