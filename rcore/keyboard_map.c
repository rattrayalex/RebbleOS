/* keyboard_map.c
 * Pure HID keyboard logic for the external keyboard: usage mapping and
 * boot-report parsing.  Only the C library is used here, so tests/host
 * builds this file with the native compiler.
 * RebbleOS
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "keyboard_map.h"

/* USB HID Usage Tables, keyboard/keypad page (0x07). */
#define _USAGE_NONE      0x00
#define _USAGE_ROLLOVER  0x01
#define _USAGE_A         0x04
#define _USAGE_Z         0x1D
#define _USAGE_1         0x1E
#define _USAGE_0         0x27
#define _USAGE_ENTER     0x28
#define _USAGE_ESCAPE    0x29
#define _USAGE_BACKSPACE 0x2A
#define _USAGE_TAB       0x2B
#define _USAGE_SPACE     0x2C
#define _USAGE_RIGHT     0x4F
#define _USAGE_LEFT      0x50
#define _USAGE_DOWN      0x51
#define _USAGE_UP        0x52
#define _USAGE_KP_ENTER  0x58

/* Left or right shift in report byte 0. */
#define _MOD_SHIFT 0x22

/* Digits 1..9, 0 (usages 0x1E..0x27) and their shifted symbols. */
static const char _digits[]       = "1234567890";
static const char _digits_shift[] = "!@#$%^&*()";

/* Punctuation keys with their shifted characters. */
static const struct {
    uint8_t usage;
    char normal;
    char shifted;
} _punctuation[] = {
    { 0x2D, '-',  '_' },
    { 0x2E, '=',  '+' },
    { 0x2F, '[',  '{' },
    { 0x30, ']',  '}' },
    { 0x31, '\\', '|' },
    { 0x33, ';',  ':' },
    { 0x34, '\'', '"' },
    { 0x35, '`',  '~' },
    { 0x36, ',',  '<' },
    { 0x37, '.',  '>' },
    { 0x38, '/',  '?' },
};

int keyboard_map_usage_to_button(uint8_t usage)
{
    switch (usage) {
        case _USAGE_UP:
            return KEYBOARD_MAP_BUTTON_UP;
        case _USAGE_DOWN:
            return KEYBOARD_MAP_BUTTON_DOWN;
        case _USAGE_ENTER:
        case _USAGE_KP_ENTER:
        case _USAGE_RIGHT:
            return KEYBOARD_MAP_BUTTON_SELECT;
        case _USAGE_ESCAPE:
        case _USAGE_LEFT:
        case _USAGE_BACKSPACE:
            return KEYBOARD_MAP_BUTTON_BACK;
        default:
            return -1;
    }
}

char keyboard_map_usage_to_char(uint8_t usage, uint8_t modifiers)
{
    bool shift = (modifiers & _MOD_SHIFT) != 0;

    if (usage >= _USAGE_A && usage <= _USAGE_Z)
        return (char)((shift ? 'A' : 'a') + (usage - _USAGE_A));
    if (usage >= _USAGE_1 && usage <= _USAGE_0)
        return (shift ? _digits_shift : _digits)[usage - _USAGE_1];

    switch (usage) {
        case _USAGE_ENTER:
        case _USAGE_KP_ENTER:
            return '\n';
        case _USAGE_TAB:
            return '\t';
        case _USAGE_SPACE:
            return ' ';
        default:
            break;
    }

    for (size_t i = 0; i < sizeof(_punctuation) / sizeof(_punctuation[0]); i++) {
        if (_punctuation[i].usage == usage)
            return shift ? _punctuation[i].shifted : _punctuation[i].normal;
    }

    return 0;
}

int keyboard_map_report_parse(const uint8_t *report, size_t len, keyboard_report_t *out)
{
    if (!report || !out || len != KEYBOARD_MAP_REPORT_LEN)
        return -1;

    /* Six 0x01 bytes: too many keys held, the keyboard reports nothing usable. */
    bool rollover = true;
    for (size_t i = 0; i < KEYBOARD_MAP_REPORT_KEYS; i++) {
        if (report[2 + i] != _USAGE_ROLLOVER)
            rollover = false;
    }
    if (rollover)
        return -1;

    out->modifiers = report[0];
    memcpy(out->keys, report + 2, KEYBOARD_MAP_REPORT_KEYS);
    return 0;
}

static bool _report_has(const keyboard_report_t *report, uint8_t usage)
{
    for (size_t i = 0; i < KEYBOARD_MAP_REPORT_KEYS; i++) {
        if (report->keys[i] == usage)
            return true;
    }
    return false;
}

/* Write the usages of `from` that `in` lacks to out[]; return how many. */
static size_t _keys_not_in(const keyboard_report_t *from, const keyboard_report_t *in, uint8_t *out)
{
    size_t n = 0;

    for (size_t i = 0; i < KEYBOARD_MAP_REPORT_KEYS; i++) {
        uint8_t usage = from->keys[i];
        bool duplicate = false;

        if (usage == _USAGE_NONE || usage == _USAGE_ROLLOVER)
            continue;
        for (size_t j = 0; j < i; j++) {
            if (from->keys[j] == usage)
                duplicate = true;
        }
        if (duplicate || _report_has(in, usage))
            continue;
        out[n++] = usage;
    }
    return n;
}

size_t keyboard_map_report_diff(const keyboard_report_t *prev, const keyboard_report_t *cur,
                                uint8_t *pressed, uint8_t *released, size_t *num_released)
{
    size_t n_released = _keys_not_in(prev, cur, released);

    if (num_released)
        *num_released = n_released;
    return _keys_not_in(cur, prev, pressed);
}
