#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_MATRIX_KEY_COUNT 80U
#define SOLAR_OS_MATRIX_KEY_RAW 1U
#define SOLAR_OS_MATRIX_KEY_LAYER_TAP 2U
#define SOLAR_OS_MATRIX_KEY_ALT_BLOCK 4U

typedef struct {
    uint16_t usage;
    uint8_t key;
    uint8_t shifted_key;
    uint8_t flags;
} solar_os_matrix_key_t;

typedef struct {
    uint8_t rows, cols;
    solar_os_matrix_key_t keys[2][SOLAR_OS_MATRIX_KEY_COUNT + 1U];
} solar_os_matrix_keyboard_map_t;

typedef struct {
    bool held[SOLAR_OS_MATRIX_KEY_COUNT + 1U];
    solar_os_matrix_key_t pressed_keys[SOLAR_OS_MATRIX_KEY_COUNT + 1U];
    bool caps_lock, layer_used;
    uint8_t modifiers, layer_key;
} solar_os_matrix_keyboard_state_t;

typedef struct {
    uint8_t physical_key;
    uint16_t usage;
    uint8_t key, modifiers;
    bool pressed;
    uint16_t tap_usage;
    uint8_t tap_key;
} solar_os_matrix_key_transition_t;

esp_err_t solar_os_matrix_keyboard_default_map(solar_os_matrix_keyboard_map_t *map,
                                               unsigned rows, unsigned cols);
esp_err_t solar_os_matrix_keyboard_validate_map(const solar_os_matrix_keyboard_map_t *map);
void solar_os_matrix_keyboard_release(solar_os_matrix_keyboard_state_t *state);
bool solar_os_matrix_keyboard_decode(solar_os_matrix_keyboard_state_t *state,
                                     const solar_os_matrix_keyboard_map_t *map,
                                     uint8_t raw, solar_os_matrix_key_transition_t *transition);
