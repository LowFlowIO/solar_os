#include "qmi8658.h"

#include <string.h>

#define REG_WHO_AM_I 0x00U
#define REG_REVISION 0x01U
#define REG_CTRL1 0x02U
#define REG_CTRL2 0x03U
#define REG_CTRL3 0x04U
#define REG_CTRL5 0x06U
#define REG_CTRL7 0x08U
#define REG_STATUS0 0x2EU
#define REG_ACCEL_X_L 0x35U

#define CTRL1_VENDOR_DEFAULT 0x60U
#define CTRL2_ACCEL_8G_1000HZ 0x23U
#define CTRL3_GYRO_512DPS_1000HZ 0x43U
#define CTRL7_ACCEL_ENABLE (1U << 0)
#define CTRL7_GYRO_ENABLE (1U << 1)
#define STATUS0_ACCEL_AVAILABLE (1U << 0)
#define STATUS0_GYRO_AVAILABLE (1U << 1)

#define STANDARD_GRAVITY_M_S2 9.80665F
#define DEGREES_TO_RADIANS 0.01745329251994329577F
#define ACCEL_LSB_PER_G 4096.0F
#define GYRO_LSB_PER_DPS 64.0F

static bool device_valid(const qmi8658_t *device)
{
    return device != NULL && device->initialized &&
        device->io.read != NULL && device->io.write != NULL;
}

static esp_err_t read_u8(qmi8658_t *device, uint8_t reg, uint8_t *value)
{
    return device->io.read(device->io.ctx, reg, value, 1U);
}

static esp_err_t write_u8(qmi8658_t *device, uint8_t reg, uint8_t value)
{
    return device->io.write(device->io.ctx, reg, &value, 1U);
}

static int16_t decode_i16(const uint8_t data[2])
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

esp_err_t qmi8658_init(qmi8658_t *device, const qmi8658_io_t *io)
{
    if (device == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(device, 0, sizeof(*device));
    device->io = *io;

    uint8_t identity = 0U;
    esp_err_t ret = ESP_OK;
    bool identity_read = false;
    for (size_t attempt = 0; attempt < 5U; attempt++) {
        ret = read_u8(device, REG_WHO_AM_I, &identity);
        if (ret == ESP_OK) {
            identity_read = true;
            if (identity == QMI8658_WHO_AM_I) break;
        }
    }
    if (identity != QMI8658_WHO_AM_I) {
        ret = identity_read ? ESP_ERR_NOT_FOUND : ret;
    }
    if (ret == ESP_OK) ret = read_u8(device, REG_REVISION, &device->revision);
    if (ret == ESP_OK) ret = write_u8(device, REG_CTRL7, 0U);
    if (ret == ESP_OK) ret = write_u8(device, REG_CTRL1, CTRL1_VENDOR_DEFAULT);
    if (ret == ESP_OK) ret = write_u8(device, REG_CTRL2,
                                      CTRL2_ACCEL_8G_1000HZ);
    if (ret == ESP_OK) ret = write_u8(device, REG_CTRL3,
                                      CTRL3_GYRO_512DPS_1000HZ);
    if (ret == ESP_OK) ret = write_u8(device, REG_CTRL5, 0U);
    if (ret == ESP_OK) ret = write_u8(device, REG_CTRL7,
                                      CTRL7_ACCEL_ENABLE |
                                      CTRL7_GYRO_ENABLE);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    device->initialized = true;
    return ESP_OK;
}

esp_err_t qmi8658_deinit(qmi8658_t *device)
{
    if (!device_valid(device)) return ESP_ERR_INVALID_STATE;
    const esp_err_t ret = write_u8(device, REG_CTRL7, 0U);
    if (ret == ESP_OK) memset(device, 0, sizeof(*device));
    return ret;
}

esp_err_t qmi8658_data_ready(qmi8658_t *device, bool *ready)
{
    if (!device_valid(device) || ready == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t status = 0U;
    const esp_err_t ret = read_u8(device, REG_STATUS0, &status);
    if (ret == ESP_OK) {
        const uint8_t mask = STATUS0_ACCEL_AVAILABLE |
            STATUS0_GYRO_AVAILABLE;
        *ready = (status & mask) == mask;
    }
    return ret;
}

esp_err_t qmi8658_read_sample(qmi8658_t *device, qmi8658_sample_t *sample)
{
    if (!device_valid(device) || sample == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t raw[12] = {0};
    const esp_err_t ret = device->io.read(device->io.ctx,
                                          REG_ACCEL_X_L,
                                          raw,
                                          sizeof(raw));
    if (ret != ESP_OK) return ret;

    const float acceleration_scale = STANDARD_GRAVITY_M_S2 / ACCEL_LSB_PER_G;
    const float angular_velocity_scale =
        DEGREES_TO_RADIANS / GYRO_LSB_PER_DPS;
    for (size_t i = 0; i < 3U; i++) {
        sample->acceleration_m_s2[i] =
            (float)decode_i16(&raw[i * 2U]) * acceleration_scale;
        sample->angular_velocity_rad_s[i] =
            (float)decode_i16(&raw[6U + i * 2U]) * angular_velocity_scale;
    }
    return ESP_OK;
}
