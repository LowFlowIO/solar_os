#include "solar_os_pwm_native.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_resources.h"

#if SOLAR_OS_BOARD_HAS_PWM
#include "driver/gpio.h"
#include "pwm_port.h"
#endif

#include "solar_os_gpio.h"

#if SOLAR_OS_BOARD_HAS_PWM
static void pwm_resource_owner(int pin, char *owner, size_t owner_size)
{
    snprintf(owner, owner_size, "pwm:%d", pin);
}
#endif

static esp_err_t native_init(void)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    return ESP_ERR_NOT_SUPPORTED;
#else
    return pwm_port_init();
#endif
}

static size_t native_pin_count(void)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    return 0;
#else
    size_t count = 0;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t gpio_info;
        if (solar_os_gpio_get_pin_info(i, &gpio_info) && gpio_info.runtime_allowed) {
            count++;
        }
    }
    return count;
#endif
}

static bool native_get_pin_info(size_t index, solar_os_pwm_pin_info_t *info)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    (void)index;
    (void)info;
    return false;
#else
    if (info == NULL) {
        return false;
    }

    size_t current = 0;
    for (size_t i = 0; i < solar_os_gpio_pin_count(); i++) {
        solar_os_gpio_pin_info_t gpio_info;
        if (!solar_os_gpio_get_pin_info(i, &gpio_info) || !gpio_info.runtime_allowed) {
            continue;
        }
        if (current++ != index) {
            continue;
        }

        pwm_port_status_t status = {0};
        (void)pwm_port_get((gpio_num_t)gpio_info.pin, &status);
        *info = (solar_os_pwm_pin_info_t) {
            .pin = gpio_info.pin,
            .output = (uint16_t)gpio_info.pin,
            .frequency_group = 0U,
            .runtime_allowed = true,
            .active = status.active,
            .channel = status.active ? (int)status.channel : -1,
            .freq_hz = status.freq_hz,
            .duty_percent = status.duty_percent,
            .enabled = status.enabled,
            .reserved = status.reserved,
            .resolution_bits = status.resolution_bits,
            .pulse_width_us = status.pulse_width_us,
        };
        return true;
    }
    return false;
#endif
}

#if SOLAR_OS_BOARD_HAS_PWM
static esp_err_t pwm_claim(int pin, const char *owner)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const solar_os_resource_request_t requests[] = {
        {SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, "pwm-gpio"},
        {SOLAR_OS_RESOURCE_PWM_PIN, pin, -1, "pwm"},
    };
    return solar_os_resource_claim_bundle(requests, 2U, owner, NULL);
#else
    (void)pin;
    (void)owner;
    return ESP_OK;
#endif
}

static void pwm_release(int pin, const char *owner)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    (void)solar_os_resource_release(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, owner);
    (void)solar_os_resource_release(SOLAR_OS_RESOURCE_PWM_PIN, pin, -1, owner);
#else
    (void)pin;
    (void)owner;
#endif
}
#endif

static esp_err_t pwm_set(int pin, uint32_t freq_hz, uint32_t value, bool pulse)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    (void)pin;
    (void)freq_hz;
    (void)value;
    (void)pulse;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (freq_hz < SOLAR_OS_PWM_FREQ_MIN_HZ ||
        freq_hz > SOLAR_OS_PWM_FREQ_MAX_HZ ||
        (pulse ? (uint64_t)value * freq_hz >= 1000000U :
                 value > SOLAR_OS_PWM_DUTY_MAX_PERCENT)) {
        return ESP_ERR_INVALID_ARG;
    }
    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    pwm_resource_owner(pin, owner, sizeof(owner));
    pwm_port_status_t previous = {0};
    (void)pwm_port_get((gpio_num_t)pin, &previous);
    esp_err_t ret = pwm_claim(pin, owner);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = pulse ? pwm_port_set_pulse((gpio_num_t)pin, freq_hz, value) :
        pwm_port_set((gpio_num_t)pin, freq_hz, (uint8_t)value);
    /* An unsuccessful update must retain claims for the existing output. */
    if (ret != ESP_OK && !previous.active) pwm_release(pin, owner);
    return ret;
#endif
}

static esp_err_t native_set(int pin, uint32_t freq_hz, uint8_t duty_percent)
{
    return pwm_set(pin, freq_hz, duty_percent, false);
}

