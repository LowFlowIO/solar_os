/* Use the real input service and its host peripheral fixtures. */
#define main input_service_tests
#include "input_test.c"
#undef main

#define SOLAR_OS_PACKAGE_SERVICE_AUDIO 0
static bool session_switch_alt_held, overlay_active;
static uint8_t session_switch_nav_held;
static unsigned guest_events, overlay_closes;
static uint8_t guest_modifiers;
static bool char_input;
static unsigned app_exits;
static unsigned next_sessions, previous_sessions;
static void maybe_exit_suspend_for_ble_keyboard(void) {}
static uint32_t millis_u32(void) { return 0; }
static void solar_os_power_note_activity(uint32_t now) { (void)now; }
static bool input_focus_accepts_key_events(void) { return !char_input; }
static void solar_os_sessions_show_input_focus_overlay(void) { overlay_active = true; }
static void close_session_overlay(void) { overlay_active = false; overlay_closes++; }
static void dispatch_key_to_input_focus(const solar_os_input_key_event_t *event)
{ guest_events++; guest_modifiers = event->modifiers; }
static void process_app_requests(void) {}
static bool solar_os_sessions_cycle_display_focus(void) { return true; }
static bool solar_os_sessions_cycle_display_focus_previous(void) { return true; }
static bool solar_os_sessions_cycle_display_focus_down(void) { return true; }
static bool solar_os_sessions_cycle_display_focus_up(void) { return true; }
static bool solar_os_sessions_cycle_input_focus(void) { next_sessions++; return true; }
static bool solar_os_sessions_cycle_input_focus_previous(void) { previous_sessions++; return true; }
static void dispatch_input_chars(const char *chars, size_t count)
{
    assert(char_input && count == 1 && (uint8_t)chars[0] == SOLAR_OS_KEY_APP_EXIT);
    /* The app resumes its text shell synchronously. Its first presentation must
     * not be suppressed by the outgoing app's persistent focus overlay. */
    assert(!overlay_active && !session_switch_alt_held);
    app_exits++;
}

#include "foreground_focus_dispatch.inc"

static void dispatch_desktop_queue(void)
{
    solar_os_input_key_event_t events[8];
    size_t count = solar_os_input_read_events(events, 8);
    for (size_t i = 0; i < count; ++i) dispatch_input_key(&events[i]);
}

#ifdef TDECK_TRACKBALL_TEST
#include "solar_os_buttons.h"
#include "solar_os_input_composition.h"
#include "solar_os_log.h"

static int trackball_levels[64];
static void (*trackball_handlers[64])(void *);
static void *trackball_args[64];
static gpio_int_type_t trackball_intr[64];
static int fail_trackball_pin = -1;

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->mode == GPIO_MODE_INPUT);
    assert(config->intr_type == GPIO_INTR_DISABLE);
    assert(config->pull_up_en == GPIO_PULLUP_ENABLE);
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) { return trackball_levels[pin]; }
esp_err_t gpio_install_isr_service(int flags)
{ assert(flags == 0); return ESP_ERR_INVALID_STATE; /* Already shared with BOOT. */ }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg)
{
    if (pin == fail_trackball_pin) return ESP_FAIL;
    assert(trackball_handlers[pin] == NULL);
    trackball_handlers[pin] = handler;
    trackball_args[pin] = arg;
    return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{ trackball_handlers[pin] = NULL; return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type)
{ trackball_intr[pin] = type; return ESP_OK; }

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }

static void trackball_edge(gpio_num_t pin)
{
    trackball_levels[pin] ^= 1;
    now_us += 1000;
    assert(trackball_intr[pin] == GPIO_INTR_ANYEDGE);
    assert(trackball_handlers[pin] != NULL);
    trackball_handlers[pin](trackball_args[pin]);
}

