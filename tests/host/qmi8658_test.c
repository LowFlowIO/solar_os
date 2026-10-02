#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "qmi8658.h"

typedef struct {
    uint8_t registers[256];
    esp_err_t read_result;
    esp_err_t write_result;
    size_t read_count;
} fake_chip_t;

static esp_err_t read_regs(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_chip_t *chip = ctx;
    chip->read_count++;
    if (chip->read_result != ESP_OK) return chip->read_result;
    if ((size_t)reg + len > sizeof(chip->registers)) return ESP_ERR_INVALID_ARG;
    memcpy(data, &chip->registers[reg], len);
    return ESP_OK;
}

static esp_err_t write_regs(void *ctx,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    fake_chip_t *chip = ctx;
    if (chip->write_result != ESP_OK) return chip->write_result;
    if ((size_t)reg + len > sizeof(chip->registers)) return ESP_ERR_INVALID_ARG;
    memcpy(&chip->registers[reg], data, len);
    return ESP_OK;
}

static esp_err_t init(fake_chip_t *chip,
                      uint8_t identity,
                      uint8_t revision,
                      qmi8658_t *device)
{
    chip->registers[0x00] = identity;
    chip->registers[0x01] = revision;
    const qmi8658_io_t io = {
        .read = read_regs,
        .write = write_regs,
        .ctx = chip,
    };
    return qmi8658_init(device, &io);
}

static void encode_i16(uint8_t data[2], int16_t value)
{
    data[0] = (uint8_t)((uint16_t)value & 0xFFU);
    data[1] = (uint8_t)((uint16_t)value >> 8U);
}

static void assert_near(float actual, float expected, float tolerance)
{
    assert(fabsf(actual - expected) <= tolerance);
}

int main(void)
{
    fake_chip_t chip = {0};
    qmi8658_t device;
    assert(init(&chip, 0x00U, 0x11U, &device) == ESP_ERR_NOT_FOUND);
    assert(chip.read_count == 5U);
    assert(!device.initialized);

    memset(&chip, 0, sizeof(chip));
    assert(init(&chip, QMI8658_WHO_AM_I, 0x7CU, &device) == ESP_OK);
    assert(device.revision == 0x7CU);
    assert(chip.registers[0x02] == 0x60U);
    assert(chip.registers[0x03] == 0x23U);
    assert(chip.registers[0x04] == 0x43U);
    assert(chip.registers[0x06] == 0x00U);
    assert(chip.registers[0x08] == 0x03U);

    bool ready = true;
    chip.registers[0x2E] = 0x01U;
    assert(qmi8658_data_ready(&device, &ready) == ESP_OK);
    assert(!ready);
    chip.registers[0x2E] = 0x03U;
    assert(qmi8658_data_ready(&device, &ready) == ESP_OK);
    assert(ready);

    int16_t values[] = {4096, -4096, 2048, 64, -64, 32};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        encode_i16(&chip.registers[0x35 + i * 2U], values[i]);
    }
    qmi8658_sample_t sample;
    assert(qmi8658_read_sample(&device, &sample) == ESP_OK);
    assert_near(sample.acceleration_m_s2[0], 9.80665F, 0.0001F);
    assert_near(sample.acceleration_m_s2[1], -9.80665F, 0.0001F);
    assert_near(sample.acceleration_m_s2[2], 4.903325F, 0.0001F);
    assert_near(sample.angular_velocity_rad_s[0], 0.017453293F, 0.000001F);
    assert_near(sample.angular_velocity_rad_s[1], -0.017453293F, 0.000001F);
    assert_near(sample.angular_velocity_rad_s[2], 0.008726646F, 0.000001F);

    assert(qmi8658_deinit(&device) == ESP_OK);
    assert(chip.registers[0x08] == 0U);
    assert(qmi8658_data_ready(&device, &ready) == ESP_ERR_INVALID_ARG);
    puts("QMI8658 tests: ok");
    return 0;
}
