#include "solar_os_pca9685.h"

#include <stdio.h>
#include <string.h>
#include "pca9685.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"
#include "solar_os_pwm.h"

typedef struct {
    bool active;
    bool reserved;
    bool enabled;
} pca9685_output_t;

typedef struct {
    char name[SOLAR_OS_PWM_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    bool have_oe;
    bool registered;
    solar_os_gpio_line_ref_t oe;
    pca9685_t chip;
    pca9685_output_t outputs[PCA9685_CHANNEL_COUNT];
} pca9685_device_t;

/* Device metadata is allocated only on attach, preferring PSRAM. The PWM
 * service already owns each provider's operation mutex. This mutex protects
 * only this driver's attachment table, not ordinary PWM transfers. */
static pca9685_device_t *devices[SOLAR_OS_PWM_PROVIDER_MAX];
static StaticSemaphore_t registry_storage;
static SemaphoreHandle_t registry_mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

static void lock_devices(void)
{
    portENTER_CRITICAL(&init_lock);
    if (registry_mutex == NULL) registry_mutex = xSemaphoreCreateMutexStatic(&registry_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
}

static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    pca9685_device_t *d = ctx;
    return solar_os_bus_i2c_read_reg(d->bus, d->address, reg, data, len);
}

static esp_err_t write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    pca9685_device_t *d = ctx;
    return solar_os_bus_i2c_write_reg(d->bus, d->address, reg, data, len);
}

static void delay_ms(void *ctx, uint32_t ms)
{
    (void)ctx;
    /* Round up even when the scheduler tick is longer than one millisecond. */
    vTaskDelay(pdMS_TO_TICKS(ms) + 1U);
}

static size_t output_count(void *ctx) { (void)ctx; return PCA9685_CHANNEL_COUNT; }

static bool get_output(void *ctx, size_t index, solar_os_pwm_output_info_t *info)
{
    if (index >= PCA9685_CHANNEL_COUNT || info == NULL) return false;
    const pca9685_device_t *d = ctx;
    const pca9685_output_t *output = &d->outputs[index];
    const uint32_t divider = d->chip.prescale + 1U;
    const uint32_t clock = d->chip.oscillator_hz;
    const uint32_t period_ticks = divider * PCA9685_TICKS;
    const uint16_t ticks = d->chip.ticks[index];
    *info = (solar_os_pwm_output_info_t){
        .output = (uint16_t)index, .frequency_group = 0U, .pin = -1,
        .runtime_allowed = true, .active = output->active, .channel = (int)index,
        .freq_hz = (clock + period_ticks / 2U) / period_ticks,
        .duty_percent = (uint8_t)((ticks * 100U + PCA9685_TICKS / 2U) / PCA9685_TICKS),
        .enabled = output->enabled, .reserved = output->reserved, .resolution_bits = 12U,
        .pulse_width_us = (uint32_t)(((uint64_t)ticks * divider * 1000000U + clock / 2U) / clock),
    };
    return true;
}

static esp_err_t configure(pca9685_device_t *d, uint16_t channel, uint32_t frequency,
                           uint32_t value, bool pulse, bool reserved)
{
    if (channel >= PCA9685_CHANNEL_COUNT || (d->outputs[channel].active && reserved) ||
        d->outputs[channel].reserved) return channel >= PCA9685_CHANNEL_COUNT ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    uint8_t prescale;
    esp_err_t ret = pca9685_prescale(d->chip.oscillator_hz, frequency, &prescale);
    if (ret != ESP_OK) return ret;
    for (size_t i = 0; i < PCA9685_CHANNEL_COUNT; i++) {
        if (i != channel && d->outputs[i].active && prescale != d->chip.prescale) return ESP_ERR_INVALID_STATE;
    }
    uint16_t ticks;
    if (pulse) {
        ret = pca9685_pulse_ticks(d->chip.oscillator_hz, prescale, value, &ticks);
        if (ret != ESP_OK) return ret;
    } else {
        if (value > 100U) return ESP_ERR_INVALID_ARG;
        ticks = (uint16_t)((value * PCA9685_TICKS + 50U) / 100U);
    }
    ret = pca9685_write(&d->chip, (uint8_t)channel, prescale, ticks);
    if (ret == ESP_OK) d->outputs[channel] = (pca9685_output_t){.active = true, .reserved = reserved, .enabled = ticks != 0U};
    return ret;
}

