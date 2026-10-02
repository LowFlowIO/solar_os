#pragma once
#include "../../gpio_controller_stubs/driver/gpio.h"
#define GPIO_NUM_MAX 49
#define GPIO_NUM_NC -1
esp_err_t gpio_reset_pin(gpio_num_t pin);
