#pragma once

enum prefs_key {
    PREFS_KEY_TZ,
    PREFS_KEY_IS24H,
    PREFS_KEY_WATCHFACE,
    PREFS_KEY_KEYBOARD_BOND,  /* struct hid_bond, hw/drivers/nrf52_bluetooth/nrf52_bluetooth_hid.c */
};

int prefs_get(const uint32_t key, void *buf, uint32_t bufsz);
int prefs_put(const uint32_t key, void *buf, uint32_t bufsz);
