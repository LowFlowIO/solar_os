#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cw2017.h"
#include "solar_os_cw2017.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "freertos/task.h"

static uint8_t registers[256];
static unsigned write_count, delay_count;
static int failed_reg = -1;
static bool corrupt_profile;
static esp_err_t provider_error;
static solar_os_battery_provider_t provider;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n); dst[n] = 0; }
    return len;
}

static esp_err_t read_reg(void *user, uint8_t reg, uint8_t *data, size_t len)
{
    (void)user;
    if (reg == failed_reg) return ESP_ERR_TIMEOUT;
    assert((size_t)reg + len <= sizeof(registers));
    memcpy(data, registers + reg, len);
    if (corrupt_profile && reg == 0x10) data[0] ^= 1;
    return ESP_OK;
}

static esp_err_t write_reg(void *user, uint8_t reg, const uint8_t *data, size_t len)
{
    (void)user;
    if (reg == failed_reg) return ESP_ERR_TIMEOUT;
    assert((size_t)reg + len <= sizeof(registers));
    memcpy(registers + reg, data, len);
    ++write_count;
    if (reg == 0x08 && data[0] == 0) registers[0] = 0x0d;
    return ESP_OK;
}

static void delay_ms(void *user, uint32_t milliseconds)
{
    (void)user;
    assert(milliseconds == 20);
    ++delay_count;
}

void vTaskDelay(TickType_t ticks) { delay_ms(NULL, ticks); }

bool solar_os_expansion_find_i2c_bus(const char *name,
    solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    (void)bus; (void)index;
    return strcmp(name, "i2c0") == 0;
}

esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t addr)
{
    assert(strcmp(name, "i2c0") == 0 && addr == 0x63);
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t addr,
    uint8_t reg, uint8_t *data, size_t len)
{
    assert(strcmp(name, "i2c0") == 0 && addr == 0x63);
    return read_reg(NULL, reg, data, len);
}

esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t addr,
    uint8_t reg, const uint8_t *data, size_t len)
{
    assert(strcmp(name, "i2c0") == 0 && addr == 0x63);
    return write_reg(NULL, reg, data, len);
}

esp_err_t solar_os_battery_register_provider(const char *owner,
    const solar_os_battery_provider_t *value)
{
    assert(strcmp(owner, "battery0") == 0);
    if (provider_error != ESP_OK) return provider_error;
    provider = *value;
    return ESP_OK;
}

esp_err_t solar_os_battery_unregister_provider(const char *owner)
{
    assert(strcmp(owner, "battery0") == 0);
    if (provider_error != ESP_OK) return provider_error;
    memset(&provider, 0, sizeof(provider));
    return ESP_OK;
}

int main(void)
{
    cw2017_t chip;
    cw2017_sample_t sample;
    const cw2017_io_t io = {read_reg, write_reg, delay_ms, NULL};
    assert(cw2017_init(NULL, &io) == ESP_ERR_INVALID_ARG);
    registers[0] = 0xff;
    assert(cw2017_init(&chip, &io) == ESP_ERR_INVALID_RESPONSE);
    registers[0] = 0xa0; registers[8] = 0xf0;
    registers[0x0b] = 0x94;
    registers[0x10] = 0x50;
    assert(cw2017_init(&chip, &io) == ESP_OK);
    assert(registers[8] == 0 && write_count == 2 && delay_count == 2);
    assert(registers[0x0b] == 0x94 && registers[0x10] == 0x50);
    write_count = 0;
    assert(cw2017_init(&chip, &io) == ESP_OK && write_count == 0);
    /* High reserved voltage bits are masked; fractional SOC is rounded. */
    registers[2] = 0xef; registers[3] = 0xa0; /* raw 12192 = 3810 mV */
    registers[4] = 42; registers[5] = 0x80;
    assert(cw2017_read_sample(&chip, &sample) == ESP_OK);
    assert(sample.voltage_mv == 3810 && sample.percent == 43 && sample.percent_valid);
    registers[4] = 100; registers[5] = 255;
    assert(cw2017_read_sample(&chip, &sample) == ESP_OK && sample.percent == 100);
    registers[4] = 101;
    assert(cw2017_read_sample(&chip, &sample) == ESP_OK && !sample.percent_valid);
    registers[4] = 0; registers[5] = 0; /* A genuine empty cell is valid. */
    assert(cw2017_read_sample(&chip, &sample) == ESP_OK && sample.percent_valid);
    registers[0] = 0xa0;
    assert(cw2017_read_sample(&chip, &sample) == ESP_OK && !sample.percent_valid);
    registers[0] = 0x0f;
    registers[0x0b] = 0x14;
    assert(cw2017_read_sample(&chip, &sample) == ESP_OK && !sample.percent_valid);
    registers[0x0b] = 0x94; memset(registers + 0x10, 0xff, 80);
    assert(cw2017_read_sample(&chip, &sample) == ESP_OK && !sample.percent_valid);
    failed_reg = 2;
    assert(cw2017_read_sample(&chip, &sample) == ESP_ERR_TIMEOUT);
    failed_reg = -1;
    uint8_t profile[80] = {0x50, 1, 2, 3};
    assert(cw2017_set_profile(&chip, profile, 79) == ESP_ERR_INVALID_ARG);
    failed_reg = 0x20;
    assert(cw2017_set_profile(&chip, profile, 80) == ESP_ERR_TIMEOUT);
    assert(registers[0x0b] == 0x14);
    failed_reg = -1; corrupt_profile = true;
    assert(cw2017_set_profile(&chip, profile, 80) == ESP_ERR_INVALID_RESPONSE);
    assert(registers[0x0b] == 0x14);
    corrupt_profile = false;
    assert(cw2017_set_profile(&chip, profile, 80) == ESP_OK);
    assert(memcmp(registers + 0x10, profile, 80) == 0 && registers[0x0b] == 0x94);

    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x63},
    };
    bindings[1].value = 0x64;
    assert(solar_os_cw2017_attach("battery0", bindings, 2) == ESP_ERR_INVALID_ARG);
    bindings[1].value = 0x63;
    provider_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_cw2017_attach("battery0", bindings, 2) == ESP_ERR_INVALID_STATE);
    provider_error = ESP_OK;
    assert(solar_os_cw2017_attach("battery0", bindings, 2) == ESP_OK);
    assert(solar_os_cw2017_attach("battery0", bindings, 2) == ESP_ERR_INVALID_STATE);
    solar_os_battery_sample_t value;
    assert(provider.read(provider.user, &value) == ESP_OK);
    assert(value.battery_mv == 3810 && value.percent_valid && value.calibrated);
    assert(!value.charging_valid && !value.external_power_valid);
    provider_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_cw2017_detach("battery0") == ESP_ERR_INVALID_STATE);
    assert(provider.read(provider.user, &value) == ESP_OK);
    provider_error = ESP_OK;
    assert(solar_os_cw2017_detach("wrong") == ESP_ERR_NOT_FOUND);
    assert(solar_os_cw2017_detach("battery0") == ESP_OK);
    assert(solar_os_cw2017_attach("battery0", bindings, 2) == ESP_OK);
    assert(solar_os_cw2017_detach("battery0") == ESP_OK);
    puts("CW2017 driver/provider tests passed");
    return 0;
}
