#include <assert.h>
#include <stdio.h>
#include "../../src/drivers/epd_xteink_identify.c"

static int reset_count, locked, removed, dc_level, pull_mode;
static uint8_t current_command, version[5] = {0, 0, 1, 0xff, 0xff}, status = 0x13;
static bool unstable, fail_read;
void vTaskDelay(TickType_t ticks) { assert(ticks == 50 || ticks == 30); }
esp_err_t gpio_config(const gpio_config_t *c) { assert(c->mode == GPIO_MODE_OUTPUT); return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 14 && !level) reset_count++;
    if (pin == 18) dc_level = (int)level;
    return ESP_OK;
}
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode)
{ assert(pin == 11); pull_mode = mode; return ESP_OK; }
esp_err_t solar_os_bus_spi_add_device(const char *name, const spi_device_interface_config_t *c,
                                     spi_device_handle_t *spi)
{
    assert(!strcmp(name, "spi0") && c->spics_io_num == 13 && c->clock_speed_hz == 500000);
    assert(c->flags == (SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY));
    assert(pull_mode == GPIO_PULLUP_ONLY);
    *spi = (void *)1; return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t spi)
{ assert(spi && !locked); removed++; return ESP_OK; }
esp_err_t spi_device_acquire_bus(spi_device_handle_t spi, unsigned wait)
{ assert(spi && wait == portMAX_DELAY && !locked); locked = 1; return ESP_OK; }
void spi_device_release_bus(spi_device_handle_t spi) { assert(spi && locked); locked = 0; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t spi, spi_transaction_t *t)
{
    assert(spi && locked);
    if (t->length) {
        assert(!dc_level && t->length == 8 && t->flags & SPI_TRANS_CS_KEEP_ACTIVE);
        current_command = t->tx_data[0];
    } else {
        assert(dc_level && !t->flags);
        if (fail_read) return ESP_FAIL;
        if (current_command == 0x71) { assert(t->rxlength == 8); *(uint8_t *)t->rx_buffer = status; }
        else {
            assert(current_command == 0x70 && t->rxlength == 40);
            memcpy(t->rx_buffer, version, 5);
            if (unstable && reset_count == 2) ((uint8_t *)t->rx_buffer)[1] ^= 1;
        }
    }
    return ESP_OK;
}

static void probe(epd_xteink_controller_t expected, esp_err_t err)
{
    reset_count = removed = 0;
    epd_xteink_controller_t result;
    assert(epd_xteink_identify("spi0", 13, 18, 14, 11, &result) == err);
    assert(result == expected && removed == 1 && !locked && pull_mode == GPIO_FLOATING);
    if (err == ESP_OK) assert(reset_count == 2);
}
int main(void)
{
    probe(EPD_XTEINK_UC8179, ESP_OK);
    version[0] = 0xff; probe(EPD_XTEINK_UNKNOWN, ESP_ERR_NOT_SUPPORTED);
    version[0] = 0;
    const uint8_t uc8279[] = {2, 3, 0x67, 0x68, 0x69};
    for (size_t i = 0; i < sizeof(uc8279); ++i) {
        version[2] = uc8279[i]; probe(EPD_XTEINK_UC8279, ESP_OK);
    }
    version[2] = 4; probe(EPD_XTEINK_UNKNOWN, ESP_ERR_NOT_SUPPORTED);
    memset(version, 0xff, 5); status = 0xff; probe(EPD_XTEINK_SSD1677, ESP_OK);
    memset(version, 0, 5); status = 0; probe(EPD_XTEINK_UNKNOWN, ESP_ERR_NOT_SUPPORTED);
    version[2] = 1; status = 0x12; probe(EPD_XTEINK_UNKNOWN, ESP_ERR_NOT_SUPPORTED);
    status = 0x13; unstable = true; probe(EPD_XTEINK_UNKNOWN, ESP_ERR_NOT_SUPPORTED);
    unstable = false; fail_read = true; probe(EPD_XTEINK_UNKNOWN, ESP_FAIL);
    puts("Xteink controller identification tests passed");
}
