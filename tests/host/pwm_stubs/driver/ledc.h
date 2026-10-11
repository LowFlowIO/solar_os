#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int ledc_channel_t;
typedef int ledc_timer_bit_t;
enum { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3 };
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_0 0
#define LEDC_AUTO_CLK 0
#define LEDC_INTR_DISABLE 0
#define LEDC_SLEEP_MODE_NO_ALIVE_NO_PD 0
typedef struct {
    int speed_mode, timer_num, duty_resolution;
    uint32_t freq_hz;
    int clk_cfg;
} ledc_timer_config_t;
typedef struct {
    int gpio_num, speed_mode, channel, intr_type, timer_sel;
    uint32_t duty, hpoint;
    int sleep_mode;
} ledc_channel_config_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t *config);
esp_err_t ledc_channel_config(const ledc_channel_config_t *config);
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty);
esp_err_t ledc_update_duty(int mode, int channel);
esp_err_t ledc_stop(int mode, int channel, uint32_t idle);
