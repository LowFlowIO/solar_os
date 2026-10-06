#include "solar_os_matrix_keyboard.h"

#include <string.h>
#include "solar_os_input.h"

static bool mapped(solar_os_matrix_key_t key)
{
    return key.usage != 0U || key.key != 0U || key.shifted_key != 0U || key.flags != 0U;
}

static bool valid_usage(uint16_t usage)
{
    return usage == 0U || (usage >= 4U && usage <= 0xa4U) ||
           (usage >= 0xe0U && usage <= 0xe7U);
}

esp_err_t solar_os_matrix_keyboard_validate_map(const solar_os_matrix_keyboard_map_t *map)
{
    if (map == NULL || map->rows < 1U || map->rows > 8U ||
        map->cols < 1U || map->cols > 10U) return ESP_ERR_INVALID_ARG;
    unsigned layer_keys = 0;
    for (unsigned layer = 0; layer < 2U; layer++) {
        if (mapped(map->keys[layer][0])) return ESP_ERR_INVALID_ARG;
        for (unsigned id = 1; id <= SOLAR_OS_MATRIX_KEY_COUNT; id++) {
            const solar_os_matrix_key_t key = map->keys[layer][id];
            if (((id - 1U) / 10U >= map->rows || (id - 1U) % 10U >= map->cols) &&
                mapped(key)) return ESP_ERR_INVALID_ARG;
            if (!valid_usage(key.usage) || (key.flags != 0U &&
                key.flags != SOLAR_OS_MATRIX_KEY_RAW &&
                key.flags != SOLAR_OS_MATRIX_KEY_LAYER_TAP &&
                key.flags != SOLAR_OS_MATRIX_KEY_ALT_BLOCK))
                return ESP_ERR_INVALID_ARG;
            if (key.flags == SOLAR_OS_MATRIX_KEY_RAW &&
                (key.usage != 0U || key.key != 0U || key.shifted_key != 0U))
                return ESP_ERR_INVALID_ARG;
            if (key.flags == SOLAR_OS_MATRIX_KEY_LAYER_TAP) {
                if (layer != 0U || key.usage >= 0xe0U || key.usage == 0x39U)
                    return ESP_ERR_INVALID_ARG;
                layer_keys++;
            }
        }
    }
    return layer_keys <= 1U ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_matrix_keyboard_default_map(solar_os_matrix_keyboard_map_t *map,
                                               unsigned rows, unsigned cols)
{
    if (map == NULL || rows < 1U || rows > 8U || cols < 1U || cols > 10U)
        return ESP_ERR_INVALID_ARG;
    /* Reference wiring, not a layout supplied by the controller. HID usages
     * let the normal SolarOS keyboard layout translate printable keys. */
    static const uint8_t reference[8][10] = {
        {0x14,0x1a,0x08,0x15,0x17,0x1c,0x18,0x0c,0x12,0x13},
        {0x04,0x16,0x07,0x09,0x0a,0x0b,0x0d,0x0e,0x0f,0x28},
        {0xe1,0x1d,0x1b,0x06,0x19,0x05,0x11,0x10,0x2a,0xe0},
        {0x2c,0x2b,0x29,0x39,0xe2,0x50,0x51,0x52,0x4f,0x4c},
        {0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27},
        {0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40,0x41,0x42,0x43},
        {0x2d,0x2e,0x2f,0x30,0x31,0x33,0x34,0x35,0x36,0x37},
        {0xe3,0xe4,0xe5,0xe6,0xe7,0x44,0x45,0x49,0x4a,0x4d},
    };
    memset(map, 0, sizeof(*map));
    map->rows = (uint8_t)rows;
    map->cols = (uint8_t)cols;
    for (unsigned row = 0; row < rows; row++)
        for (unsigned col = 0; col < cols; col++)
            map->keys[0][1U + row * 10U + col].usage = reference[row][col];
    return ESP_OK;
}

void solar_os_matrix_keyboard_release(solar_os_matrix_keyboard_state_t *state)
{
    if (state == NULL) return;
    const bool caps = state->caps_lock;
    memset(state, 0, sizeof(*state));
    state->caps_lock = caps;
}

bool solar_os_matrix_keyboard_decode(solar_os_matrix_keyboard_state_t *state,
                                     const solar_os_matrix_keyboard_map_t *map,
                                     uint8_t raw, solar_os_matrix_key_transition_t *transition)
{
    if (state == NULL || map == NULL || transition == NULL) return false;
    memset(transition, 0, sizeof(*transition));
    const unsigned id = raw & 0x7fU;
    const bool pressed = (raw & 0x80U) != 0U;
    /* TCA8418 IDs retain a stride of TEN even for narrower matrices. */
    if (id == 0U || id > SOLAR_OS_MATRIX_KEY_COUNT ||
        (id - 1U) / 10U >= map->rows || (id - 1U) % 10U >= map->cols ||
        state->held[id] == pressed) return false;
    solar_os_matrix_key_t key = state->pressed_keys[id];
    if (pressed) {
        key = map->keys[0][id];
        if (state->layer_key != 0U && key.flags != SOLAR_OS_MATRIX_KEY_LAYER_TAP) {
            if (mapped(map->keys[1][id])) key = map->keys[1][id];
        }
        if (!mapped(key)) return false;
        if (state->layer_key != 0U && key.flags != SOLAR_OS_MATRIX_KEY_LAYER_TAP)
            state->layer_used = true;
        if (key.flags == SOLAR_OS_MATRIX_KEY_ALT_BLOCK &&
            (state->modifiers & SOLAR_OS_INPUT_MOD_ALT) != 0U)
            key = (solar_os_matrix_key_t){.flags = SOLAR_OS_MATRIX_KEY_RAW};
        state->pressed_keys[id] = key;
    }
    state->held[id] = pressed;
    state->modifiers = 0U;
    for (unsigned i = 1; i <= SOLAR_OS_MATRIX_KEY_COUNT; i++) {
        const uint16_t usage = state->pressed_keys[i].usage;
        if (state->held[i] && usage >= 0xe0U && usage <= 0xe7U)
            state->modifiers |= (uint8_t)(1U << (usage - 0xe0U));
    }
    if (key.usage == 0x39U && pressed) state->caps_lock = !state->caps_lock;
    transition->physical_key = (uint8_t)id;
    transition->pressed = pressed;
    transition->modifiers = state->modifiers;
    if (key.flags == SOLAR_OS_MATRIX_KEY_LAYER_TAP) {
        if (pressed) {
            state->layer_key = (uint8_t)id;
            state->layer_used = false;
        } else {
            if (!state->layer_used) {
                transition->tap_usage = key.usage;
                transition->tap_key = key.key;
            }
            state->layer_key = 0U;
            state->layer_used = false;
        }
        return true;
    }
    transition->usage = key.usage;
    if ((state->modifiers & (SOLAR_OS_INPUT_MOD_CTRL | SOLAR_OS_INPUT_MOD_ALT)) == 0U)
        transition->key = (state->modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0U &&
            key.shifted_key != 0U ? key.shifted_key : key.key;
    return true;
}
