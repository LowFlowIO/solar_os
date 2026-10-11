#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pca9685.h"

typedef struct {
    uint8_t registers[256];
    int fail_reg;
    unsigned failures;
    unsigned fail_after;
    bool persistent;
    unsigned delay;
} fake_chip_t;

static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    fake_chip_t *fake = ctx;
    assert((size_t)reg + len <= sizeof(fake->registers));
    memcpy(data, &fake->registers[reg], len);
    return ESP_OK;
}

static esp_err_t write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    fake_chip_t *fake = ctx;
    assert((size_t)reg + len <= sizeof(fake->registers));
    if (reg == 0xfe) assert(fake->registers[0] & 0x10); /* SLEEP required. */
    if (len > 1) assert(fake->registers[0] & 0x20); /* Auto-increment. */
    const bool fail_now = fake->fail_after > 0U && --fake->fail_after == 0U;
    if (fail_now || (fake->fail_reg == reg && (fake->persistent || fake->failures > 0U))) {
        if (fake->failures > 0U) fake->failures--;
        /* An I2C error can follow a partial transfer. */
        if (len > 1) fake->registers[reg] = data[0];
        return ESP_FAIL;
    }
    memcpy(&fake->registers[reg], data, len);
    if (reg == 0xfd && len == 1) {
        for (size_t i = 0; i < 16; i++) fake->registers[0x09 + 4 * i] = data[0];
    }
    return ESP_OK;
}

static void delay(void *ctx, uint32_t ms) { ((fake_chip_t *)ctx)->delay += ms; }

int main(void)
{
    fake_chip_t fake = {.fail_reg = -1};
    fake.registers[0] = 0x01; /* Power-on All Call enabled. */
    const pca9685_io_t io = {.read = read_reg, .write = write_reg, .delay_ms = delay, .ctx = &fake};
    pca9685_t chip;
    assert(pca9685_init(&chip, &io, PCA9685_OSCILLATOR_HZ) == ESP_OK);
    assert(fake.registers[0] == 0x20 && fake.registers[1] == 0x04);
    assert(chip.prescale == 121 && fake.registers[0xfe] == 121 && fake.delay >= 1);
    for (size_t i = 0; i < 16; i++) {
        assert(fake.registers[0x06 + 4 * i] == 0);
        assert(fake.registers[0x09 + 4 * i] == 0x10); /* Every output full-off. */
    }
    uint8_t prescale;
    assert(pca9685_prescale(25000000, 24, &prescale) == ESP_OK);
    assert(pca9685_prescale(25000000, 1526, &prescale) == ESP_OK && prescale == 3);
    assert(pca9685_prescale(25000000, 23, &prescale) == ESP_ERR_INVALID_ARG);
    assert(pca9685_prescale(25000000, 2000, &prescale) == ESP_ERR_INVALID_ARG);
    assert(pca9685_prescale(25000000, 1700, &prescale) == ESP_ERR_INVALID_ARG);
    assert(pca9685_prescale(25000000, 0, &prescale) == ESP_ERR_INVALID_ARG);
    assert(pca9685_prescale(26000000, 50, &prescale) == ESP_OK && prescale == 126);
    uint16_t ticks;
    assert(pca9685_pulse_ticks(25000000, 121, 1500, &ticks) == ESP_OK && ticks == 307);
    assert(pca9685_pulse_ticks(25000000, 121, 19989, &ticks) == ESP_ERR_INVALID_ARG);
    assert(pca9685_pulse_ticks(25000000, 121, 1, &ticks) == ESP_ERR_INVALID_ARG);
    assert(pca9685_pulse_ticks(25000000, 121, 0, &ticks) == ESP_OK && ticks == 0);
    assert(pca9685_write(&chip, 16, 121, 300) == ESP_ERR_INVALID_ARG);
    assert(pca9685_write(&chip, 0, 121, 4097) == ESP_ERR_INVALID_ARG);
    assert(pca9685_write(&chip, 0, 121, 307) == ESP_OK);
    assert(fake.registers[0x08] == 0x33 && fake.registers[0x09] == 0x01);
    assert(pca9685_write(&chip, 1, 121, 4096) == ESP_OK);
    assert(fake.registers[0x0b] == 0x10 && fake.registers[0x0d] == 0); /* Full-on. */

    fake.fail_reg = 0x06;
    fake.failures = 1;
    assert(pca9685_write(&chip, 0, 121, 400) == ESP_FAIL);
    assert(chip.ticks[0] == 307 && !chip.faulted);
    assert(fake.registers[0x08] == 0x33 && fake.registers[0x09] == 0x01);
    /* Failure after entering sleep restores frequency and all channel words. */
    fake.fail_reg = 0xfe;
    fake.failures = 1;
    assert(pca9685_write(&chip, 0, 100, 400) == ESP_FAIL);
    assert(chip.prescale == 121 && fake.registers[0xfe] == 121 && fake.registers[0] == 0x20);
    assert(fake.registers[0x08] == 0x33 && fake.registers[0x0b] == 0x10);
    fake.fail_reg = -1;
    for (unsigned step = 1; step <= 5; step++) {
        fake.fail_after = step;
        assert(pca9685_write(&chip, 0, 100, 400) == ESP_FAIL);
        assert(!chip.faulted && chip.prescale == 121 && chip.ticks[0] == 307);
        assert(fake.registers[0] == 0x20 && fake.registers[0xfe] == 121);
        assert(fake.registers[0x08] == 0x33 && fake.registers[0x0b] == 0x10);
    }
    assert(pca9685_write(&chip, 0, 100, 400) == ESP_OK);
    assert(chip.prescale == 100 && chip.ticks[0] == 400 && chip.ticks[1] == 4096);
    assert(fake.registers[0xfe] == 100);
    assert(pca9685_disable(&chip, 1) == ESP_OK && fake.registers[0x0d] == 0x10);

    fake.fail_reg = 0x06;
    fake.persistent = true;
    assert(pca9685_write(&chip, 0, 100, 200) == ESP_FAIL && chip.faulted);
    assert(pca9685_write(&chip, 2, 100, 200) == ESP_ERR_INVALID_STATE);
    fake.persistent = false;
    assert(pca9685_disable(&chip, 0) == ESP_OK);
    fake.fail_reg = 0xfd;
    fake.failures = 1;
    assert(pca9685_deinit(&chip) == ESP_FAIL && chip.initialized);
    assert(pca9685_deinit(&chip) == ESP_OK && !chip.initialized);
    assert(fake.registers[0] == 0x30);

    fake.registers[0] = 0x40;
    assert(pca9685_init(&chip, &io, 25000000) == ESP_ERR_NOT_SUPPORTED);
    assert(fake.registers[0] == 0x40 && !chip.initialized);
    assert(pca9685_init(&chip, &io, 0) == ESP_ERR_INVALID_ARG);
    puts("PCA9685 register, timing and transfer recovery tests passed");
    return 0;
}
