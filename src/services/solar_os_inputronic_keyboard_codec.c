#include "solar_os_inputronic_keyboard_codec.h"

#include <string.h>

#include "solar_os_input.h"

/* Matrix wiring and printed legends: Soldered Inputronic KEYBOARD, SKU 333360.
 * Hardware protocol references (no upstream driver code is incorporated):
 * https://github.com/SolderedElectronics/SOLDERED-Inputronic-KEYBOARD-Arduino-Library
 * revision 1b77c3c6b19ccd12b82aac5d4a957e7e53ffd401, Inputronic-Keymap.h
 * and Inputronic-Shiftmap.h; TI TCA8418 datasheet SCPS215G.
 * Entries are canonical USB HID usages, indexed by the FIFO's 1-based key ID.
 * FN1..FN6 have no standard meaning and retain their physical identity only.
 */
static const uint8_t usages[SOLAR_OS_INPUTRONIC_KEY_COUNT + 1U] = {
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

void solar_os_inputronic_keyboard_release(solar_os_inputronic_keyboard_state_t *state)
{
    if (state != NULL) {
        memset(state->held, 0, sizeof(state->held));
        state->modifiers = 0U;
    }
}

bool solar_os_inputronic_keyboard_decode(solar_os_inputronic_keyboard_state_t *state,
                                         uint8_t raw,
                                         solar_os_inputronic_key_transition_t *transition)
{
    if (state == NULL || transition == NULL) return false;
    memset(transition, 0, sizeof(*transition));
    const uint8_t id = raw & 0x7fU;
    const bool pressed = (raw & 0x80U) != 0U;
    if (id == 0U || id > SOLAR_OS_INPUTRONIC_KEY_COUNT ||
        state->held[id] == pressed) return false;

    const uint8_t usage = usages[id];
    const bool fn = (id >= 17U && id <= 20U) || id == 78U || id == 79U;
    if (usage == 0U && !fn) return false;
    state->held[id] = pressed;
    if (id == 74U && pressed) state->caps_lock = !state->caps_lock;
    state->modifiers = (state->held[75] ? SOLAR_OS_INPUT_MOD_LEFT_SHIFT : 0U) |
                       (state->held[76] ? SOLAR_OS_INPUT_MOD_LEFT_CTRL : 0U) |
                       (state->held[77] ? SOLAR_OS_INPUT_MOD_LEFT_ALT : 0U);
    *transition = (solar_os_inputronic_key_transition_t) {
        .physical_key = id, .usage = usage, .modifiers = state->modifiers,
        .pressed = pressed,
    };

    /* Keep the printed symbol layout, rather than a US HID number row.
     * Control/Alt chords are translated by the common input service. */
    if ((state->modifiers & (SOLAR_OS_INPUT_MOD_CTRL | SOLAR_OS_INPUT_MOD_ALT)) == 0U) {
        const bool shift = (state->modifiers & SOLAR_OS_INPUT_MOD_SHIFT) != 0U;
        if (id >= 31U && id <= 40U) {
            static const char shifted[] = "=!\"#$%&/()";
            transition->key = shift ? (uint8_t)shifted[id - 31U] :
                                      (uint8_t)('0' + id - 31U);
        } else if (id == 51U) transition->key = shift ? ':' : ';';
        else if (id == 69U) transition->key = shift ? ';' : ',';
        else if (id == 70U) transition->key = shift ? ':' : '.';
    }
    return true;
}
