/* keyboard.c
 * External keyboard input (Bluetooth HID keyboards)
 * RebbleOS
 */

#include "rebbleos.h"
#include "keyboard.h"

/* STUB: the real implementation is being written. */

static KeyboardLinkState _link_state = KeyboardLinkDisconnected;
static char _name[KEYBOARD_NAME_MAX] = "";

void keyboard_init(void) {
}

void keyboard_hid_boot_report(const uint8_t *report, size_t len) {
    (void)report;
    (void)len;
}

void keyboard_link_state_changed(KeyboardLinkState state, const char *name) {
    _link_state = state;
    if (name) {
        strncpy(_name, name, sizeof(_name) - 1);
        _name[sizeof(_name) - 1] = 0;
    }
}

KeyboardLinkState keyboard_get_link_state(void) {
    return _link_state;
}

const char *keyboard_get_name(void) {
    return _name;
}

int keyboard_usage_to_button(uint8_t usage) {
    (void)usage;
    return -1;
}

char keyboard_usage_to_char(uint8_t usage, uint8_t modifiers) {
    (void)usage;
    (void)modifiers;
    return 0;
}

/* Weak defaults for platforms without a keyboard host driver. */
__attribute__((weak)) bool hw_keyboard_is_supported(void) { return false; }
__attribute__((weak)) bool hw_keyboard_has_bond(void) { return false; }
__attribute__((weak)) int hw_keyboard_pair_start(void) { return -1; }
__attribute__((weak)) int hw_keyboard_connect(void) { return -1; }
__attribute__((weak)) int hw_keyboard_forget(void) { return -1; }
