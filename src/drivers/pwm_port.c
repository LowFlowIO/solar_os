#include "pwm_port.h"

#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gpio_port.h"
#include "soc/soc_caps.h"

#define PWM_PORT_CHANNEL_COUNT 4
#define PWM_PORT_OWNER_MAX 24

typedef struct {
    bool active;
    bool enabled;
    gpio_num_t pin;
    ledc_channel_t channel;
    uint32_t duty;
    char owner[PWM_PORT_OWNER_MAX];
} pwm_port_slot_t;

static const char *TAG = "pwm_port";
static pwm_port_slot_t pwm_slots[PWM_PORT_CHANNEL_COUNT] = {
    {.channel = LEDC_CHANNEL_0}, {.channel = LEDC_CHANNEL_1},
    {.channel = LEDC_CHANNEL_2}, {.channel = LEDC_CHANNEL_3},
};
static SemaphoreHandle_t pwm_mutex;
static StaticSemaphore_t pwm_mutex_storage;
static portMUX_TYPE pwm_init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t pwm_timer_freq_hz;
static uint8_t pwm_resolution_bits;

static esp_err_t pwm_port_ensure_init(void)
{
    portENTER_CRITICAL(&pwm_init_lock);
    if (pwm_mutex == NULL) pwm_mutex = xSemaphoreCreateMutexStatic(&pwm_mutex_storage);
    portEXIT_CRITICAL(&pwm_init_lock);
    return pwm_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static pwm_port_slot_t *pwm_slot_for_pin_locked(gpio_num_t pin)
{
    for (size_t i = 0; i < PWM_PORT_CHANNEL_COUNT; i++) {
        if (pwm_slots[i].active && pwm_slots[i].pin == pin) return &pwm_slots[i];
    }
    return NULL;
}

static pwm_port_slot_t *pwm_free_slot_locked(void)
{
    for (size_t i = 0; i < PWM_PORT_CHANNEL_COUNT; i++) {
        if (!pwm_slots[i].active) return &pwm_slots[i];
    }
    return NULL;
}

static uint8_t pwm_resolution(uint32_t frequency)
{
    /* Servo-rate outputs gain precision without reducing the existing 40 kHz
     * range. IDF validates the selected clock/divider on each target. */
    uint8_t bits = frequency <= 1000U ? 14U : 10U;
    return bits <= SOC_LEDC_TIMER_BIT_WIDTH ? bits : SOC_LEDC_TIMER_BIT_WIDTH;
}

static esp_err_t pwm_config_timer_locked(uint32_t frequency)
{
    const uint8_t bits = pwm_resolution(frequency);
    if (pwm_timer_freq_hz == frequency && pwm_resolution_bits == bits) return ESP_OK;
    const ledc_timer_config_t config = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = frequency,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const esp_err_t ret = ledc_timer_config(&config);
    if (ret == ESP_OK) {
        pwm_timer_freq_hz = frequency;
        pwm_resolution_bits = bits;
    }
    return ret;
}

static esp_err_t pwm_check_frequency_locked(const pwm_port_slot_t *slot,
                                             uint32_t frequency)
{
    for (size_t i = 0; i < PWM_PORT_CHANNEL_COUNT; i++) {
        if (pwm_slots[i].active && &pwm_slots[i] != slot &&
            frequency != pwm_timer_freq_hz) return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t pwm_write_locked(pwm_port_slot_t *slot, uint32_t duty)
{
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, slot->channel, duty);
    if (ret == ESP_OK) ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->channel);
    if (ret == ESP_OK) {
        slot->duty = duty;
        slot->enabled = true;
    } else {
        (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, slot->channel, slot->duty);
        (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->channel);
        if (!slot->enabled) (void)ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
    }
    return ret;
}

static esp_err_t pwm_config_slot_locked(pwm_port_slot_t *slot, gpio_num_t pin,
                                         uint32_t frequency, uint32_t duty)
{
    const uint32_t previous_frequency = pwm_timer_freq_hz;
    esp_err_t ret = pwm_check_frequency_locked(slot, frequency);
    if (ret != ESP_OK) return ret;
    ret = pwm_config_timer_locked(frequency);
    if (ret != ESP_OK) return ret;
    if (slot->active) {
        ret = pwm_write_locked(slot, duty);
    } else {
        const ledc_channel_config_t config = {
            .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = slot->channel, .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0, .duty = duty, .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        };
        ret = ledc_channel_config(&config);
        if (ret == ESP_OK) {
            slot->active = true;
            slot->enabled = true;
            slot->pin = pin;
            slot->duty = duty;
        }
    }
    if (ret != ESP_OK && previous_frequency != 0U && previous_frequency != frequency) {
        if (pwm_config_timer_locked(previous_frequency) == ESP_OK && slot->active) {
            /* Restore duty after restoring the old resolution: it may exceed
             * the failed update's smaller timer range. */
            (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, slot->channel, slot->duty);
            (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->channel);
            if (!slot->enabled) (void)ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
        }
    }
    return ret;
}

static esp_err_t pwm_set(gpio_num_t pin, uint32_t frequency, uint32_t value,
                          bool pulse)
{
    if (!gpio_port_is_valid_output_pin(pin) ||
        frequency < PWM_PORT_FREQ_MIN_HZ || frequency > PWM_PORT_FREQ_MAX_HZ ||
        (pulse ? (uint64_t)value * frequency >= 1000000U : value > 100U)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(pwm_port_ensure_init(), TAG, "PWM init failed");
    xSemaphoreTake(pwm_mutex, portMAX_DELAY);
    pwm_port_slot_t *slot = pwm_slot_for_pin_locked(pin);
    esp_err_t ret = ESP_OK;
    if (slot != NULL && slot->owner[0] != '\0') ret = ESP_ERR_INVALID_STATE;
    else {
        if (slot == NULL) slot = pwm_free_slot_locked();
        if (slot == NULL) ret = ESP_ERR_NOT_FOUND;
        else {
            const uint32_t ticks = 1U << pwm_resolution(frequency);
            const uint32_t duty = pulse ?
                (uint32_t)(((uint64_t)value * frequency * ticks + 500000U) / 1000000U) :
                (value * (ticks - 1U) + 50U) / 100U;
            ret = duty < ticks ? pwm_config_slot_locked(slot, pin, frequency, duty) :
                ESP_ERR_INVALID_ARG;
        }
    }
    xSemaphoreGive(pwm_mutex);
    return ret;
}

esp_err_t pwm_port_init(void) { return pwm_port_ensure_init(); }

esp_err_t pwm_port_set(gpio_num_t pin, uint32_t frequency, uint8_t percent)
{
    return pwm_set(pin, frequency, percent, false);
}

esp_err_t pwm_port_set_pulse(gpio_num_t pin, uint32_t frequency, uint32_t pulse_us)
{
    return pwm_set(pin, frequency, pulse_us, true);
}

esp_err_t pwm_port_open(gpio_num_t pin, uint32_t frequency, const char *owner)
{
    if (owner == NULL || owner[0] == '\0' || strlen(owner) >= PWM_PORT_OWNER_MAX ||
        !gpio_port_is_valid_output_pin(pin) || frequency < PWM_PORT_FREQ_MIN_HZ ||
        frequency > PWM_PORT_FREQ_MAX_HZ) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(pwm_port_ensure_init(), TAG, "PWM init failed");
    xSemaphoreTake(pwm_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (pwm_slot_for_pin_locked(pin) == NULL) {
        pwm_port_slot_t *slot = pwm_free_slot_locked();
        ret = slot == NULL ? ESP_ERR_NOT_FOUND :
            pwm_config_slot_locked(slot, pin, frequency, 0U);
        if (ret == ESP_OK) {
            memcpy(slot->owner, owner, strlen(owner) + 1U);
            slot->enabled = false;
        }
    }
    xSemaphoreGive(pwm_mutex);
    return ret;
}

esp_err_t pwm_port_write_pulse(gpio_num_t pin, const char *owner, uint32_t pulse_us)
{
    if (owner == NULL || owner[0] == '\0') return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(pwm_port_ensure_init(), TAG, "PWM init failed");
    xSemaphoreTake(pwm_mutex, portMAX_DELAY);
    pwm_port_slot_t *slot = pwm_slot_for_pin_locked(pin);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (slot != NULL) {
        if (strcmp(slot->owner, owner) != 0) ret = ESP_ERR_INVALID_STATE;
        else if ((uint64_t)pulse_us * pwm_timer_freq_hz >= 1000000U) ret = ESP_ERR_INVALID_ARG;
        else {
            const uint32_t ticks = 1U << pwm_resolution_bits;
            const uint32_t duty = (uint32_t)(((uint64_t)pulse_us *
                pwm_timer_freq_hz * ticks + 500000U) / 1000000U);
            ret = duty < ticks ? pwm_write_locked(slot, duty) : ESP_ERR_INVALID_ARG;
        }
    }
    xSemaphoreGive(pwm_mutex);
    return ret;
}

static esp_err_t pwm_stop(gpio_num_t pin, const char *owner, bool close)
{
    if (owner == NULL) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(pwm_port_ensure_init(), TAG, "PWM init failed");
    xSemaphoreTake(pwm_mutex, portMAX_DELAY);
    pwm_port_slot_t *slot = pwm_slot_for_pin_locked(pin);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (slot != NULL) {
        ret = strcmp(slot->owner, owner) == 0 ?
            ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0U) : ESP_ERR_INVALID_STATE;
        if (ret == ESP_OK) {
            slot->enabled = false;
            if (close) {
                /* Disconnect this pin from the LEDC signal before the channel
                 * is reused, otherwise it could drive both old and new pins. */
                ret = gpio_reset_pin(pin);
                if (ret == ESP_OK) {
                    slot->active = false;
                    slot->duty = 0U;
                    slot->owner[0] = '\0';
                }
            }
        }
    }
    xSemaphoreGive(pwm_mutex);
    return ret;
}

esp_err_t pwm_port_stop(gpio_num_t pin) { return pwm_stop(pin, "", true); }
esp_err_t pwm_port_disable(gpio_num_t pin, const char *owner)
{
    return owner != NULL && owner[0] != '\0' ? pwm_stop(pin, owner, false) : ESP_ERR_INVALID_ARG;
}
esp_err_t pwm_port_close(gpio_num_t pin, const char *owner)
{
    return owner != NULL && owner[0] != '\0' ? pwm_stop(pin, owner, true) : ESP_ERR_INVALID_ARG;
}

bool pwm_port_get(gpio_num_t pin, pwm_port_status_t *status)
{
    if (status == NULL || pwm_port_ensure_init() != ESP_OK) return false;
    xSemaphoreTake(pwm_mutex, portMAX_DELAY);
    pwm_port_slot_t *slot = pwm_slot_for_pin_locked(pin);
    *status = (pwm_port_status_t){.pin = pin};
    if (slot != NULL) {
        const uint32_t ticks = 1U << pwm_resolution_bits;
        *status = (pwm_port_status_t){
            .pin = pin, .active = true, .channel = slot->channel,
            .freq_hz = pwm_timer_freq_hz,
            .duty_percent = (uint8_t)((slot->duty * 100U + (ticks - 1U) / 2U) / (ticks - 1U)),
            .enabled = slot->enabled, .reserved = slot->owner[0] != '\0',
            .resolution_bits = pwm_resolution_bits,
            .pulse_width_us = (uint32_t)(((uint64_t)slot->duty * 1000000U +
                (uint64_t)pwm_timer_freq_hz * ticks / 2U) / ((uint64_t)pwm_timer_freq_hz * ticks)),
        };
    }
    xSemaphoreGive(pwm_mutex);
    return true;
}