static esp_err_t native_set_pulse(int pin, uint32_t freq_hz, uint32_t pulse_us)
{
    return pwm_set(pin, freq_hz, pulse_us, true);
}

static esp_err_t native_open(int pin, uint32_t freq_hz, const char *owner)
{
    if (owner == NULL || owner[0] == '\0' || strlen(owner) >= SOLAR_OS_RESOURCE_OWNER_MAX ||
        freq_hz < SOLAR_OS_PWM_FREQ_MIN_HZ || freq_hz > SOLAR_OS_PWM_FREQ_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_PWM
    (void)pin;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_gpio_is_runtime_allowed(pin)) return ESP_ERR_NOT_ALLOWED;
    /* Do not let rollback release a previously reserved output's claims. */
    pwm_port_status_t previous = {0};
    (void)pwm_port_get((gpio_num_t)pin, &previous);
    if (previous.active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = pwm_claim(pin, owner);
    if (ret == ESP_OK) {
        ret = pwm_port_open((gpio_num_t)pin, freq_hz, owner);
        if (ret != ESP_OK) pwm_release(pin, owner);
    }
    return ret;
#endif
}

static esp_err_t native_write_pulse(int pin, const char *owner, uint32_t pulse_us)
{
#if SOLAR_OS_BOARD_HAS_PWM
    return pwm_port_write_pulse((gpio_num_t)pin, owner, pulse_us);
#else
    (void)pin; (void)owner; (void)pulse_us;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t native_disable(int pin, const char *owner)
{
#if SOLAR_OS_BOARD_HAS_PWM
    return pwm_port_disable((gpio_num_t)pin, owner);
#else
    (void)pin; (void)owner;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t native_close(int pin, const char *owner)
{
#if SOLAR_OS_BOARD_HAS_PWM
    const esp_err_t ret = pwm_port_close((gpio_num_t)pin, owner);
    if (ret == ESP_OK) pwm_release(pin, owner);
    return ret;
#else
    (void)pin; (void)owner;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t native_stop(int pin)
{
#if !SOLAR_OS_BOARD_HAS_PWM
    (void)pin;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_gpio_is_runtime_allowed(pin)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    pwm_resource_owner(pin, owner, sizeof(owner));
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_resource_claim_t claim;
    if (solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim) &&
        strcmp(claim.owner, owner) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    const esp_err_t ret = pwm_port_stop((gpio_num_t)pin);
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    if (ret == ESP_OK) {
        pwm_release(pin, owner);
    }
#endif
    return ret;
#endif
}

static size_t provider_count(void *ctx)
{
    (void)ctx;
    return native_pin_count();
}

static bool provider_get(void *ctx, size_t index, solar_os_pwm_output_info_t *info)
{
    (void)ctx;
    return native_get_pin_info(index, info);
}

static esp_err_t provider_set(void *ctx, uint16_t output, uint32_t frequency, uint32_t value, bool pulse)
{
    (void)ctx;
    return pulse ? native_set_pulse(output, frequency, value) :
        native_set(output, frequency, (uint8_t)value);
}

static esp_err_t provider_open(void *ctx, uint16_t output, uint32_t frequency, const char *owner)
{
    (void)ctx;
    return native_open(output, frequency, owner);
}

static esp_err_t provider_write(void *ctx, uint16_t output, const char *owner, uint32_t pulse)
{
    (void)ctx;
    return native_write_pulse(output, owner, pulse);
}

static esp_err_t provider_disable(void *ctx, uint16_t output, const char *owner)
{
    (void)ctx;
    return native_disable(output, owner);
}

static esp_err_t provider_close(void *ctx, uint16_t output, const char *owner)
{
    (void)ctx;
    return native_close(output, owner);
}

static esp_err_t provider_stop(void *ctx, uint16_t output)
{
    (void)ctx;
    return native_stop(output);
}

esp_err_t solar_os_pwm_native_register(void)
{
    const esp_err_t ret = native_init();
    if (ret != ESP_OK) return ret;
    static const solar_os_pwm_ops_t ops = {
        .output_count = provider_count, .get_output = provider_get,
        .set = provider_set, .open = provider_open, .write_pulse = provider_write,
        .disable = provider_disable, .close = provider_close, .stop = provider_stop,
    };
    const solar_os_pwm_registration_t registration = {
        .name = "native", .driver = "esp32-ledc", .ops = &ops,
    };
    return solar_os_pwm_register_native(&registration);
}
