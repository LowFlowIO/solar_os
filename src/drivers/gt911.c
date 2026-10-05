#include "gt911.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define GT911_REG_PRODUCT_ID 0x8140U
#define GT911_REG_STATUS 0x814eU
#define GT911_REG_POINTS 0x814fU

static const char *TAG = "gt911";
static bool ready;
static char bus_name[SOLAR_OS_BUS_NAME_MAX];
static uint8_t device_address;
static uint8_t configured_address;
static uint8_t configured_alternate_address;
static int configured_reset = -1, configured_power = -1;
static int configured_irq = -1, configured_power_level;

static bool valid_address(uint8_t address)
{
    return address == GT911_ADDRESS || address == GT911_ALTERNATE_ADDRESS;
}

static esp_err_t transfer(uint16_t reg, uint8_t *data, size_t len)
{
    const uint8_t tx[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return solar_os_bus_i2c_transmit_receive(bus_name, device_address,
                                             tx, sizeof(tx), data, len);
}

static esp_err_t write_u8(uint16_t reg, uint8_t value)
{
    const uint8_t tx[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    return solar_os_bus_i2c_transmit(bus_name, device_address, tx, sizeof(tx));
}

esp_err_t gt911_init(const char *i2c_bus,
                     uint8_t address,
                     uint8_t alternate_address,
                     int irq_pin)
{
    return gt911_init_with_reset(i2c_bus, address, alternate_address, irq_pin, -1, -1, 1);
}

static esp_err_t reset_controller(int irq_pin, int reset_pin, uint8_t address)
{
    if (reset_pin < 0) return ESP_OK;
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << irq_pin) | (1ULL << reset_pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "reset pins failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(reset_pin, 0), TAG, "reset low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(irq_pin, address == GT911_ALTERNATE_ADDRESS), TAG, "address select failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(reset_pin, 1), TAG, "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(60));
    const gpio_config_t input = {.pin_bit_mask = 1ULL << irq_pin, .mode = GPIO_MODE_INPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "release irq failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t gt911_init_with_reset(const char *i2c_bus, uint8_t address,
                                uint8_t alternate_address, int irq_pin,
                                int reset_pin, int power_pin, int power_active_level)
{
    if (!i2c_bus || !i2c_bus[0] ||
        !valid_address(address) ||
        (alternate_address != 0U &&
         (!valid_address(alternate_address) || alternate_address == address)) ||
        !GPIO_IS_VALID_GPIO(irq_pin) ||
        (reset_pin >= 0 && (!GPIO_IS_VALID_OUTPUT_GPIO(reset_pin) ||
                           !GPIO_IS_VALID_OUTPUT_GPIO(irq_pin) || reset_pin == irq_pin)) ||
        (power_pin >= 0 && (!GPIO_IS_VALID_OUTPUT_GPIO(power_pin) ||
                           power_pin == irq_pin || power_pin == reset_pin)) ||
        (power_active_level != 0 && power_active_level != 1)) return ESP_ERR_INVALID_ARG;
    if (ready) return strcmp(bus_name, i2c_bus) == 0 &&
        configured_address == address &&
        configured_alternate_address == alternate_address &&
        configured_reset == reset_pin && configured_power == power_pin &&
        configured_irq == irq_pin && configured_power_level == power_active_level
        ? ESP_OK : ESP_ERR_INVALID_STATE;

    configured_reset = reset_pin;
    configured_power = power_pin;
    configured_irq = irq_pin;
    configured_power_level = power_active_level;
    if (power_pin >= 0) {
        const gpio_config_t output = {.pin_bit_mask = 1ULL << power_pin, .mode = GPIO_MODE_OUTPUT};
        ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "power pin failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(power_pin, power_active_level), TAG, "power on failed");
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << (uint32_t)irq_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "irq config failed");
    ESP_RETURN_ON_ERROR(reset_controller(irq_pin, reset_pin, address), TAG, "reset failed");
    strlcpy(bus_name, i2c_bus, sizeof(bus_name));
    configured_address = address;
    configured_alternate_address = alternate_address;
    device_address = address;
    uint8_t product[4] = {0};
    esp_err_t err = transfer(GT911_REG_PRODUCT_ID, product, sizeof(product));
    if (err != ESP_OK && alternate_address != 0U) {
        device_address = alternate_address;
        err = reset_controller(irq_pin, reset_pin, alternate_address);
        if (err == ESP_OK) err = transfer(GT911_REG_PRODUCT_ID, product, sizeof(product));
    }
    if (err != ESP_OK) {
        bus_name[0] = '\0';
        device_address = 0;
        configured_address = 0;
        configured_alternate_address = 0;
        return err;
    }
    ready = true;
    return ESP_OK;
}

esp_err_t gt911_read(gt911_sample_t *sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;
    if (!ready) return ESP_ERR_INVALID_STATE;
    memset(sample, 0, sizeof(*sample));
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(transfer(GT911_REG_STATUS, &status, 1), TAG, "status read failed");
    if (!(status & 0x80U)) return ESP_OK;
    sample->valid = true;
    sample->home = (status & 0x10U) != 0;
    const uint8_t count = status & 0x0fU;
    if (count) {
        uint8_t point[8];
        ESP_RETURN_ON_ERROR(transfer(GT911_REG_POINTS, point, sizeof(point)), TAG,
                            "point read failed");
        sample->touched = true;
        sample->id = point[0] & 0x0fU;
        sample->x = (uint16_t)point[1] | ((uint16_t)point[2] << 8);
        sample->y = (uint16_t)point[3] | ((uint16_t)point[4] << 8);
    }
    return write_u8(GT911_REG_STATUS, 0);
}

void gt911_deinit(void)
{
    if (configured_power >= 0) (void)gpio_set_level(configured_power, !configured_power_level);
    configured_power = configured_reset = configured_irq = -1;
    ready = false;
    bus_name[0] = '\0';
    device_address = 0;
    configured_address = 0;
    configured_alternate_address = 0;
}
