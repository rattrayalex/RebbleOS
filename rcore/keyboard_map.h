/* keyboard_map.h
 * Pure HID keyboard logic for the external keyboard: usage mapping and
 * boot-report parsing.  No firmware includes, so tests/host builds this
 * pair with the native compiler.
 * RebbleOS
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Pebble button numbers, the same numbering as HW_BUTTON_BACK = 0, UP, SELECT,
 * DOWN in hw/drivers/nrf52_buttons/nrf52_buttons.h and stm32_buttons.h.
 * rcore/keyboard.c checks them against ButtonId at compile time. */
#define KEYBOARD_MAP_BUTTON_BACK   0
#define KEYBOARD_MAP_BUTTON_UP     1
#define KEYBOARD_MAP_BUTTON_SELECT 2
#define KEYBOARD_MAP_BUTTON_DOWN   3
#define KEYBOARD_MAP_NUM_BUTTONS   4

/* HID boot-protocol keyboard input report: modifiers, reserved, six keys. */
#define KEYBOARD_MAP_REPORT_LEN  8
#define KEYBOARD_MAP_REPORT_KEYS 6

typedef struct {
    uint8_t modifiers;
    uint8_t keys[KEYBOARD_MAP_REPORT_KEYS];
} keyboard_report_t;

/* Pebble button for a HID usage ID: Up -> UP, Down -> DOWN, Enter, keypad
 * Enter and Right -> SELECT, Escape, Left and Backspace -> BACK.  Returns -1
 * for every other usage. */
int keyboard_map_usage_to_button(uint8_t usage);

/* Printable ASCII for a usage ID under the given modifier bits (report byte
 * 0), US layout: letters (shift gives upper case), digits and their shifted
 * symbols, space, punctuation with shift variants, Enter and keypad Enter ->
 * '\n', Tab -> '\t'.  Returns 0 when the key has no character. */
char keyboard_map_usage_to_char(uint8_t usage, uint8_t modifiers);

/* Parse one boot report into *out.  Returns 0 on success, -1 when len is not
 * 8 or when the report is a rollover error (all six key bytes 0x01); a
 * rejected report leaves *out untouched and must be ignored. */
int keyboard_map_report_parse(const uint8_t *report, size_t len, keyboard_report_t *out);

/* Compare two reports.  Writes the usages present in cur but not prev to
 * pressed[] and those present in prev but not cur to released[]; both arrays
 * hold KEYBOARD_MAP_REPORT_KEYS entries.  Usages 0 (none) and 1 (rollover)
 * are skipped, and a usage repeated within one report counts once.  Returns
 * the number of pressed entries and stores the number of released entries in
 * *num_released (may be NULL). */
size_t keyboard_map_report_diff(const keyboard_report_t *prev, const keyboard_report_t *cur,
                                uint8_t *pressed, uint8_t *released, size_t *num_released);