static esp_err_t set(void *ctx, uint16_t channel, uint32_t frequency, uint32_t value, bool pulse)
{ return configure(ctx, channel, frequency, value, pulse, false); }

static esp_err_t open_output(void *ctx, uint16_t channel, uint32_t frequency, const char *owner)
{ (void)owner; return configure(ctx, channel, frequency, 0U, false, true); }

static esp_err_t write_pulse(void *ctx, uint16_t channel, const char *owner, uint32_t pulse)
{
    (void)owner;
    pca9685_device_t *d = ctx;
    if (channel >= PCA9685_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!d->outputs[channel].reserved) return ESP_ERR_INVALID_STATE;
    uint16_t ticks;
    esp_err_t ret = pca9685_pulse_ticks(d->chip.oscillator_hz, d->chip.prescale, pulse, &ticks);
    if (ret == ESP_OK) ret = pca9685_write(&d->chip, (uint8_t)channel, d->chip.prescale, ticks);
    if (ret == ESP_OK) d->outputs[channel].enabled = ticks != 0U;
    return ret;
}

static esp_err_t disable_output(void *ctx, uint16_t channel, const char *owner)
{
    (void)owner;
    pca9685_device_t *d = ctx;
    if (channel >= PCA9685_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!d->outputs[channel].active) return ESP_ERR_NOT_FOUND;
    const esp_err_t ret = pca9685_disable(&d->chip, (uint8_t)channel);
    if (ret == ESP_OK) d->outputs[channel].enabled = false;
    return ret;
}

static esp_err_t close_output(void *ctx, uint16_t channel, const char *owner)
{
    const esp_err_t ret = disable_output(ctx, channel, owner);
    if (ret == ESP_OK) ((pca9685_device_t *)ctx)->outputs[channel] = (pca9685_output_t){0};
    return ret;
}
static esp_err_t stop_output(void *ctx, uint16_t channel) { return close_output(ctx, channel, NULL); }

static const solar_os_pwm_ops_t ops = {
    .output_count = output_count, .get_output = get_output, .set = set,
    .open = open_output, .write_pulse = write_pulse, .disable = disable_output,
    .close = close_output, .stop = stop_output,
};

