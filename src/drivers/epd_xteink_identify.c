#include "epd_xteink_identify.h"

#include <string.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

epd_xteink_controller_t epd_xteink_classify(const uint8_t version[5], uint8_t status)
{
    if (!version) return EPD_XTEINK_UNKNOWN;
    bool uniform = true;
    for (size_t i = 1; i < 5; ++i) uniform &= version[i] == version[0];
    /* This fallback is restricted to the X4-family 800x480 controller set.
     * A grounded bus is ambiguous, not evidence for an SSD1677. */
    if (uniform && version[0] == 0xff && status == 0xff)
        return EPD_XTEINK_SSD1677;
    if (uniform || version[0] != 0 || status == 0 || status == 0xff || !(status & 1))
        return EPD_XTEINK_UNKNOWN;
    switch (version[2]) {
    case 0x01: return EPD_XTEINK_UC8179;
    case 0x02: case 0x03: case 0x67: case 0x68: case 0x69:
        return EPD_XTEINK_UC8279;
    default: return EPD_XTEINK_UNKNOWN;
    }
}

static esp_err_t read_register(spi_device_handle_t spi, int dc, uint8_t reg,
                                uint8_t *data, size_t length)
{
    esp_err_t err = spi_device_acquire_bus(spi, portMAX_DELAY);
    if (err != ESP_OK) return err;
    err = gpio_set_level(dc, 0);
    spi_transaction_t tx = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE,
        .length = 8, .tx_data = {reg},
    };
    if (err == ESP_OK) err = spi_device_polling_transmit(spi, &tx);
    if (err == ESP_OK) err = gpio_set_level(dc, 1);
    spi_transaction_t rx = {.rxlength = length * 8, .rx_buffer = data};
    if (err == ESP_OK) err = spi_device_polling_transmit(spi, &rx);
    spi_device_release_bus(spi);
    return err;
}

esp_err_t epd_xteink_identify(const char *spi_bus, int cs, int dc, int reset,
                             int mosi, epd_xteink_controller_t *controller)
{
    if (!spi_bus || !controller || !GPIO_IS_VALID_OUTPUT_GPIO(cs) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(dc) || !GPIO_IS_VALID_OUTPUT_GPIO(reset) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(mosi)) return ESP_ERR_INVALID_ARG;
    *controller = EPD_XTEINK_UNKNOWN;
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << dc) | (1ULL << reset), .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&output);
    if (err != ESP_OK) return err;
    err = gpio_set_pull_mode(mosi, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) return err;
    const spi_device_interface_config_t config = {
        .clock_speed_hz = 500000, .mode = 0, .spics_io_num = cs, .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    spi_device_handle_t spi = NULL;
    err = solar_os_bus_spi_add_device(spi_bus, &config, &spi);
    uint8_t previous[5] = {0}, previous_status = 0;
    for (int pass = 0; err == ESP_OK && pass < 2; ++pass) {
        err = gpio_set_level(reset, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        if (err == ESP_OK) err = gpio_set_level(reset, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        uint8_t version[5] = {0}, status = 0;
        if (err == ESP_OK) err = read_register(spi, dc, 0x71, &status, 1);
        if (err == ESP_OK) err = read_register(spi, dc, 0x70, version, sizeof(version));
        const epd_xteink_controller_t result = epd_xteink_classify(version, status);
        if (err == ESP_OK && (result == EPD_XTEINK_UNKNOWN ||
            (pass && (memcmp(version, previous, sizeof(version)) || status != previous_status))))
            err = ESP_ERR_NOT_SUPPORTED;
        if (err == ESP_OK) {
            memcpy(previous, version, sizeof(version));
            previous_status = status;
            *controller = result;
        }
    }
    if (spi) {
        const esp_err_t removed = spi_bus_remove_device(spi);
        if (err == ESP_OK) err = removed;
    }
    (void)gpio_set_pull_mode(mosi, GPIO_FLOATING);
    if (err != ESP_OK) *controller = EPD_XTEINK_UNKNOWN;
    return err;
}
