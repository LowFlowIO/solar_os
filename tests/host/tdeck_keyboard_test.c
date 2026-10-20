/* Real input and TUI services, with their existing host peripheral fixtures. */
#define main input_service_tests
#include "input_test.c"
#undef main
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

static tdeck_device_t device;
static uint8_t matrix[TDECK_COLUMNS];

static size_t logical_events(solar_os_input_key_event_t *out)
{
    solar_os_input_key_event_t events[32];
    size_t count = solar_os_input_read_events(events, 32), kept = 0;
    for (size_t i = 0; i < count; i++) if (events[i].key) out[kept++] = events[i];
    return kept;
}

static void expect_event(uint8_t key, solar_os_input_key_action_t action,
                         uint8_t modifiers, uint16_t usage)
{
    solar_os_input_key_event_t events[32];
    assert(logical_events(events) == 1);
    assert(events[0].key == key && events[0].action == action);
    assert(events[0].modifiers == modifiers && events[0].usage == usage);
    assert(events[0].source == device.input_source && events[0].physical_key != 0);
}

static void expect_none(void)
{
    solar_os_input_key_event_t events[32];
    assert(logical_events(events) == 0);
}

static void scan(void) { process_matrix(&device, matrix); }
static void clear_matrix(void) { memset(matrix, 0, sizeof(matrix)); scan(); }

static void test_holds_layers_and_shortcuts(void)
{
    matrix[3] = 1U << 4; scan();
    expect_event('b', SOLAR_OS_INPUT_KEY_PRESS, 0, 0x05);
    scan(); expect_none();
    clear_matrix(); expect_event('b', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0x05);

    matrix[1] = 1U << 6; matrix[3] = 1U << 4; scan();
    expect_event('B', SOLAR_OS_INPUT_KEY_PRESS, SOLAR_OS_INPUT_MOD_LEFT_SHIFT, 0x05);
    matrix[1] = 0; scan(); expect_none();
    clear_matrix(); expect_event('B', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0x05);

    matrix[0] = 1U << 2; matrix[3] = 1U << 4; scan();
    expect_event('!', SOLAR_OS_INPUT_KEY_PRESS, 0, 0);
    matrix[0] = 0; scan(); expect_none();
    clear_matrix(); expect_event('!', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0);

    /* Native Alt+B is consumed under Shift/Sym too, in either release order. */
    for (unsigned layer = 0; layer < 3; layer++) {
        matrix[0] = (1U << 4) | (layer == 2 ? 1U << 2 : 0);
        matrix[1] = layer == 1 ? 1U << 6 : 0;
        matrix[3] = 1U << 4; scan(); expect_none();
        matrix[0] = 0; matrix[1] = 0; scan(); expect_none();
        clear_matrix(); expect_none();
        matrix[0] = 1U << 4; matrix[3] = 1U << 4; scan(); expect_none();
        matrix[3] = 0; scan(); expect_none();
        clear_matrix(); expect_none();
    }
    /* B already down before Alt remains an ordinary held B. */
    matrix[3] = 1U << 4; scan(); expect_event('b', SOLAR_OS_INPUT_KEY_PRESS, 0, 0x05);
    matrix[0] = 1U << 4; scan(); expect_none();
    clear_matrix(); expect_event('b', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0x05);

    matrix[0] = 1U << 4; matrix[2] = 1U << 5; scan();
    expect_event(SOLAR_OS_KEY_ESCAPE, SOLAR_OS_INPUT_KEY_PRESS, 0, 0);
    matrix[0] = 0; scan(); expect_none();
    clear_matrix(); expect_event(SOLAR_OS_KEY_ESCAPE, SOLAR_OS_INPUT_KEY_RELEASE, 0, 0);
    matrix[0] = (1U << 4) | 1U; scan();
    expect_event(SOLAR_OS_KEY_APP_EXIT, SOLAR_OS_INPUT_KEY_PRESS, 0, 0);
    clear_matrix(); expect_event(SOLAR_OS_KEY_APP_EXIT, SOLAR_OS_INPUT_KEY_RELEASE, 0, 0);

    matrix[0] = 1U << 6; scan(); expect_event('\t', SOLAR_OS_INPUT_KEY_PRESS, 0, 0x2b);
    scan(); expect_none();
    clear_matrix(); expect_event('\t', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0x2b);
    matrix[0] = (1U << 6) | (1U << 2); scan(); expect_event('0', SOLAR_OS_INPUT_KEY_PRESS, 0, 0);
    matrix[0] &= (uint8_t)~(1U << 2); scan(); expect_none();
    clear_matrix(); expect_event('0', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0);

    /* Modifier-only events let guests and desktop shortcuts observe release. */
    matrix[0] = 1U << 4; scan();
    solar_os_input_key_event_t event;
    assert(solar_os_input_read_events(&event, 1) == 1);
    assert(event.key == 0 && event.usage == 0xe2 && event.action == SOLAR_OS_INPUT_KEY_PRESS);
    clear_matrix();
    assert(solar_os_input_read_events(&event, 1) == 1);
    assert(event.key == 0 && event.usage == 0xe2 && event.action == SOLAR_OS_INPUT_KEY_RELEASE);
    assert(event.modifiers == 0 && solar_os_input_composition_modifiers() == 0);
}

