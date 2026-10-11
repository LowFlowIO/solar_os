#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"

#define PWM_PORT_FREQ_MIN_HZ 1U
#define PWM_PORT_FREQ_MAX_HZ 40000U
#define PWM_PORT_DUTY_MAX_PERCENT 100U

typedef struct {
    int pin;
    bool active;
    ledc_channel_t channel;
    uint32_t freq_hz;
    uint8_t duty_percent;
    bool enabled;
    bool reserved;
    uint8_t resolution_bits;
    uint32_t pulse_width_us;
} pwm_port_status_t;

esp_err_t pwm_port_init(void);
esp_err_t pwm_port_set(gpio_num_t pin, uint32_t freq_hz, uint8_t duty_percent);
esp_err_t pwm_port_set_pulse(gpio_num_t pin, uint32_t freq_hz, uint32_t pulse_us);
/* Reserved outputs start low and hold their channel/timer until closed. */
esp_err_t pwm_port_open(gpio_num_t pin, uint32_t freq_hz, const char *owner);
esp_err_t pwm_port_write_pulse(gpio_num_t pin, const char *owner, uint32_t pulse_us);
esp_err_t pwm_port_disable(gpio_num_t pin, const char *owner);
esp_err_t pwm_port_close(gpio_num_t pin, const char *owner);
esp_err_t pwm_port_stop(gpio_num_t pin);
bool pwm_port_get(gpio_num_t pin, pwm_port_status_t *status);
