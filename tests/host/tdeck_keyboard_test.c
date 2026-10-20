/* Reuse the real input service's host peripheral fixtures. */
#define main input_service_tests
#include "input_test.c"
#undef main
#include <solar_os_board.h>
#include "solar_os_input_composition.h"

#define TDECK_COLUMNS 5U
#define TDECK_ROWS 7U
typedef struct { uint8_t normal, symbol; } tdeck_key_t;
static const tdeck_key_t keymap[TDECK_COLUMNS][TDECK_ROWS] = SOLAR_OS_BOARD_TDECK_KEYMAP;
typedef struct {
    uint8_t previous[TDECK_COLUMNS];
    solar_os_input_source_t input_source;
} tdeck_device_t;
#include "tdeck_matrix.inc"

static void expect_events(uint8_t key)
{
    solar_os_input_key_event_t events[3];
    const size_t count = solar_os_input_read_events(events, 3);
    if (key == 0U) { assert(count == 0); return; }
    assert(count == 2);
    assert(events[0].key == key && events[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(events[1].key == key && events[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
}

static void sample(tdeck_device_t *device, bool alt, bool shift, bool sym, bool b, uint8_t key)
{
    uint8_t state[TDECK_COLUMNS] = {
        (alt ? 1U << 4 : 0U) | (sym ? 1U << 2 : 0U),
        shift ? 1U << 6 : 0U, 0U, b ? 1U << 4 : 0U, 0U,
    };
    process_matrix(device, state);
    expect_events(key);
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    tdeck_device_t device = {0};
    assert(solar_os_input_keyboard_source_open("tdeck", true, &device.input_source) == ESP_OK);

    sample(&device, false, false, false, true, 'b');
    sample(&device, false, false, false, false, 0);
    sample(&device, false, true, false, true, 'B');
    sample(&device, false, false, false, false, 0);
    sample(&device, false, false, true, true, '!');
    sample(&device, false, false, false, false, 0);

    sample(&device, true, false, false, false, 0); /* Alt before B. */
    sample(&device, true, false, false, true, 0);
    assert(solar_os_input_composition_modifiers() == SOLAR_OS_INPUT_MOD_LEFT_ALT);
    sample(&device, true, false, false, true, 0); /* Holding emits nothing. */
    sample(&device, false, false, false, true, 0); /* Release Alt first. */
    sample(&device, false, false, false, false, 0);
    assert(solar_os_input_composition_modifiers() == 0U);
    sample(&device, false, false, false, true, 'b');
    sample(&device, false, false, false, false, 0);

    sample(&device, true, false, false, true, 0); /* Same scan, then release B first. */
    sample(&device, true, false, false, false, 0);
    sample(&device, true, false, false, true, 0); /* A second toggle is consumed too. */
    sample(&device, false, false, false, false, 0);
    sample(&device, true, true, false, true, 0); /* Firmware ignores Shift/Sym for this shortcut. */
    sample(&device, false, false, false, false, 0);
    sample(&device, true, false, true, true, 0);
    sample(&device, false, false, false, false, 0);

    /* B already down before Alt is ordinary text, not a new firmware toggle. */
    sample(&device, false, false, false, true, 'b');
    sample(&device, true, false, false, true, 0);
    sample(&device, false, false, false, false, 0);

    uint8_t state[TDECK_COLUMNS] = {1U << 4, 0, 1U << 5, 0, 0};
    process_matrix(&device, state); /* Alt+C remains Escape. */
    expect_events(SOLAR_OS_KEY_ESCAPE);
    sample(&device, false, false, false, false, 0);
    state[0] = (1U << 4) | 1U;
    state[2] = 0;
    process_matrix(&device, state); /* Alt+Q remains app exit. */
    expect_events(SOLAR_OS_KEY_APP_EXIT);
    sample(&device, false, false, false, false, 0);
    state[0] = (1U << 4) | (1U << 3);
    state[3] = 1U << 4;
    process_matrix(&device, state); /* Consume B without swallowing another pressed key. */
    expect_events('a');
    sample(&device, false, false, false, false, 0);
    solar_os_input_source_close(device.input_source);
    puts("T-Deck consumes native Alt+B without changing ordinary keys or other shortcuts");
    return 0;
}
