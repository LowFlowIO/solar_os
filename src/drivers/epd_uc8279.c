/* Panel sequence adapted from FreeInk SDK Uc8279X4Driver.cpp (MIT).
 * See doc/licenses/freeink-display.txt for attribution and license. */
#include "epd_uc8279.h"

#define SEND(cmd, ...) do { const uint8_t bytes_[] = {__VA_ARGS__}; \
    esp_err_t err_ = epd_ultrachip_command(d, cmd, bytes_, sizeof(bytes_)); \
    if (err_ != ESP_OK) return err_; } while (0)
#define TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static esp_err_t initialize_panel(epd_ultrachip_t *d, bool program_pll)
{
    SEND(0x00, 0x37, 0x4d);
    SEND(0x61, 0x03, 0x20, 0x02, 0x58);
    SEND(0x65, 0, 0, 0, 0);
    SEND(0x03, 0x20);
    if (program_pll) SEND(0x30, 0x0e);
    SEND(0xe1, 0x02);
    /* Drive rails and booster settings remain those programmed into the panel. */
    return ESP_OK;
}

static esp_err_t initialize_pro(epd_ultrachip_t *d) { return initialize_panel(d, true); }
static esp_err_t initialize_classic(epd_ultrachip_t *d) { return initialize_panel(d, false); }

static esp_err_t activate(epd_ultrachip_t *d, bool partial)
{
    SEND(0x50, partial ? 0xd7 : 0x97); /* UC8279 CDI has one byte. */
    SEND(0xe0, 0x02);
    SEND(0xe5, partial ? 0x5a : 0x1e);
    if (partial) {
        SEND(0x03, 0x20);
        SEND(0xe1, 0x02);
    }
    TRY(epd_ultrachip_power_on(d));
    if (partial) {
        TRY(epd_ultrachip_command(d, 0x91, NULL, 0));
        /* Full visible region in gate coordinates: X=0..799, Y=120..599.
         * PTIN without this PTL window does not develop a valid partial image. */
        SEND(0x90, 0, 0, 0x03, 0x1f, 0, 0x78, 0x02, 0x57, 0x01);
    }
    /* PON reloads MTP settings: select OTP/KW after power-on, before DRF. */
    SEND(0x00, 0x17, 0x4d);
    return epd_ultrachip_command(d, 0x12, NULL, 0);
}

static const epd_ultrachip_panel_t panel_pro = {
    .name = "UC8279 X4 Pro 800x480", .gate_count = 600, .gate_offset = 120,
    .initialize = initialize_pro, .activate = activate,
};
static const epd_ultrachip_panel_t panel_classic = {
    .name = "UC8279 X4 Classic 800x480", .gate_count = 600, .gate_offset = 120,
    .initialize = initialize_classic, .activate = activate,
};

esp_err_t epd_uc8279_init(epd_ultrachip_t *d, const epd_ultrachip_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    const epd_ultrachip_panel_t *panel;
    switch (config->panel) {
    case EPD_UC8279_PANEL_X4_PRO_800X480: panel = &panel_pro; break;
    case EPD_UC8279_PANEL_X4_CLASSIC_800X480: panel = &panel_classic; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    return epd_ultrachip_init(d, config, panel);
}
