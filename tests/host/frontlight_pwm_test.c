#include <assert.h>
#include <stdio.h>
#include "../../src/drivers/frontlight_pwm.c"

static uint32_t duties[2];
static int channel_configs, stops, fail_channel;
esp_err_t ledc_timer_config(const ledc_timer_config_t *c)
{
    assert(c->speed_mode == LEDC_LOW_SPEED_MODE && c->timer_num == LEDC_TIMER_2);
    assert(c->duty_resolution == 10 && c->freq_hz == 25000);
    return ESP_OK;
}
esp_err_t ledc_channel_config(const ledc_channel_config_t *c)
{
    assert(c->channel == 4 + channel_configs && c->timer_sel == 2 && !c->duty);
    assert(c->gpio_num == 8 + channel_configs);
    channel_configs++;
    return ESP_OK;
}
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty)
{
    assert(mode == 0 && (channel == 4 || channel == 5));
    if (channel == fail_channel) { fail_channel = 0; return ESP_FAIL; }
    duties[channel - 4] = duty;
    return ESP_OK;
}
esp_err_t ledc_update_duty(int mode, int channel)
{ assert(mode == 0 && (channel == 4 || channel == 5)); return ESP_OK; }
esp_err_t ledc_stop(int mode, int channel, uint32_t idle)
{ assert(mode == 0 && !idle); duties[channel - 4] = 0; stops++; return ESP_OK; }

int main(void)
{
    frontlight_pwm_t light = {0};
    assert(frontlight_pwm_init(&light, 8, 8) == ESP_ERR_INVALID_ARG);
    assert(frontlight_pwm_init(&light, 8, 9) == ESP_OK);
    assert(frontlight_pwm_set(&light, 100) == ESP_OK);
    assert(duties[0] == 511 && duties[1] == 512);
    assert(frontlight_pwm_set(&light, 50) == ESP_OK);
    assert(duties[0] == 256 && duties[1] == 256);
    assert(frontlight_pwm_set(&light, 101) == ESP_ERR_INVALID_ARG);
    fail_channel = 5;
    assert(frontlight_pwm_set(&light, 80) == ESP_FAIL);
    assert(light.percent == 50 && duties[0] == 256 && duties[1] == 256);
    assert(frontlight_pwm_suspend(&light, true) == ESP_OK);
    assert(!duties[0] && !duties[1] && light.percent == 50);
    assert(frontlight_pwm_set(&light, 20) == ESP_OK);
    assert(!duties[0] && !duties[1]);
    assert(frontlight_pwm_suspend(&light, false) == ESP_OK);
    assert(duties[0] + duties[1] == 205 && light.percent == 20);
    assert(frontlight_pwm_set(&light, 0) == ESP_OK);
    assert(!duties[0] && !duties[1]);
    frontlight_pwm_deinit(&light);
    assert(stops == 2 && !light.ready);
    assert(frontlight_pwm_set(&light, 50) == ESP_ERR_INVALID_STATE);
    puts("frontlight PWM tests passed");
}
