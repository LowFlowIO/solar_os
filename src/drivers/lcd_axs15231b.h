#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AXS15231B panel on a private quad-SPI (QSPI) host.
 *
 * The controller has no D/C line in QSPI mode. Every transfer starts with an
 * 8-bit opcode and a 24-bit address on one line: opcode 0x02 writes a register
 * (address = cmd << 8, parameters on one line), opcode 0x32 streams pixels
 * (address = RAMWR 0x2C << 8, data on four lines). The driver owns the SPI host
 * and its pins; they are not registered as a shared SolarOS bus. */
typedef struct {
    int spi_host;
    int sclk_pin;
    int data0_pin;
    int data1_pin;
    int data2_pin;
    int data3_pin;
    int cs_pin;
    int reset_pin;
    int backlight_pin;
    uint32_t spi_clock_hz;
    uint32_t backlight_pwm_hz;
    uint32_t reset_low_ms;
    uint16_t width;
    uint16_t height;
    uint8_t madctl;
    bool invert_color;
    bool backlight_active_high;
    bool backlight_pwm;
    /* Push full-height frames only (RAMWR from row 0). Partial row bands set
     * RASET/CASET like ESPHome's mipi_spi AXS15231 model. */
    bool full_frame_only;
    /* SPI mode (0-3). ESPHome mipi_spi uses mode 0 for quad with a CS pin;
     * the vendor esp_lcd_axs15231b demo uses mode 3. */
    uint8_t spi_mode;
    /* Drive CS as a plain GPIO around each command / pixel stream (ESPHome
     * spi_esp_idf behaviour) instead of the SPI peripheral's CS line. */
    bool software_cs;
    /* Send the long vendor register table from the JC3248W535 demo
     * (esp_bsp.c lcd_init_cmds) before COLMOD/MADCTL/SLPOUT/DISPON. */
    bool vendor_init;
    /* Show solid red, green, blue and white for ~0.4 s each after init. */
    bool self_test;
    const u8g2_cb_t *rotation;
} lcd_axs15231b_config_t;

typedef struct {
    spi_device_handle_t spi;
    bool bus_initialized;
    u8g2_t u8g2;
    uint8_t *buffer;        /* u8g2 tile buffer */
    uint8_t *shadow;        /* last tile data written into the framebuffer */
    uint8_t *frame;         /* native RGB565, wire byte order */
    uint8_t *dma[2];
    uint8_t *frame_scratch;
    size_t buffer_size;
    size_t frame_size;
    size_t dma_size;
    size_t frame_scratch_size;
    uint16_t dirty_y0;
    uint16_t dirty_y1;
    bool dirty;
    bool reset_done;
    bool panel_ready;       /* init sequence + first clear completed */
    uint32_t flush_count;
    bool indexed_surface_valid;
    const uint8_t *indexed_surface_data;
    bool shadow_valid;
    uint16_t foreground_rgb565;
    uint16_t background_rgb565;
    esp_err_t last_error;
    uint8_t backlight_percent;
    bool backlight_power;
    lcd_axs15231b_config_t config;
    u8x8_display_info_t display_info;
    uint16_t tile_width;
    uint16_t tile_height;
    size_t buffer_row_bytes;
} lcd_axs15231b_t;

esp_err_t lcd_axs15231b_init(lcd_axs15231b_t *display,
                             const lcd_axs15231b_config_t *config);
esp_err_t lcd_axs15231b_resume(lcd_axs15231b_t *display);
esp_err_t lcd_axs15231b_shutdown(lcd_axs15231b_t *display);
void lcd_axs15231b_deinit(lcd_axs15231b_t *display);
u8g2_t *lcd_axs15231b_get_u8g2(lcd_axs15231b_t *display);
bool lcd_axs15231b_backlight_supported(const lcd_axs15231b_t *display);
esp_err_t lcd_axs15231b_get_backlight(const lcd_axs15231b_t *display,
                                      uint8_t *percent);
esp_err_t lcd_axs15231b_set_backlight(lcd_axs15231b_t *display, uint8_t percent);
esp_err_t lcd_axs15231b_set_power_save(lcd_axs15231b_t *display, bool enabled);
esp_err_t lcd_axs15231b_set_colors(lcd_axs15231b_t *display,
                                   uint32_t foreground_rgb888,
                                   uint32_t background_rgb888);
esp_err_t lcd_axs15231b_present_surface(
    lcd_axs15231b_t *display,
    const solar_os_display_surface_t *surface);
esp_err_t lcd_axs15231b_present_frame(
    lcd_axs15231b_t *display,
    const solar_os_display_raster_t *frame);

#ifdef __cplusplus
}
#endif
