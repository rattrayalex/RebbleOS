/* keyboard.c
 * External keyboard input (Bluetooth HID keyboards)
 * RebbleOS
 *
 * The platform keyboard host driver calls keyboard_hid_boot_report() and
 * keyboard_link_state_changed() from thread context (the service worker
 * thread).  Keys that map to a Pebble button drive rcore/buttons.c through
 * button_inject_state(); every key change also goes out as a KeyboardEvent
 * through keyboard_service.  The pure mapping and report logic lives in
 * keyboard_map.c so that tests/host can build it without the firmware.
 */

#include "rebbleos.h"
#include "buttons.h"
#include "keyboard.h"
#include "keyboard_map.h"
#include "keyboard_service.h"

/* keyboard_map.h repeats the button numbering so that it builds without
 * firmware headers; make sure it agrees with ButtonId. */
_Static_assert(KEYBOARD_MAP_BUTTON_BACK == BUTTON_ID_BACK, "keyboard_map button numbering differs from ButtonId");
_Static_assert(KEYBOARD_MAP_BUTTON_UP == BUTTON_ID_UP, "keyboard_map button numbering differs from ButtonId");
_Static_assert(KEYBOARD_MAP_BUTTON_SELECT == BUTTON_ID_SELECT, "keyboard_map button numbering differs from ButtonId");
_Static_assert(KEYBOARD_MAP_BUTTON_DOWN == BUTTON_ID_DOWN, "keyboard_map button numbering differs from ButtonId");
_Static_assert(KEYBOARD_MAP_NUM_BUTTONS == NUM_BUTTONS, "keyboard_map button count differs from NUM_BUTTONS");
_Static_assert(KEYBOARD_MAP_REPORT_LEN == KEYBOARD_BOOT_REPORT_LEN, "boot report length differs between keyboard.h and keyboard_map.h");

static KeyboardLinkState _link_state = KeyboardLinkDisconnected;
static char _name[KEYBOARD_NAME_MAX] = "";

/* The last accepted report; the next one is diffed against it. */
static keyboard_report_t _prev_report;

/* How many held keys map to each button (Enter and Right arrow both hold
 * Select).  The button releases when its count drops to zero. */
static uint8_t _button_holds[NUM_BUTTONS];

void keyboard_init(void)
{
    _link_state = KeyboardLinkDisconnected;
    _name[0] = 0;
    memset(&_prev_report, 0, sizeof(_prev_report));
    memset(_button_holds, 0, sizeof(_button_holds));
}

static void _hold_button(int button)
{
    if (button < 0 || button >= NUM_BUTTONS)
        return;
    if (_button_holds[button]++ == 0)
        button_inject_state((ButtonId)button, true);
}

static void _release_button(int button)
{
    if (button < 0 || button >= NUM_BUTTONS || _button_holds[button] == 0)
        return;
    if (--_button_holds[button] == 0)
        button_inject_state((ButtonId)button, false);
}

static void _release_all_buttons(void)
{
    for (int button = 0; button < NUM_BUTTONS; button++) {
        if (_button_holds[button] == 0)
            continue;
        _button_holds[button] = 0;
        button_inject_state((ButtonId)button, false);
    }
}

static void _post_key_event(KeyboardEventType type, uint8_t usage, uint8_t modifiers)
{
    KeyboardEvent event = {
        .type = type,
        .usage = usage,
        .modifiers = modifiers,
        .character = keyboard_map_usage_to_char(usage, modifiers),
        .link_state = _link_state,
    };
    keyboard_service_post(&event);
}

void keyboard_hid_boot_report(const uint8_t *report, size_t len)
{
    keyboard_report_t cur;
    uint8_t pressed[KEYBOARD_MAP_REPORT_KEYS];
    uint8_t released[KEYBOARD_MAP_REPORT_KEYS];
    size_t num_pressed, num_released;

    if (keyboard_map_report_parse(report, len, &cur) < 0)
        return;

    num_pressed = keyboard_map_report_diff(&_prev_report, &cur, pressed, released, &num_released);
    _prev_report = cur;

    /* Releases first, so that a key rolling onto another key that holds the
     * same button lets go of it before the new key takes it. */
    for (size_t i = 0; i < num_released; i++) {
        _release_button(keyboard_map_usage_to_button(released[i]));
        _post_key_event(KeyboardEventKeyUp, released[i], cur.modifiers);
    }
    for (size_t i = 0; i < num_pressed; i++) {
        _hold_button(keyboard_map_usage_to_button(pressed[i]));
        _post_key_event(KeyboardEventKeyDown, pressed[i], cur.modifiers);
    }
}

void keyboard_link_state_changed(KeyboardLinkState state, const char *name)
{
    _link_state = state;
    if (name) {
        strncpy(_name, name, sizeof(_name) - 1);
        _name[sizeof(_name) - 1] = 0;
    }

    /* Keys held when the link drops never get a release report. */
    if (state != KeyboardLinkConnected) {
        _release_all_buttons();
        memset(&_prev_report, 0, sizeof(_prev_report));
    }

    KeyboardEvent event = {
        .type = KeyboardEventLinkState,
        .link_state = state,
    };
    keyboard_service_post(&event);
}

KeyboardLinkState keyboard_get_link_state(void)
{
    return _link_state;
}

const char *keyboard_get_name(void)
{
    return _name;
}

int keyboard_usage_to_button(uint8_t usage)
{
    return keyboard_map_usage_to_button(usage);
}

char keyboard_usage_to_char(uint8_t usage, uint8_t modifiers)
{
    return keyboard_map_usage_to_char(usage, modifiers);
}

/* Weak defaults for platforms without a keyboard host driver. */
__attribute__((weak)) bool hw_keyboard_is_supported(void) { return false; }
__attribute__((weak)) bool hw_keyboard_has_bond(void) { return false; }
__attribute__((weak)) int hw_keyboard_pair_start(void) { return -1; }
__attribute__((weak)) int hw_keyboard_connect(void) { return -1; }
__attribute__((weak)) int hw_keyboard_forget(void) { return -1; }
