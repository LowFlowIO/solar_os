#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define QMI8658_I2C_ADDRESS_LOW 0x6AU
#define QMI8658_I2C_ADDRESS_HIGH 0x6BU
#define QMI8658_WHO_AM_I 0x05U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} qmi8658_io_t;

typedef struct {
    float acceleration_m_s2[3];
    float angular_velocity_rad_s[3];
} qmi8658_sample_t;

typedef struct {
    qmi8658_io_t io;
    uint8_t revision;
    bool initialized;
} qmi8658_t;

esp_err_t qmi8658_init(qmi8658_t *device, const qmi8658_io_t *io);
esp_err_t qmi8658_deinit(qmi8658_t *device);
esp_err_t qmi8658_data_ready(qmi8658_t *device, bool *ready);
esp_err_t qmi8658_read_sample(qmi8658_t *device, qmi8658_sample_t *sample);
