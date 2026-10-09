#pragma once
typedef int gpio_num_t;
#include "esp_err.h"
esp_err_t gpio_reset_pin(gpio_num_t pin);
