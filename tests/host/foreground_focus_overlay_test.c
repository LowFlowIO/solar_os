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
static bool solar_os_sessions_cycle_input_focus(void) { return true; }
static bool solar_os_sessions_cycle_input_focus_previous(void) { return true; }
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
    puts("Foreground capture releases modifiers; app-exit dismisses the Alt switcher before handback");
    return 0;
}
