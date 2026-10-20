/* Reuse the real input service's host peripheral fixtures. */
#define main input_service_tests
#include "input_test.c"
#undef main
/* Reuse terminal/TUI fixtures to exercise the actual full-screen consumer. */
#define main tui_widget_tests
#include "tui_widgets_test.c"
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
    assert(events[1].modifiers == 0U);
    if (key == SOLAR_OS_KEY_ESCAPE || key == SOLAR_OS_KEY_APP_EXIT) {
        assert(events[0].modifiers == 0U); /* Board-translated chords are plain keys. */
    }
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

    /* Full-screen text menus need the Alt prefix, not an ordinary Enter. */
    uint8_t enter[TDECK_COLUMNS] = {0, 0, 0, 1U << 3, 0};
    solar_os_tui_t tui = {
        .terminal = (solar_os_terminal_t *)(uintptr_t)1U,
        .screen_active = true,
        .saved_status_bar_visible = true,
    };
    char chars[4];
    process_matrix(&device, enter);
    assert(solar_os_input_read_chars(chars, sizeof(chars)) == 1);
    assert((uint8_t)chars[0] == SOLAR_OS_KEY_ENTER);
    assert(solar_os_tui_screen_key(&tui, (uint8_t)chars[0]) == SOLAR_OS_TUI_SCREEN_KEY_NONE);
    sample(&device, false, false, false, false, 0);
    enter[0] = 1U << 4; /* Alt and Enter first appear in the same scan. */
    process_matrix(&device, enter);
    assert(solar_os_input_read_chars(chars, sizeof(chars)) == 2);
    assert((uint8_t)chars[0] == SOLAR_OS_KEY_ALT_PREFIX);
    assert((uint8_t)chars[1] == SOLAR_OS_KEY_ENTER);
    assert(solar_os_tui_screen_key(&tui, (uint8_t)chars[0]) == SOLAR_OS_TUI_SCREEN_KEY_CONSUMED);
    assert(solar_os_tui_screen_key(&tui, (uint8_t)chars[1]) == SOLAR_OS_TUI_SCREEN_KEY_TOGGLED);
    assert(solar_os_tui_screen_fullscreen(&tui) && !test_status_bar_visible);
    process_matrix(&device, enter);
    expect_events(0); /* Holding the chord must not keep toggling full screen. */
    enter[0] = 0; /* Releasing Alt before Enter must not emit plain Enter. */
    process_matrix(&device, enter);
    expect_events(0);
    sample(&device, false, false, false, false, 0);

    enter[0] = 1U << 4;
    process_matrix(&device, enter);
    assert(solar_os_input_read_chars(chars, sizeof(chars)) == 2);
    assert(solar_os_tui_screen_key(&tui, (uint8_t)chars[0]) == SOLAR_OS_TUI_SCREEN_KEY_CONSUMED);
    assert(solar_os_tui_screen_key(&tui, (uint8_t)chars[1]) == SOLAR_OS_TUI_SCREEN_KEY_TOGGLED);
    assert(!solar_os_tui_screen_fullscreen(&tui) && test_status_bar_visible);
    sample(&device, false, false, false, false, 0);

    /* Event-based apps receive the same chord, with a real physical key and a
     * modifier-free release so the session overlay cannot become stuck. */
    sample(&device, true, false, false, false, 0);
    enter[0] = 1U << 4;
    process_matrix(&device, enter);
    solar_os_input_key_event_t chord[3];
    assert(solar_os_input_read_events(chord, 3) == 2);
    assert(chord[0].key == SOLAR_OS_KEY_ENTER);
    assert(chord[0].modifiers == SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(chord[0].physical_key == 3U * TDECK_ROWS + 3U + 1U);
    assert(chord[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(chord[1].modifiers == 0U && chord[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    enter[3] = 0; /* Releasing Enter before Alt also emits nothing extra. */
    process_matrix(&device, enter);
    expect_events(0);
    sample(&device, false, false, false, false, 0);

    /* The microphone Tab participates in universal Alt+Tab session switching. */
    uint8_t alt_tab[TDECK_COLUMNS] = {(1U << 4) | (1U << 6), 0, 0, 0, 0};
    process_matrix(&device, alt_tab);
    assert(solar_os_input_read_chars(chars, sizeof(chars)) == 2);
    assert((uint8_t)chars[0] == SOLAR_OS_KEY_ALT_PREFIX && chars[1] == '\t');
    sample(&device, false, false, false, false, 0);

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
    /* The microphone key supplies Tab; two separate taps reach completion. */
    uint8_t microphone[TDECK_COLUMNS] = {1U << 6, 0, 0, 0, 0};
    process_matrix(&device, microphone);
    expect_events('\t');
    process_matrix(&device, microphone);
    expect_events(0); /* Holding does not invent another completion tap. */
    sample(&device, false, false, false, false, 0);
    process_matrix(&device, microphone);
    expect_events('\t');
    sample(&device, false, false, false, false, 0);
    microphone[0] |= 1U << 2; /* Sym+microphone remains the digit zero. */
    process_matrix(&device, microphone);
    expect_events('0');
    microphone[0] &= (uint8_t)~(1U << 2);
    process_matrix(&device, microphone);
    expect_events(0); /* Releasing Sym while held must not leak Tab. */
    sample(&device, false, false, false, false, 0);
    process_matrix(&device, microphone);
    expect_events('\t');
    sample(&device, false, false, false, false, 0);
    solar_os_input_source_close(device.input_source);
    puts("T-Deck preserves Alt+Enter and consumes Alt+B while retaining keyboard mappings");
    return 0;
}
