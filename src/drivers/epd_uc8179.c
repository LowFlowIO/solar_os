/* Panel sequence adapted from FreeInk SDK Uc8179Driver.cpp (MIT).
 * See doc/licenses/freeink-display.txt for attribution and license. */
#include "epd_uc8179.h"

#define SEND(cmd, ...) do { const uint8_t bytes_[] = {__VA_ARGS__}; \
    esp_err_t err_ = epd_ultrachip_command(d, cmd, bytes_, sizeof(bytes_)); \
    if (err_ != ESP_OK) return err_; } while (0)
#define TRY(call) do { esp_err_t err_ = (call); if (err_ != ESP_OK) return err_; } while (0)

static esp_err_t initialize(epd_ultrachip_t *d)
{
    SEND(0x00, 0x3f, 0x0a);
    SEND(0x61, 0x03, 0x20, 0x02, 0x58);
    SEND(0x65, 0, 0, 0, 0);
    SEND(0x03, 0x20);
    SEND(0x06, 0x25, 0x25, 0x3c, 0x25);
    SEND(0xe1, 0x02);
    SEND(0xe3, 0x22);
    return ESP_OK;
}

static esp_err_t activate(epd_ultrachip_t *d, bool partial)
{
    SEND(0x50, 0x29, 0x07);
    SEND(0xe0, 0x02);
    SEND(0xe5, partial ? 0x5a : 0x1e);
    SEND(0x00, 0x1f, 0x0a); /* REG=0: use the panel's OTP waveform. */
    if (partial) {
        SEND(0x03, 0x20);
        SEND(0xe1, 0x02);
    }
    TRY(epd_ultrachip_power_on(d));
    if (partial) TRY(epd_ultrachip_command(d, 0x91, NULL, 0));
    return epd_ultrachip_command(d, 0x12, NULL, 0);
}

static esp_err_t finish(epd_ultrachip_t *d)
{
    SEND(0x50, 0xa9, 0x07);
    return ESP_OK;
}

static const epd_ultrachip_panel_t panel = {
    .name = "UC8179 Xteink 800x480", .gate_count = 600, .gate_offset = 0,
    .reverse_rows = true, .initialize = initialize, .activate = activate, .finish = finish,
};

esp_err_t epd_uc8179_init(epd_ultrachip_t *d, const epd_ultrachip_config_t *config)
{
    if (!config || config->panel != EPD_UC8179_PANEL_XTEINK_800X480)
        return ESP_ERR_INVALID_ARG;
    return epd_ultrachip_init(d, config, &panel);
}