static void test_fullscreen_and_completion(void)
{
    char chars[8];
    solar_os_tui_t tui = {.terminal = (solar_os_terminal_t *)(uintptr_t)1,
        .screen_active = true, .saved_status_bar_visible = true};
    matrix[3] = 1U << 3; scan();
    assert(solar_os_input_read_chars(chars, sizeof(chars)) == 1);
    assert(solar_os_tui_screen_key(&tui, chars[0]) == SOLAR_OS_TUI_SCREEN_KEY_NONE);
    clear_matrix(); assert(solar_os_input_read_chars(chars, sizeof(chars)) == 0);
    for (unsigned i = 0; i < 2; i++) {
        matrix[0] = 1U << 4; matrix[3] = 1U << 3; scan();
        assert(solar_os_input_read_chars(chars, sizeof(chars)) == 2);
        assert((uint8_t)chars[0] == SOLAR_OS_KEY_ALT_PREFIX && chars[1] == SOLAR_OS_KEY_ENTER);
        assert(solar_os_tui_screen_key(&tui, (uint8_t)chars[0]) == SOLAR_OS_TUI_SCREEN_KEY_CONSUMED);
        assert(solar_os_tui_screen_key(&tui, chars[1]) == SOLAR_OS_TUI_SCREEN_KEY_TOGGLED);
        assert(solar_os_tui_screen_fullscreen(&tui) == (i == 0));
        scan(); assert(solar_os_input_read_chars(chars, sizeof(chars)) == 0);
        matrix[0] = 0; scan(); assert(solar_os_input_read_chars(chars, sizeof(chars)) == 0);
        clear_matrix(); assert(solar_os_input_read_chars(chars, sizeof(chars)) == 0);
    }
    matrix[0] = (1U << 4) | (1U << 6); scan();
    assert(solar_os_input_read_chars(chars, sizeof(chars)) == 2);
    assert((uint8_t)chars[0] == SOLAR_OS_KEY_ALT_PREFIX && chars[1] == '\t');
    clear_matrix(); assert(solar_os_input_read_chars(chars, sizeof(chars)) == 0);
}