static esp_err_t parse_bindings(pca9685_device_t *d,
                                const solar_os_expansion_binding_t *bindings, size_t count,
                                uint32_t *oscillator_hz)
{
    bool have_bus = false, have_address = false, have_clock = false;
    const char *address_bus = NULL;
    *oscillator_hz = PCA9685_OSCILLATOR_HZ;
    if (bindings == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_bus) {
            if (strlcpy(d->bus, b->target, sizeof(d->bus)) >= sizeof(d->bus)) return ESP_ERR_INVALID_ARG;
            have_bus = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS && !have_address &&
                   b->value >= 0x40 && b->value <= 0x77 && b->value != 0x70) {
            d->address = (uint8_t)b->value;
            address_bus = b->target;
            have_address = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE && !d->have_oe && strcmp(b->role, "oe") == 0) {
            if (b->value < 0 || b->value > UINT8_MAX ||
                strlcpy(d->oe.controller, b->target, sizeof(d->oe.controller)) >= sizeof(d->oe.controller)) return ESP_ERR_INVALID_ARG;
            d->oe.line = (uint8_t)b->value;
            d->have_oe = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !have_clock &&
                   strcmp(b->role, "osc_hz") == 0 && b->value >= 20000000 && b->value <= 30000000) {
            *oscillator_hz = (uint32_t)b->value;
            have_clock = true;
        } else return ESP_ERR_INVALID_ARG;
    }
    return have_bus && have_address &&
        (address_bus[0] == '\0' || strcmp(address_bus, d->bus) == 0) &&
        solar_os_expansion_find_i2c_bus(d->bus, NULL, NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_pca9685_attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    solar_os_pwm_output_t endpoint;
    if (name == NULL || strnlen(name, SOLAR_OS_PWM_NAME_MAX) >= SOLAR_OS_PWM_NAME_MAX) return ESP_ERR_INVALID_ARG;
    /* Validate the same provider-name alphabet before touching hardware. */
    char text[SOLAR_OS_PWM_ENDPOINT_MAX];
    snprintf(text, sizeof(text), "%s:0", name);
    if (!solar_os_pwm_parse_output(text, &endpoint)) return ESP_ERR_INVALID_ARG;
    lock_devices();
    size_t slot = SOLAR_OS_PWM_PROVIDER_MAX;
    esp_err_t ret = ESP_OK;
    solar_os_pwm_provider_info_t provider;
    for (size_t i = 0; solar_os_pwm_get_provider(i, &provider); i++) {
        if (strcmp(provider.name, name) == 0) ret = ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK && solar_os_pwm_provider_count() >= SOLAR_OS_PWM_PROVIDER_MAX) ret = ESP_ERR_NO_MEM;
    for (size_t i = 0; i < SOLAR_OS_PWM_PROVIDER_MAX; i++) {
        if (devices[i] != NULL && strcmp(devices[i]->name, name) == 0) ret = ESP_ERR_INVALID_STATE;
        if (devices[i] == NULL && slot == SOLAR_OS_PWM_PROVIDER_MAX) slot = i;
    }
    pca9685_device_t *d = NULL;
    if (ret == ESP_OK && slot == SOLAR_OS_PWM_PROVIDER_MAX) ret = ESP_ERR_NO_MEM;
    if (ret == ESP_OK) {
        d = solar_os_memory_calloc(1U, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "pca9685.device");
        if (d == NULL) ret = ESP_ERR_NO_MEM;
    }
    uint32_t oscillator_hz = PCA9685_OSCILLATOR_HZ;
    if (ret == ESP_OK) ret = parse_bindings(d, bindings, count, &oscillator_hz);
    if (ret == ESP_OK) {
        strlcpy(d->name, name, sizeof(d->name));
        if (d->have_oe) ret = solar_os_gpio_line_configure(&d->oe, SOLAR_OS_GPIO_LINE_MODE_OUTPUT, SOLAR_OS_GPIO_LINE_PULL_NONE);
        if (ret == ESP_OK && d->have_oe) ret = solar_os_gpio_line_write(&d->oe, true);
        if (ret == ESP_OK) ret = solar_os_bus_i2c_probe(d->bus, d->address);
        const pca9685_io_t io = {.read = read_reg, .write = write_reg, .delay_ms = delay_ms, .ctx = d};
        if (ret == ESP_OK) ret = pca9685_init(&d->chip, &io, oscillator_hz);
        if (ret == ESP_OK && d->have_oe) ret = solar_os_gpio_line_write(&d->oe, false);
        if (ret == ESP_OK) {
            const solar_os_pwm_registration_t registration = {.name = name, .driver = "pca9685",
                .detachable = true, .ops = &ops, .ctx = d};
            ret = solar_os_pwm_register(&registration);
            d->registered = ret == ESP_OK;
        }
    }
    if (ret == ESP_OK) devices[slot] = d;
    else if (d != NULL) {
        if (d->chip.initialized) {
            if (d->have_oe) (void)solar_os_gpio_line_write(&d->oe, true);
            (void)pca9685_deinit(&d->chip);
        }
        solar_os_memory_free(d);
    }
    xSemaphoreGive(registry_mutex);
    return ret;
}

esp_err_t solar_os_pca9685_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    lock_devices();
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < SOLAR_OS_PWM_PROVIDER_MAX; i++) {
        pca9685_device_t *d = devices[i];
        if (d == NULL || strcmp(d->name, name) != 0) continue;
        ret = d->registered ? solar_os_pwm_unregister(name) : ESP_OK;
        if (ret == ESP_OK) {
            d->registered = false;
            if (d->have_oe) ret = solar_os_gpio_line_write(&d->oe, true);
            if (ret == ESP_OK && d->chip.initialized) ret = pca9685_deinit(&d->chip);
            if (ret == ESP_OK) { devices[i] = NULL; solar_os_memory_free(d); }
        }
        break;
    }
    xSemaphoreGive(registry_mutex);
    return ret;
}
