#pragma once
#include "solar_os_matrix_keyboard.h"
/* Sparse replacements over a built-in map. The output is untouched on error. */
esp_err_t solar_os_matrix_keyboard_parse_map(const char *json,
    const solar_os_matrix_keyboard_map_t *base, solar_os_matrix_keyboard_map_t *out);
