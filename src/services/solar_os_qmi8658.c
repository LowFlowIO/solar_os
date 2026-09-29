#include "solar_os_qmi8658.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "qmi8658.h"
#include "solar_os_buses.h"
#include "solar_os_imu.h"

#define QMI8658_DEVICE_MAX 2U
#define QMI8658_SAMPLE_CAPABILITIES \
    (SOLAR_OS_IMU_CAP_ACCELERATION | SOLAR_OS_IMU_CAP_ANGULAR_VELOCITY)

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    qmi8658_t chip;
} qmi8658_device_t;

static const char *TAG = "qmi8658";
static EXT_RAM_BSS_ATTR qmi8658_device_t devices[QMI8658_DEVICE_MAX];

static esp_err_t chip_read(void *context,
                           uint8_t reg,
                           uint8_t *data,
                           size_t len)
{
    qmi8658_device_t *device = context;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                     reg, data, len);
}

static esp_err_t chip_write(void *context,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    qmi8658_device_t *device = context;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                      reg, data, len);
}

static esp_err_t read_sample(void *context,
                             uint32_t timeout_ms,
                             solar_os_imu_sample_t *sample)
{
    qmi8658_device_t *device = context;
    if (device == NULL || !device->active || sample == NULL ||
        timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const int64_t deadline =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    esp_err_t ret = ESP_OK;
    bool ready = false;
    do {
        ret = qmi8658_data_ready(&device->chip, &ready);
        if (ret != ESP_OK || ready) break;
        if (esp_timer_get_time() >= deadline) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    } while (true);

    qmi8658_sample_t chip_sample;
    if (ret == ESP_OK) ret = qmi8658_read_sample(&device->chip, &chip_sample);
    if (ret == ESP_OK) {
        *sample = (solar_os_imu_sample_t) {
            .timestamp_us = (uint64_t)esp_timer_get_time(),
            .valid = QMI8658_SAMPLE_CAPABILITIES,
        };
        memcpy(sample->acceleration_m_s2,
               chip_sample.acceleration_m_s2,
               sizeof(sample->acceleration_m_s2));
        memcpy(sample->angular_velocity_rad_s,
               chip_sample.angular_velocity_rad_s,
               sizeof(sample->angular_velocity_rad_s));
    }
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_imu_ops_t imu_ops = {
    .read_sample = read_sample,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address)
{
    bool have_i2c = false;
    bool have_address = false;
    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address &&
                   (binding->value == QMI8658_I2C_ADDRESS_LOW ||
                    binding->value == QMI8658_I2C_ADDRESS_HIGH)) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
        solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL) ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(qmi8658_device_t *device)
{
    if (device == NULL) return;
    if (device->mutex != NULL) vSemaphoreDelete(device->mutex);
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_qmi8658_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    qmi8658_device_t *device = NULL;
    for (size_t i = 0; i < QMI8658_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) device = &devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;

    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address),
                        TAG,
                        "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    device->address = address;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    const qmi8658_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = device,
    };
    esp_err_t ret = qmi8658_init(&device->chip, &io);
    if (ret != ESP_OK) {
        clear_device(device);
        return ret;
    }

    const solar_os_imu_registration_t registration = {
        .name = name,
        .driver = "qmi8658",
        .capabilities = QMI8658_SAMPLE_CAPABILITIES,
        .ops = &imu_ops,
        .ctx = device,
    };
    ret = solar_os_imu_register(&registration);
    if (ret != ESP_OK) {
        (void)qmi8658_deinit(&device->chip);
        clear_device(device);
        return ret;
    }

    ESP_LOGI(TAG,
             "%s attached on %s addr=0x%02x revision=0x%02x",
             name,
             i2c_bus,
             address,
             device->chip.revision);
    return ESP_OK;
}

esp_err_t solar_os_qmi8658_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < QMI8658_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            ESP_RETURN_ON_ERROR(solar_os_imu_unregister(name),
                                TAG,
                                "unregister failed");
            const esp_err_t ret = qmi8658_deinit(&devices[i].chip);
            clear_device(&devices[i]);
            return ret;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