static void test_native_capture(void)
{
    int owner = 0;
    assert(solar_os_input_capture_keyboard_with_shortcuts("tdeck", &owner) == ESP_OK);
    const struct { size_t col, row; uint8_t key; uint16_t usage; } controls[] = {
        {1, 5, 'z', 0x1d}, {1, 4, 'x', 0x1b}, {3, 3, SOLAR_OS_KEY_ENTER, 0x28}, {4, 3, '\b', 0x2a},
    };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        matrix[controls[i].col] = 1U << controls[i].row; scan();
        solar_os_input_key_event_t event;
        bool reset;
        assert(solar_os_input_read_captured_key(&owner, &event, &reset) && !reset);
        assert(event.key == controls[i].key && event.usage == controls[i].usage);
        assert(event.physical_key == controls[i].col * TDECK_ROWS + controls[i].row + 1);
        assert(event.action == SOLAR_OS_INPUT_KEY_PRESS);
        scan();
        assert(!solar_os_input_read_captured_key(&owner, &event, &reset));
        clear_matrix();
        assert(solar_os_input_read_captured_key(&owner, &event, &reset) && !reset);
        assert(event.key == controls[i].key && event.action == SOLAR_OS_INPUT_KEY_RELEASE);
    }
    /* Two controls remain held independently until their actual matrix releases. */
    matrix[1] = (1U << 4) | (1U << 5); scan();
    solar_os_input_key_event_t event;
    bool reset;
    assert(solar_os_input_read_captured_key(&owner, &event, &reset) && event.key == 'x');
    assert(solar_os_input_read_captured_key(&owner, &event, &reset) && event.key == 'z');
    solar_os_input_key_event_t held[4];
    assert(solar_os_input_get_pressed(held, 4) == 2);
    matrix[1] = 1U << 5; scan();
    assert(solar_os_input_read_captured_key(&owner, &event, &reset));
    assert(event.key == 'x' && event.action == SOLAR_OS_INPUT_KEY_RELEASE);
    assert(solar_os_input_get_pressed(held, 4) == 1);
    clear_matrix();
    assert(solar_os_input_read_captured_key(&owner, &event, &reset));
    assert(event.key == 'z' && event.action == SOLAR_OS_INPUT_KEY_RELEASE);
    solar_os_input_release_keyboard(&owner);
}

static void test_keyrate(void)
{
    assert(solar_os_input_set_repeat(5, 200) == ESP_OK);
    matrix[0] = 1U << 3; scan(); expect_event('a', SOLAR_OS_INPUT_KEY_PRESS, 0, 0x04);
    now_us += 199000; scan(); expect_none();
    now_us += 1000; expect_event('a', SOLAR_OS_INPUT_KEY_REPEAT, 0, 0x04);
    now_us += 199000; expect_none();
    now_us += 1000; expect_event('a', SOLAR_OS_INPUT_KEY_REPEAT, 0, 0x04);
    clear_matrix(); expect_event('a', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0x04);

    assert(solar_os_input_set_repeat(20, 100) == ESP_OK);
    matrix[0] = 1U << 3; scan(); expect_event('a', SOLAR_OS_INPUT_KEY_PRESS, 0, 0x04);
    now_us += 100000; expect_event('a', SOLAR_OS_INPUT_KEY_REPEAT, 0, 0x04);
    now_us += 49000; expect_none();
    now_us += 1000; expect_event('a', SOLAR_OS_INPUT_KEY_REPEAT, 0, 0x04);
    assert(solar_os_input_set_repeat(0, 100) == ESP_OK);
    now_us += 500000; scan(); expect_none();
    clear_matrix(); expect_event('a', SOLAR_OS_INPUT_KEY_RELEASE, 0, 0x04);
    now_us += 500000; expect_none();
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    assert(solar_os_input_keyboard_source_open("tdeck", true, &device.input_source) == ESP_OK);
    assert(solar_os_input_set_repeat(0, 300) == ESP_OK);
    test_holds_layers_and_shortcuts();
    test_fullscreen_and_completion();
    test_native_capture();
    test_keyrate();
    solar_os_input_source_close(device.input_source);
    puts("T-Deck real holds/releases, native capture, shortcuts and keyrate passed");
    return 0;
}
