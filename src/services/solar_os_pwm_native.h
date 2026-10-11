#pragma once

#include "solar_os_pwm.h"

/* Internal initialization path, separate from expansion provider registration. */
esp_err_t solar_os_pwm_native_register(void);
esp_err_t solar_os_pwm_register_native(const solar_os_pwm_registration_t *registration);
