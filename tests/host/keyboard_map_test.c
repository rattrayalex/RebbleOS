/* keyboard_map_test.c
 * Host unit test for rcore/keyboard_map.c.  Build and run: make -C tests/host
 * RebbleOS
 */

#include <stdio.h>
#include <string.h>
#include "keyboard_map.h"

static int _failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        _failures++; \
    } \
} while (0)

static void _test_buttons(void)
{
    CHECK(keyboard_map_usage_to_button(0x52) == KEYBOARD_MAP_BUTTON_UP);
    CHECK(keyboard_map_usage_to_button(0x51) == KEYBOARD_MAP_BUTTON_DOWN);
    CHECK(keyboard_map_usage_to_button(0x28) == KEYBOARD_MAP_BUTTON_SELECT); /* Enter */
    CHECK(keyboard_map_usage_to_button(0x58) == KEYBOARD_MAP_BUTTON_SELECT); /* keypad Enter */
    CHECK(keyboard_map_usage_to_button(0x4F) == KEYBOARD_MAP_BUTTON_SELECT); /* Right */
    CHECK(keyboard_map_usage_to_button(0x29) == KEYBOARD_MAP_BUTTON_BACK);   /* Escape */
    CHECK(keyboard_map_usage_to_button(0x50) == KEYBOARD_MAP_BUTTON_BACK);   /* Left */
    CHECK(keyboard_map_usage_to_button(0x2A) == KEYBOARD_MAP_BUTTON_BACK);   /* Backspace */
    CHECK(keyboard_map_usage_to_button(0x04) == -1);                         /* a */
    CHECK(keyboard_map_usage_to_button(0x00) == -1);
    CHECK(keyboard_map_usage_to_button(0xFF) == -1);
}

static void _test_chars(void)
{
    CHECK(keyboard_map_usage_to_char(0x04, 0x00) == 'a');
    CHECK(keyboard_map_usage_to_char(0x04, 0x02) == 'A');  /* left shift */
    CHECK(keyboard_map_usage_to_char(0x1D, 0x20) == 'Z');  /* right shift */
    CHECK(keyboard_map_usage_to_char(0x04, 0x01) == 'a');  /* ctrl changes nothing */
    CHECK(keyboard_map_usage_to_char(0x1E, 0x00) == '1');
    CHECK(keyboard_map_usage_to_char(0x1E, 0x02) == '!');
    CHECK(keyboard_map_usage_to_char(0x27, 0x00) == '0');
    CHECK(keyboard_map_usage_to_char(0x27, 0x02) == ')');
    CHECK(keyboard_map_usage_to_char(0x2C, 0x00) == ' ');
    CHECK(keyboard_map_usage_to_char(0x2D, 0x02) == '_');
    CHECK(keyboard_map_usage_to_char(0x31, 0x00) == '\\');
    CHECK(keyboard_map_usage_to_char(0x34, 0x02) == '"');
    CHECK(keyboard_map_usage_to_char(0x38, 0x02) == '?');
    CHECK(keyboard_map_usage_to_char(0x28, 0x00) == '\n');
    CHECK(keyboard_map_usage_to_char(0x2B, 0x00) == '\t');
    CHECK(keyboard_map_usage_to_char(0x52, 0x00) == 0);    /* Up arrow */
    CHECK(keyboard_map_usage_to_char(0x32, 0x00) == 0);    /* non-US # */
}

static void _test_parse(void)
{
    const uint8_t valid[8] = { 0x02, 0x00, 0x04, 0x52, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t rollover[8] = { 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
    const uint8_t nine[9] = { 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    keyboard_report_t r;

    memset(&r, 0xAA, sizeof(r));
    CHECK(keyboard_map_report_parse(valid, 8, &r) == 0);
    CHECK(r.modifiers == 0x02 && r.keys[0] == 0x04 && r.keys[1] == 0x52 && r.keys[5] == 0x00);
    CHECK(keyboard_map_report_parse(valid, 7, &r) == -1);
    CHECK(keyboard_map_report_parse(nine, 9, &r) == -1);
    CHECK(keyboard_map_report_parse(rollover, 8, &r) == -1);
    CHECK(keyboard_map_report_parse(NULL, 8, &r) == -1);
}

static void _test_diff(void)
{
    keyboard_report_t prev = { .modifiers = 0, .keys = { 0x04, 0x05, 0, 0, 0, 0 } };
    keyboard_report_t cur = { .modifiers = 0, .keys = { 0x05, 0x06, 0, 0, 0, 0 } };
    keyboard_report_t empty = { .modifiers = 0, .keys = { 0, 0, 0, 0, 0, 0 } };
    keyboard_report_t odd = { .modifiers = 0, .keys = { 0x01, 0x07, 0x07, 0, 0, 0 } };
    uint8_t pressed[KEYBOARD_MAP_REPORT_KEYS];
    uint8_t released[KEYBOARD_MAP_REPORT_KEYS];
    size_t num_released = 99;

    /* One key pressed (0x06), one released (0x04), one held (0x05). */
    CHECK(keyboard_map_report_diff(&prev, &cur, pressed, released, &num_released) == 1);
    CHECK(pressed[0] == 0x06);
    CHECK(num_released == 1 && released[0] == 0x04);

    /* Same report twice: nothing changes. */
    CHECK(keyboard_map_report_diff(&cur, &cur, pressed, released, &num_released) == 0);
    CHECK(num_released == 0);

    /* Usage 1 is not a key; a usage repeated in one report counts once. */
    CHECK(keyboard_map_report_diff(&empty, &odd, pressed, released, &num_released) == 1);
    CHECK(pressed[0] == 0x07 && num_released == 0);
    CHECK(keyboard_map_report_diff(&odd, &empty, pressed, released, NULL) == 0);
}

int main(void)
{
    _test_buttons();
    _test_chars();
    _test_parse();
    _test_diff();

    if (_failures) {
        printf("keyboard_map_test: %d check(s) failed\n", _failures);
        return 1;
    }
    printf("keyboard_map_test: all checks passed\n");
    return 0;
}
