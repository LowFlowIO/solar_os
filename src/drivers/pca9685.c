#include "pca9685.h"

#include <string.h>

#define MODE1 0x00U
#define MODE2 0x01U
#define LED0 0x06U
#define ALL_LED 0xFAU
#define ALL_OFF_H 0xFDU
#define PRE_SCALE 0xFEU
#define AI 0x20U
#define SLEEP 0x10U
#define EXTCLK 0x40U
#define OUTDRV 0x04U

static esp_err_t write_byte(pca9685_t *chip, uint8_t reg, uint8_t value)
{
    return chip->io.write(chip->io.ctx, reg, &value, 1U);
}

static void encode(uint16_t ticks, uint8_t data[4])
{
    data[0] = 0;
    data[1] = ticks == PCA9685_TICKS ? 0x10U : 0U;
    data[2] = (uint8_t)ticks;
    data[3] = ticks == 0U ? 0x10U : (uint8_t)((ticks >> 8U) & 0x0FU);
}

static esp_err_t write_channel(pca9685_t *chip, uint8_t channel, uint16_t ticks)
{
    uint8_t data[4];
    encode(ticks, data);
    return chip->io.write(chip->io.ctx, LED0 + 4U * channel, data, sizeof(data));
}

static esp_err_t all_off(pca9685_t *chip)
{
    /* A one-byte write also works before auto-increment is enabled. */
    return write_byte(chip, ALL_OFF_H, 0x10U);
}

static esp_err_t program(pca9685_t *chip, uint8_t prescale, const uint16_t *ticks)
{
    esp_err_t ret = all_off(chip);
    if (ret == ESP_OK) ret = write_byte(chip, MODE1, AI | SLEEP);
    if (ret == ESP_OK) ret = write_byte(chip, PRE_SCALE, prescale);
    if (ret == ESP_OK) ret = write_byte(chip, MODE1, AI);
    if (ret != ESP_OK) return ret;
    chip->io.delay_ms(chip->io.ctx, 1U); /* NXP requires at least 500 us. */
    uint8_t data[4U * PCA9685_CHANNEL_COUNT];
    for (size_t i = 0; i < PCA9685_CHANNEL_COUNT; i++) encode(ticks[i], &data[4U * i]);
    /* Outputs update on STOP. Reloading PWM after the orderly shutdown also
     * clears RESTART, without relying on the chip's previous restart state. */
    return chip->io.write(chip->io.ctx, LED0, data, sizeof(data));
}

esp_err_t pca9685_prescale(uint32_t oscillator_hz, uint32_t frequency_hz, uint8_t *prescale)
{
    if (oscillator_hz == 0U || frequency_hz == 0U || prescale == NULL) return ESP_ERR_INVALID_ARG;
    const uint64_t clock = oscillator_hz;
    const uint64_t maximum = (clock + (PCA9685_TICKS * 4U) / 2U) / (PCA9685_TICKS * 4U);
    if ((uint64_t)frequency_hz * PCA9685_TICKS * 256U < clock || frequency_hz > maximum) return ESP_ERR_INVALID_ARG;
    const uint64_t denominator = (uint64_t)PCA9685_TICKS * frequency_hz;
    const uint64_t divisor = ((uint64_t)oscillator_hz + denominator / 2U) / denominator;
    if (divisor < 4U || divisor > 256U) return ESP_ERR_INVALID_ARG;
    *prescale = (uint8_t)(divisor - 1U);
    return ESP_OK;
}

esp_err_t pca9685_pulse_ticks(uint32_t oscillator_hz, uint8_t prescale,
                             uint32_t pulse_us, uint16_t *ticks)
{
    if (oscillator_hz == 0U || prescale < 3U || ticks == NULL) return ESP_ERR_INVALID_ARG;
    const uint64_t period = (uint64_t)(prescale + 1U) * 1000000U;
    const uint64_t numerator = (uint64_t)pulse_us * oscillator_hz;
    if (numerator >= period * PCA9685_TICKS) return ESP_ERR_INVALID_ARG;
    const uint64_t result = (numerator + period / 2U) / period;
    if (pulse_us != 0U && (result == 0U || result >= PCA9685_TICKS)) return ESP_ERR_INVALID_ARG;
    *ticks = (uint16_t)result;
    return ESP_OK;
}

esp_err_t pca9685_init(pca9685_t *chip, const pca9685_io_t *io, uint32_t oscillator_hz)
{
    if (chip == NULL || io == NULL || io->read == NULL || io->write == NULL ||
        io->delay_ms == NULL || oscillator_hz < 20000000U || oscillator_hz > 30000000U) return ESP_ERR_INVALID_ARG;
    uint8_t prescale;
    esp_err_t ret = pca9685_prescale(oscillator_hz, 50U, &prescale);
    if (ret != ESP_OK) return ret;
    memset(chip, 0, sizeof(*chip));
    chip->io = *io;
    chip->oscillator_hz = oscillator_hz;
    uint8_t mode;
    ret = io->read(io->ctx, MODE1, &mode, 1U);
    /* EXTCLK is sticky until power cycle/reset; do not silently use its clock
     * with timing calculated for the internal oscillator. */
    if (ret == ESP_OK && (mode & EXTCLK)) ret = ESP_ERR_NOT_SUPPORTED;
    if (ret == ESP_OK) ret = all_off(chip);
    if (ret == ESP_OK) ret = write_byte(chip, MODE2, OUTDRV);
    if (ret == ESP_OK) ret = program(chip, prescale, chip->ticks);
    if (ret != ESP_OK) return ret;
    chip->prescale = prescale;
    chip->initialized = true;
    return ESP_OK;
}

esp_err_t pca9685_write(pca9685_t *chip, uint8_t channel, uint8_t prescale, uint16_t ticks)
{
    if (chip == NULL || !chip->initialized || chip->faulted) return ESP_ERR_INVALID_STATE;
    if (channel >= PCA9685_CHANNEL_COUNT || prescale < 3U || ticks > PCA9685_TICKS) return ESP_ERR_INVALID_ARG;
    const bool change_frequency = prescale != chip->prescale;
    esp_err_t ret;
    if (change_frequency) {
        uint16_t next[PCA9685_CHANNEL_COUNT];
        memcpy(next, chip->ticks, sizeof(next));
        next[channel] = ticks;
        ret = program(chip, prescale, next);
    } else {
        ret = write_channel(chip, channel, ticks);
    }
    if (ret == ESP_OK) {
        chip->prescale = prescale;
        chip->ticks[channel] = ticks;
    } else {
        const esp_err_t restored = change_frequency ? program(chip, chip->prescale, chip->ticks) :
            write_channel(chip, channel, chip->ticks[channel]);
        if (restored != ESP_OK) {
            chip->faulted = true;
            (void)all_off(chip);
        }
    }
    return ret;
}

esp_err_t pca9685_disable(pca9685_t *chip, uint8_t channel)
{
    if (chip == NULL || !chip->initialized) return ESP_ERR_INVALID_STATE;
    if (channel >= PCA9685_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!chip->faulted) return pca9685_write(chip, channel, chip->prescale, 0U);
    const esp_err_t ret = write_channel(chip, channel, 0U);
    if (ret == ESP_OK) chip->ticks[channel] = 0U;
    return ret;
}

esp_err_t pca9685_deinit(pca9685_t *chip)
{
    if (chip == NULL || !chip->initialized) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = all_off(chip);
    if (ret == ESP_OK) ret = write_byte(chip, MODE1, AI | SLEEP);
    if (ret == ESP_OK) chip->initialized = false;
    else chip->faulted = true;
    return ret;
}
