#include "solar_os_matrix_keyboard.h"
#include <string.h>

esp_err_t solar_os_matrix_keyboard_init_map(solar_os_matrix_keyboard_map_t *map,
                                       unsigned rows, unsigned cols)
{
    if (map == NULL || rows < 1U || rows > 8U || cols < 1U || cols > 10U)
        return ESP_ERR_INVALID_ARG;
    memset(map, 0, sizeof(*map));
    map->rows = (uint8_t)rows; map->cols = (uint8_t)cols;
    map->first = 1U; map->stride = 10U;
    map->slot_count = (uint16_t)((rows - 1U) * 10U + cols);
    for (unsigned row = 0; row < rows; row++)
        for (unsigned col = 0; col < cols; col++) {
            const unsigned id = 1U + row * 10U + col;
            map->physical[id] = (uint16_t)id;
        }
    return ESP_OK;
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
    solar_os_matrix_keyboard_init_map(map, rows, cols);
    for (unsigned row = 0; row < rows; row++)
        for (unsigned col = 0; col < cols; col++)
            map->keys[0][1U + row * 10U + col].usage = reference[row][col];
    return ESP_OK;
}

esp_err_t solar_os_matrix_keyboard_validate_map(const solar_os_matrix_keyboard_map_t *map)
{
    return solar_os_input_keymap_validate(map);
}
void solar_os_matrix_keyboard_release(solar_os_matrix_keyboard_state_t *state)
{
    solar_os_input_keymap_release(state);
}
bool solar_os_matrix_keyboard_decode(solar_os_matrix_keyboard_state_t *state,
    const solar_os_matrix_keyboard_map_t *map, uint8_t raw,
    solar_os_matrix_key_transition_t *transition)
{
    return solar_os_input_keymap_decode(state, map, raw & 0x7fU,
                                        (raw & 0x80U) != 0U, transition);
}
