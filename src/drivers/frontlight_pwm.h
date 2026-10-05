#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Two active-high channels, fixed neutral mix. Uses LEDC timer 2/channels 4/5;
 * pwm_port owns timer 0/channels 0..3. The caller owns both GPIO resources. */
typedef struct {
    bool ready, suspended;
    uint8_t percent;
} frontlight_pwm_t;

esp_err_t frontlight_pwm_init(frontlight_pwm_t *light, int cool_pin, int warm_pin);
esp_err_t frontlight_pwm_set(frontlight_pwm_t *light, uint8_t percent);
esp_err_t frontlight_pwm_suspend(frontlight_pwm_t *light, bool suspended);
void frontlight_pwm_deinit(frontlight_pwm_t *light);
