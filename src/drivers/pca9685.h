#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define PCA9685_CHANNEL_COUNT 16U
#define PCA9685_TICKS 4096U
#define PCA9685_OSCILLATOR_HZ 25000000U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void *ctx;
} pca9685_io_t;

typedef struct {
    pca9685_io_t io;
    uint32_t oscillator_hz;
    uint16_t ticks[PCA9685_CHANNEL_COUNT];
    uint8_t prescale;
    bool initialized;
    bool faulted;
} pca9685_t;

esp_err_t pca9685_init(pca9685_t *chip, const pca9685_io_t *io, uint32_t oscillator_hz);
esp_err_t pca9685_deinit(pca9685_t *chip);
esp_err_t pca9685_prescale(uint32_t oscillator_hz, uint32_t frequency_hz, uint8_t *prescale);
esp_err_t pca9685_pulse_ticks(uint32_t oscillator_hz, uint8_t prescale,
                             uint32_t pulse_us, uint16_t *ticks);
/* Changes frequency and one output together. Failed transfers attempt to
 * restore the previous command; a failed restoration requires detach/reinit. */
esp_err_t pca9685_write(pca9685_t *chip, uint8_t channel, uint8_t prescale, uint16_t ticks);
/* Full-off remains available after an unsuccessful restoration. */
esp_err_t pca9685_disable(pca9685_t *chip, uint8_t channel);
