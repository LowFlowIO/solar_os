#include "solar_os_inputronic_keyboard_profile.h"
#include <string.h>

/* Matrix wiring and printed legends: Soldered Inputronic KEYBOARD, SKU 333360.
 * Hardware protocol references (no upstream driver code is incorporated):
 * https://github.com/SolderedElectronics/SOLDERED-Inputronic-KEYBOARD-Arduino-Library
 * revision 1b77c3c6b19ccd12b82aac5d4a957e7e53ffd401, Inputronic-Keymap.h
 * and Inputronic-Shiftmap.h; TI TCA8418 datasheet SCPS215G.
 * Entries are canonical USB HID usages, indexed by the FIFO's 1-based key ID.
 * FN1..FN6 have no standard meaning and retain their physical identity only.
 */
static const uint8_t usages[SOLAR_OS_MATRIX_KEY_COUNT + 1U] = {
    [7] = 0x52, [8] = 0x50, [9] = 0x51, [10] = 0x4f,
    [14] = 0x2a, [15] = 0x28, [16] = 0x4c,
    [21] = 0x3a, [22] = 0x3b, [23] = 0x3c, [24] = 0x3d, [25] = 0x3e,
    [26] = 0x3f, [27] = 0x40, [28] = 0x41, [29] = 0x42, [30] = 0x43,
    [31] = 0x27, [32] = 0x1e, [33] = 0x1f, [34] = 0x20, [35] = 0x21,
    [36] = 0x22, [37] = 0x23, [38] = 0x24, [39] = 0x25, [40] = 0x26,
    [41] = 0x13, [42] = 0x14, [43] = 0x1a, [44] = 0x08, [45] = 0x15,
    [46] = 0x17, [47] = 0x1c, [48] = 0x18, [49] = 0x0c, [50] = 0x12,
    [51] = 0x33, [52] = 0x04, [53] = 0x16, [54] = 0x07, [55] = 0x09,
    [56] = 0x0a, [57] = 0x0b, [58] = 0x0d, [59] = 0x0e, [60] = 0x0f,
    [62] = 0x1d, [63] = 0x1b, [64] = 0x06, [65] = 0x19, [66] = 0x05,
    [67] = 0x11, [68] = 0x10, [69] = 0x36, [70] = 0x37,
    [72] = 0x29, [73] = 0x2b, [74] = 0x39, [75] = 0xe1,
    [76] = 0xe0, [77] = 0xe2, [80] = 0x2c,
};


void solar_os_inputronic_keyboard_map(solar_os_matrix_keyboard_map_t *map)
{
    solar_os_matrix_keyboard_init_map(map, 8U, 10U);
    for (unsigned id = 1; id <= SOLAR_OS_MATRIX_KEY_COUNT; id++)
        map->keys[0][id].usage = usages[id];
    for (unsigned id = 17; id <= 20; id++) map->keys[0][id].flags = SOLAR_OS_MATRIX_KEY_RAW;
    map->keys[0][78].flags = SOLAR_OS_MATRIX_KEY_RAW;
    map->keys[0][79].flags = SOLAR_OS_MATRIX_KEY_RAW;
    static const char shifted[] = "=!\"#$%&/()";
    for (unsigned id = 31; id <= 40; id++) {
        map->keys[0][id].key = (uint8_t)('0' + id - 31U);
        map->keys[0][id].shifted_key = (uint8_t)shifted[id - 31U];
    }
    map->keys[0][51].key = ';'; map->keys[0][51].shifted_key = ':';
    map->keys[0][69].key = ','; map->keys[0][69].shifted_key = ';';
    map->keys[0][70].key = '.'; map->keys[0][70].shifted_key = ':';
}
