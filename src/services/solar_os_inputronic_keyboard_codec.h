#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SOLAR_OS_INPUTRONIC_KEY_COUNT 80U

typedef struct {
    bool held[SOLAR_OS_INPUTRONIC_KEY_COUNT + 1U];
    bool caps_lock;
    uint8_t modifiers;
} solar_os_inputronic_keyboard_state_t;

typedef struct {
    uint8_t physical_key;
    uint16_t usage;
    /* Nonzero for Inputronic's printed number/punctuation symbols. */
    uint8_t key;
    uint8_t modifiers;
    bool pressed;
} solar_os_inputronic_key_transition_t;

/* Clear held keys while retaining the Caps Lock toggle. */
void solar_os_inputronic_keyboard_release(solar_os_inputronic_keyboard_state_t *state);
bool solar_os_inputronic_keyboard_decode(solar_os_inputronic_keyboard_state_t *state,
                                         uint8_t raw,
                                         solar_os_inputronic_key_transition_t *transition);
