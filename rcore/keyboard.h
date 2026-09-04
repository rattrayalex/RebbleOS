/* keyboard.h
 * External keyboard input (Bluetooth HID keyboards)
 * RebbleOS
 *
 * A platform keyboard host driver (for example, the nRF52 BLE HID host)
 * delivers HID boot-protocol keyboard reports and link state changes to
 * this module.  The module maps keys onto the four Pebble buttons, so that
 * every existing app works unchanged, and publishes raw key events through
 * keyboard_service for system apps that want them.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Link state of the external keyboard, as reported through keyboard_service. */
typedef enum KeyboardLinkState {
    KeyboardLinkDisconnected = 0,
    KeyboardLinkScanning,
    KeyboardLinkConnecting,
    KeyboardLinkConnected,
} KeyboardLinkState;

/* HID boot keyboard modifier bits (report byte 0). */
#define KEYBOARD_MOD_LEFT_CTRL   0x01
#define KEYBOARD_MOD_LEFT_SHIFT  0x02
#define KEYBOARD_MOD_LEFT_ALT    0x04
#define KEYBOARD_MOD_LEFT_GUI    0x08
#define KEYBOARD_MOD_RIGHT_CTRL  0x10
#define KEYBOARD_MOD_RIGHT_SHIFT 0x20
#define KEYBOARD_MOD_RIGHT_ALT   0x40
#define KEYBOARD_MOD_RIGHT_GUI   0x80
#define KEYBOARD_MOD_SHIFT       (KEYBOARD_MOD_LEFT_SHIFT | KEYBOARD_MOD_RIGHT_SHIFT)

/* HID keyboard/keypad usage IDs (USB HID Usage Tables, page 0x07) used here. */
#define KEYBOARD_USAGE_NONE       0x00
#define KEYBOARD_USAGE_ROLLOVER   0x01
#define KEYBOARD_USAGE_A          0x04
#define KEYBOARD_USAGE_1          0x1E
#define KEYBOARD_USAGE_ENTER      0x28
#define KEYBOARD_USAGE_ESCAPE     0x29
#define KEYBOARD_USAGE_BACKSPACE  0x2A
#define KEYBOARD_USAGE_TAB        0x2B
#define KEYBOARD_USAGE_SPACE      0x2C
#define KEYBOARD_USAGE_RIGHT      0x4F
#define KEYBOARD_USAGE_LEFT       0x50
#define KEYBOARD_USAGE_DOWN       0x51
#define KEYBOARD_USAGE_UP         0x52
#define KEYBOARD_USAGE_KP_ENTER   0x58

/* Boot-protocol keyboard input report: modifiers, reserved, six key usages. */
#define KEYBOARD_BOOT_REPORT_LEN  8
#define KEYBOARD_BOOT_REPORT_KEYS 6

/* Longest keyboard name kept by the core, including the terminator. */
#define KEYBOARD_NAME_MAX 32

/***** Core API (rcore/keyboard.c) *****/

void keyboard_init(void);

/* Driver -> core.  Deliver one HID boot-protocol keyboard input report
 * (byte 0 modifiers, byte 1 reserved, bytes 2..7 usage IDs of the keys
 * currently held).  Thread context only, never from an ISR.  Reports whose
 * length is not 8 bytes are ignored. */
void keyboard_hid_boot_report(const uint8_t *report, size_t len);

/* Driver -> core.  Thread context only.  `name` may be NULL when unknown. */
void keyboard_link_state_changed(KeyboardLinkState state, const char *name);

KeyboardLinkState keyboard_get_link_state(void);
const char *keyboard_get_name(void);  /* "" when unknown */

/* Map a HID usage ID to a Pebble button ID (BUTTON_ID_UP, ...), or -1 when
 * the key is not mapped to a button. */
int keyboard_usage_to_button(uint8_t usage);

/* Printable ASCII for a usage ID under the given modifiers (US layout), or
 * 0 when the key has no character. */
char keyboard_usage_to_char(uint8_t usage, uint8_t modifiers);

/***** Core -> driver requests *****
 * Implemented by the platform keyboard host driver.  rcore/keyboard.c
 * provides weak defaults for platforms without one: the query functions
 * return false, and the requests return -1. */

bool hw_keyboard_is_supported(void);
bool hw_keyboard_has_bond(void);
int hw_keyboard_pair_start(void);   /* scan for a keyboard; connect and bond to the first one found */
int hw_keyboard_connect(void);      /* reconnect to the bonded keyboard, if any */
int hw_keyboard_forget(void);       /* disconnect and forget the bonded keyboard */
