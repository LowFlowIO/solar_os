#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CW2017_I2C_ADDRESS 0x63U
#define CW2017_PROFILE_SIZE 80U

typedef struct {
    esp_err_t (*read)(void *user, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *user, uint8_t reg, const uint8_t *data, size_t len);
    void (*delay_ms)(void *user, uint32_t milliseconds);
    void *user;
} cw2017_io_t;

typedef struct {
    cw2017_io_t io;
    uint8_t version;
    bool initialized;
} cw2017_t;

typedef struct {
    uint16_t voltage_mv;
    uint16_t soc_raw;
    uint8_t percent;
    bool percent_valid;
} cw2017_sample_t;

/* Wake if needed; preserve the battery model and alert configuration. */
esp_err_t cw2017_init(cw2017_t *device, const cw2017_io_t *io);
esp_err_t cw2017_read_sample(cw2017_t *device, cw2017_sample_t *sample);
/* Explicit provisioning only: the caller supplies the cell-specific BATINFO. */
esp_err_t cw2017_set_profile(cw2017_t *device, const uint8_t *profile, size_t len);
