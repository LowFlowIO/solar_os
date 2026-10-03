#pragma once
#include <stdint.h>
#include "esp_err.h"
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_2 2
#define LEDC_TIMER_10_BIT 10
#define LEDC_AUTO_CLK 0
#define LEDC_CHANNEL_4 4
#define LEDC_CHANNEL_5 5
typedef struct {
    int speed_mode, timer_num, duty_resolution, freq_hz, clk_cfg;
} ledc_timer_config_t;
typedef struct {
    int gpio_num, speed_mode, channel, timer_sel;
    uint32_t duty;
} ledc_channel_config_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty);
esp_err_t ledc_update_duty(int mode, int channel);
esp_err_t ledc_stop(int mode, int channel, uint32_t idle);
