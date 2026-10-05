#include "frontlight_pwm.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"

static esp_err_t apply(uint8_t percent)
{
    /* Limit the combined duty to one full channel, shared equally. */
    const uint32_t total = ((uint32_t)percent * 1023U + 50U) / 100U;
    for (int i = 0; i < 2; ++i) {
        const uint32_t duty = i == 0 ? total / 2U : total - total / 2U;
        esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4 + i, duty);
        if (err == ESP_OK)
            err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4 + i);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t frontlight_pwm_init(frontlight_pwm_t *light, int cool_pin, int warm_pin)
{
    if (!light || !GPIO_IS_VALID_OUTPUT_GPIO(cool_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(warm_pin) || cool_pin == warm_pin)
        return ESP_ERR_INVALID_ARG;
    if (light->ready) return ESP_ERR_INVALID_STATE;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_2,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    for (int i = 0; i < 2; ++i) {
        const ledc_channel_config_t channel = {
            .gpio_num = i == 0 ? cool_pin : warm_pin,
            .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_4 + i,
            .timer_sel = LEDC_TIMER_2, .duty = 0,
        };
        err = ledc_channel_config(&channel);
        if (err != ESP_OK) {
            if (i != 0) (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, 0);
            return err;
        }
    }
    *light = (frontlight_pwm_t){.ready = true};
    return ESP_OK;
}

esp_err_t frontlight_pwm_set(frontlight_pwm_t *light, uint8_t percent)
{
    if (!light || percent > 100) return ESP_ERR_INVALID_ARG;
    if (!light->ready) return ESP_ERR_INVALID_STATE;
    if (!light->suspended) {
        const esp_err_t err = apply(percent);
        if (err != ESP_OK) {
            (void)apply(light->percent);
            return err;
        }
    }
    light->percent = percent;
    return ESP_OK;
}

esp_err_t frontlight_pwm_suspend(frontlight_pwm_t *light, bool suspended)
{
    if (!light || !light->ready) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = apply(suspended ? 0 : light->percent);
    if (err == ESP_OK) light->suspended = suspended;
    else (void)apply(light->suspended ? 0 : light->percent);
    return err;
}

void frontlight_pwm_deinit(frontlight_pwm_t *light)
{
    if (!light || !light->ready) return;
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_4, 0);
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, 0);
    memset(light, 0, sizeof(*light));
}
