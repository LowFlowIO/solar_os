#include "solar_os_lc709203f.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lc709203f.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include <string.h>

typedef struct {
    bool active, cached;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX],
        bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint32_t capacity;
    uint8_t apa_override;
    int64_t sampled_us;
    solar_os_lc709203f_t gauge;
    solar_os_battery_sample_t sample;
} gauge_t;
static gauge_t gauge;
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;

static esp_err_t read_reg(void *user, uint8_t reg, uint8_t *data, size_t len) {
    gauge_t *g = user;
    return solar_os_bus_i2c_read_reg(g->bus, SOLAR_OS_LC709203F_ADDRESS, reg,
                                     data, len);
}
static esp_err_t write_reg(void *user, uint8_t reg, const uint8_t *data,
                           size_t len) {
    gauge_t *g = user;
    return solar_os_bus_i2c_write_reg(g->bus, SOLAR_OS_LC709203F_ADDRESS, reg,
                                      data, len);
}
static void delay_ms(void *user, unsigned ms) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(ms));
}
static uint8_t configured_apa(void) {
    solar_os_battery_config_t config;
    solar_os_battery_get_config(&config);
    return gauge.apa_override != 0
               ? gauge.apa_override
               : solar_os_lc709203f_capacity_apa(config.capacity_mah != 0
                                                     ? config.capacity_mah
                                                     : gauge.capacity);
}

static esp_err_t read_battery(void *user, solar_os_battery_sample_t *sample) {
    (void)user;
    if (sample == NULL || lock == NULL)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (gauge.active) {
        const uint8_t apa = configured_apa();
        const bool changed = apa != gauge.gauge.apa;
        const int64_t now = esp_timer_get_time();
        if (!changed && gauge.cached && now - gauge.sampled_us < 2000000) {
            *sample = gauge.sample;
            err = ESP_OK;
        } else {
            err = solar_os_lc709203f_set_apa(&gauge.gauge, apa);
            solar_os_lc709203f_sample_t value;
            if (err == ESP_OK)
                err = solar_os_lc709203f_read_sample(&gauge.gauge, &value);
            if (err == ESP_OK) {
                gauge.sample =
                    (solar_os_battery_sample_t){.battery_mv = value.voltage_mv,
                                                .calibrated = true,
                                                .percent_valid = true,
                                                .percent = value.percent};
                gauge.cached = true;
                gauge.sampled_us = now;
                *sample = gauge.sample;
            } else
                gauge.cached = false;
        }
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t
solar_os_lc709203f_attach(const char *name,
                          const solar_os_expansion_binding_t *bindings,
                          size_t count) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= sizeof(gauge.name) ||
        bindings == NULL)
        return ESP_ERR_INVALID_ARG;
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    bool address = false;
    unsigned profile = 1;
    int temperature = 250;
    uint32_t capacity = 500;
    uint8_t apa_override = 0;
    for (size_t i = 0; i < count; ++i) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS)
            strlcpy(bus, b->target, sizeof(bus));
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                 b->value == SOLAR_OS_LC709203F_ADDRESS)
            address = true;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            if (strcmp(b->role, "capacity") == 0 && b->value >= 1 &&
                b->value <= 100000)
                capacity = (uint32_t)b->value;
            else if (strcmp(b->role, "profile") == 0 && b->value >= 0 &&
                     b->value <= 1)
                profile = (unsigned)b->value;
            else if (strcmp(b->role, "temp") == 0 && b->value >= -200 &&
                     b->value <= 600)
                temperature = b->value;
            else if (strcmp(b->role, "apa") == 0 && b->value >= 1 &&
                     b->value <= 255)
                apa_override = (uint8_t)b->value;
            else
                return ESP_ERR_INVALID_ARG;
        } else
            return ESP_ERR_INVALID_ARG;
    }
    if (!address || !solar_os_expansion_find_i2c_bus(bus, NULL, NULL))
        return ESP_ERR_INVALID_ARG;
    if (lock == NULL)
        lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (lock == NULL)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (gauge.active) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&gauge, 0, sizeof(gauge));
    strlcpy(gauge.name, name, sizeof(gauge.name));
    strlcpy(gauge.bus, bus, sizeof(gauge.bus));
    gauge.capacity = capacity;
    gauge.apa_override = apa_override;
    const solar_os_lc709203f_io_t io = {.read = read_reg,
                                        .write = write_reg,
                                        .delay_ms = delay_ms,
                                        .user = &gauge};
    esp_err_t err = solar_os_lc709203f_init(&gauge.gauge, &io, configured_apa(),
                                            profile, temperature);
    if (err == ESP_OK) {
        gauge.active = true;
        const solar_os_battery_provider_t provider = {.read = read_battery,
                                                      .user = &gauge};
        err = solar_os_battery_register_provider(name, &provider);
    }
    if (err != ESP_OK)
        memset(&gauge, 0, sizeof(gauge));
    xSemaphoreGive(lock);
    return err;
}

esp_err_t solar_os_lc709203f_detach(const char *name) {
    if (lock == NULL || name == NULL)
        return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!gauge.active || strcmp(name, gauge.name) != 0) {
        xSemaphoreGive(lock);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = solar_os_battery_unregister_provider(name);
    /* Keep the gauge operating on battery after detach; sleeping would lose
     * tracking. */
    if (err == ESP_OK)
        memset(&gauge, 0, sizeof(gauge));
    xSemaphoreGive(lock);
    return err;
}