static void expect_queued_motion_tap(uint8_t key, uint8_t modifiers)
{
    solar_os_input_key_event_t events[3];
    assert(solar_os_input_read_events(events, 3) == 2);
    assert(events[0].key == key && events[0].modifiers == modifiers);
    assert(events[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(events[1].key == key && events[1].modifiers == 0);
    assert(events[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    dispatch_input_key(&events[0]);
    dispatch_input_key(&events[1]);
    assert(!session_switch_alt_held && !overlay_active && session_switch_nav_held == 0);
}

static void expect_motion_tap(uint8_t key, uint8_t modifiers)
{
    now_us += 80000;
    solar_os_buttons_poll();
    expect_queued_motion_tap(key, modifiers);
}

static void expect_no_trackball_events(void)
{
    solar_os_input_key_event_t event;
    assert(solar_os_input_read_events(&event, 1) == 0);
}

static void expect_trackball_tap(gpio_num_t pin, uint8_t key, uint8_t modifiers)
{
    trackball_edge(pin);
    solar_os_buttons_poll();
    expect_no_trackball_events();
    expect_motion_tap(key, modifiers);
}

static void test_trackball_session_switching(void)
{
    char_input = false;
    now_us = 1000000;
    for (size_t i = 0; i < 64; i++) trackball_levels[i] = 1;
    /* Partial initialization must release handlers and allow a clean retry. */
    fail_trackball_pin = 1;
    assert(solar_os_buttons_init() == ESP_FAIL);
    for (size_t i = 0; i < 64; i++) assert(trackball_handlers[i] == NULL);
    fail_trackball_pin = -1;
    assert(solar_os_buttons_init() == ESP_OK);
    assert(solar_os_buttons_init() == ESP_OK);
    solar_os_input_source_t keyboard;
    assert(solar_os_input_keyboard_source_open("tdeck", true, &keyboard) == ESP_OK);

    expect_trackball_tap(1, SOLAR_OS_KEY_LEFT, 0);
    expect_trackball_tap(2, SOLAR_OS_KEY_RIGHT, 0);
    assert(next_sessions == 0 && previous_sessions == 0);

    /* The I2C keyboard and the GPIO trackball are separate input sources. */
    solar_os_input_composition_set_modifiers(keyboard, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    expect_trackball_tap(1, SOLAR_OS_KEY_LEFT, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(previous_sessions == 1 && next_sessions == 0);
    expect_trackball_tap(2, SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    expect_trackball_tap(2, SOLAR_OS_KEY_RIGHT, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(previous_sessions == 1 && next_sessions == 2);

    /* Board-translated chords remain plain Page Up/Down, without an Alt prefix. */
    expect_trackball_tap(3, SOLAR_OS_KEY_PAGE_UP, 0);
    expect_trackball_tap(15, SOLAR_OS_KEY_PAGE_DOWN, 0);
    solar_os_input_key_event_t event;
    /* A new horizontal stroke works without the old vertical-axis blackout. */
    expect_trackball_tap(1, SOLAR_OS_KEY_LEFT, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(previous_sessions == 2 && next_sessions == 2);

    solar_os_input_composition_set_modifiers(keyboard, 0);
    expect_trackball_tap(2, SOLAR_OS_KEY_RIGHT, 0);
    now_us += 1000000;
    assert(solar_os_input_read_events(&event, 1) == 0); /* No stuck key/repeat. */
    assert(previous_sessions == 2 && next_sessions == 2);
    solar_os_input_source_close(keyboard);

    /* Short edges, including complete pulses between polls, contribute to one
     * vector. Minor horizontal movement cancels; the dominant axis wins. */
    const gpio_num_t pins[] = {3, 3, 1, 3, 2};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) trackball_edge(pins[i]);
    solar_os_buttons_poll();
    expect_no_trackball_events();
    now_us += 79000;
    solar_os_buttons_poll();
    expect_no_trackball_events();
    now_us += 1000;
    solar_os_buttons_poll();
    expect_queued_motion_tap(SOLAR_OS_KEY_UP, 0);

    /* A long continuous stroke still emits nothing until it actually stops. */
    for (unsigned i = 0; i < 10; i++) {
        now_us += 20000;
        trackball_edge(2);
        solar_os_buttons_poll();
        expect_no_trackball_events();
    }
    trackball_edge(3); /* Small upward drift must not turn this into Up. */
    solar_os_buttons_poll();
    expect_motion_tap(SOLAR_OS_KEY_RIGHT, 0);

    /* Reversals cancel as relative movement; a closed movement emits no key. */
    trackball_edge(1);
    trackball_edge(2);
    trackball_edge(3);
    trackball_edge(15);
    solar_os_buttons_poll();
    now_us += 80000;
    solar_os_buttons_poll();
    expect_no_trackball_events();

    /* Equal diagonals prefer the most recently moved axis. */
    trackball_edge(3);
    trackball_edge(2);
    solar_os_buttons_poll();
    expect_motion_tap(SOLAR_OS_KEY_RIGHT, 0);

    /* A delayed poll still separates strokes using edge timestamps. */
    trackball_edge(3);
    now_us += 80000;
    trackball_edge(1);
    now_us += 80000;
    solar_os_buttons_poll();
    solar_os_input_key_event_t strokes[5];
    assert(solar_os_input_read_events(strokes, 5) == 4);
    assert(strokes[0].key == SOLAR_OS_KEY_UP && strokes[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    assert(strokes[2].key == SOLAR_OS_KEY_LEFT && strokes[3].action == SOLAR_OS_INPUT_KEY_RELEASE);

    /* Releasing Alt during the quiet interval must preserve the gesture's
     * shortcut, including the board's Page Up/Down translation. */
    assert(solar_os_input_keyboard_source_open("gesture", true, &keyboard) == ESP_OK);
    solar_os_input_composition_set_modifiers(keyboard, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    trackball_edge(1);
    solar_os_buttons_poll();
    solar_os_input_composition_set_modifiers(keyboard, 0);
    expect_motion_tap(SOLAR_OS_KEY_LEFT, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    solar_os_input_composition_set_modifiers(keyboard, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    trackball_edge(3);
    solar_os_buttons_poll();
    solar_os_input_composition_set_modifiers(keyboard, 0);
    expect_motion_tap(SOLAR_OS_KEY_PAGE_UP, 0);
    /* Also preserve Alt when the keyboard releases it before the first poll. */
    solar_os_input_composition_set_modifiers(keyboard, SOLAR_OS_INPUT_MOD_LEFT_ALT);
    trackball_edge(15);
    solar_os_input_composition_set_modifiers(keyboard, 0);
    expect_motion_tap(SOLAR_OS_KEY_PAGE_DOWN, 0);
    solar_os_input_source_close(keyboard);

    /* A stalled main loop has a bounded backlog; overflow must never leave a
     * held arrow or corrupt the ring, and fresh input must still work. */
    for (size_t i = 0; i < 100; i++) trackball_edge(1);
    solar_os_button_debug_info_t info;
    assert(solar_os_buttons_debug_get(2, &info));
    assert(info.dropped_edges > 0);
    solar_os_buttons_poll();
    expect_no_trackball_events();
    now_us += 50000; /* Dropped tail edges still postpone the end of motion. */
    solar_os_buttons_poll();
    expect_no_trackball_events();
    expect_motion_tap(SOLAR_OS_KEY_LEFT, 0);
    solar_os_buttons_poll();
    expect_no_trackball_events();
    expect_trackball_tap(2, SOLAR_OS_KEY_RIGHT, 0);

    /* Backpressure in the shared input queue must retain the pending motion
     * and retry the complete press/release pair, without a duplicate press. */
    solar_os_input_source_t filler;
    assert(solar_os_input_keyboard_source_open("filler", true, &filler) == ESP_OK);
    unsigned filled = 0;
    while (solar_os_input_write_key_tap(filler, 1, SOLAR_OS_INPUT_USAGE_NONE, 'x', 0) == ESP_OK) {
        filled++;
        assert(filled < 100);
    }
    trackball_edge(3);
    solar_os_buttons_poll();
    now_us += 80000;
    solar_os_buttons_poll();
    unsigned drained = 0;
    while (solar_os_input_read_events(&event, 1) != 0) {
        assert(event.key == 'x');
        drained++;
    }
    assert(drained == filled * 2);
    solar_os_buttons_poll();
    solar_os_input_key_event_t tap[2];
    assert(solar_os_input_read_events(tap, 2) == 2);
    assert(tap[0].key == SOLAR_OS_KEY_UP && tap[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(tap[1].key == SOLAR_OS_KEY_UP && tap[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    solar_os_buttons_poll();
    assert(solar_os_input_read_events(&event, 1) == 0);
    solar_os_input_source_close(filler);

    /* The millisecond counter may wrap while a gesture is settling. */
    now_us = ((int64_t)UINT32_MAX - 40) * 1000;
    trackball_edge(2);
    solar_os_buttons_poll();
    expect_no_trackball_events();
    expect_motion_tap(SOLAR_OS_KEY_RIGHT, 0);
}
#endif

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    solar_os_input_source_t source;
    assert(solar_os_input_keyboard_source_open("app-keyboard", true, &source) == ESP_OK);
    int owner;
    assert(solar_os_input_capture_keyboard_with_shortcuts("app-keyboard", &owner) == ESP_OK);
    assert(solar_os_input_write_key(source, 0xe2, 0xe2, 0,
        SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    dispatch_desktop_queue();
    assert(overlay_active && session_switch_alt_held);
    assert(guest_events == 1 && guest_modifiers == SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(solar_os_input_write_key(source, 0xe2, 0xe2, 0, 0, SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    dispatch_desktop_queue();
    assert(!overlay_active && !session_switch_alt_held && overlay_closes == 1);
    assert(guest_events == 2 && guest_modifiers == 0); /* Guest Alt button is released too. */

    solar_os_input_release_keyboard(&owner);
    /* Switching into the foreground app while Alt is held must not swallow the release. */
    assert(solar_os_input_write_key(source, 0xe2, 0xe2, 0,
        SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    dispatch_desktop_queue();
    assert(overlay_active && session_switch_alt_held);
    assert(solar_os_input_capture_keyboard_with_shortcuts("app-keyboard", &owner) == ESP_OK);
    assert(solar_os_input_write_key(source, 0xe2, 0xe2, 0, 0, SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    dispatch_desktop_queue();
    assert(!overlay_active && overlay_closes == 2 && guest_modifiers == 0);
    solar_os_input_release_keyboard(&owner);
    /* Alt release can already be queued when a resumed foreground app claims it. */
    session_switch_alt_held = overlay_active = true;
    assert(solar_os_input_write_key(source, 0xe2, 0xe2, 0,
        SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
    dispatch_desktop_queue();
    assert(solar_os_input_write_key(source, 0xe2, 0xe2, 0, 0, SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
    assert(solar_os_input_capture_keyboard_with_shortcuts("app-keyboard", &owner) == ESP_OK);
    dispatch_desktop_queue();
    assert(!overlay_active && overlay_closes == 3 && guest_modifiers == 0);
    solar_os_input_release_keyboard(&owner);
    char_input = true;
    for (unsigned alt_first = 0; alt_first < 2; alt_first++) {
        const uint8_t alt = SOLAR_OS_INPUT_MOD_LEFT_ALT;
        const uint8_t ctrl = SOLAR_OS_INPUT_MOD_LEFT_CTRL;
        uint16_t first = alt_first ? 0xe2 : 0xe0, second = alt_first ? 0xe0 : 0xe2;
        assert(solar_os_input_write_key(source, first, first, 0, alt_first ? alt : ctrl,
            SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
        dispatch_desktop_queue();
        assert(solar_os_input_write_key(source, second, second, 0, alt | ctrl,
            SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
        dispatch_desktop_queue();
        assert(overlay_active == (alt_first != 0));
        uint8_t exit_key = solar_os_input_translate_hid_usage(0x4c, alt | ctrl, false);
        assert(exit_key == SOLAR_OS_KEY_APP_EXIT);
        assert(solar_os_input_write_key(source, 0x4c, 0x4c, exit_key, alt | ctrl,
            SOLAR_OS_INPUT_KEY_PRESS) == ESP_OK);
        dispatch_desktop_queue();
        /* The display must already be restored before the user releases Alt. */
        assert(!overlay_active && !session_switch_alt_held);
        assert(solar_os_input_write_key(source, first, first, 0, 0,
            SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
        assert(solar_os_input_write_key(source, second, second, 0, 0,
            SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
        assert(solar_os_input_write_key(source, 0x4c, 0x4c, exit_key, 0,
            SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK);
        dispatch_desktop_queue();
        assert(!overlay_active && !session_switch_alt_held);
    }
    assert(app_exits == 2);
    solar_os_input_source_close(source);
#ifdef TDECK_TRACKBALL_TEST
    test_trackball_session_switching();
#endif
    puts("Foreground capture releases modifiers; app-exit dismisses the Alt switcher before handback");
    return 0;
}
