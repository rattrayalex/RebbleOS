# Bluetooth keyboard support (BLE HID host)

RebbleOS can act as a Bluetooth Low Energy HID host on the nRF52840 boards
(Asterix and Vla-52840).  The watch scans for a keyboard, connects to it,
bonds with it, and turns key presses into the four Pebble buttons, so every
existing app works unchanged.  System apps can also subscribe to the raw
key events.  TinTin and Snowy (CC256x with btstack), Chalk and Silk are not
supported: `hw_keyboard_is_supported()` returns false there and the
Settings page shows "Not supported on this watch".

Status: compile-tested on the CI targets (`snowy`, `asterix`,
`asterix_vla_dvb2`); not yet run on hardware.  Testers with an Asterix
board: please report the `HID:` log lines from the serial console, plus
what the Settings -> Keyboard page shows.

## How to use it

1. Put the keyboard in pairing mode.
2. On the watch, open Settings -> Keyboard -> "Pair new keyboard".  The
   status row shows "Scanning...", then "Connecting...", then
   "Connected: <name>".
3. Press a key.  The "Last key" row shows the character, or the HID usage
   ID for keys without one (`usage 0x28` for Enter).
4. "Connect" reconnects to the bonded keyboard; "Forget keyboard" drops
   the bond.  Both rows appear only while a bond exists.

Key mapping (`keyboard_usage_to_button()` in rcore/keyboard.c):

| Key                           | Button |
| ----------------------------- | ------ |
| Up arrow                      | Up     |
| Down arrow                    | Down   |
| Enter, Right arrow            | Select |
| Escape, Left arrow, Backspace | Back   |

Other keys press no button; they reach system apps through
`keyboard_service` only.  A held key repeats the way a held button does,
because the button thread applies its own repeat.

## How it is layered

1. `hw/drivers/nrf52_bluetooth/nrf52_bluetooth_hid.c`: scans, connects on
   a central link, discovers the HID service (UUID 0x1812), enables
   notifications on the input report, and receives the reports from the
   SoftDevice.  The phone keeps its own peripheral link.
2. The driver hands each report and link change to the service thread
   (rcore/service.c), which calls `keyboard_hid_boot_report()` and
   `keyboard_link_state_changed()`.
3. `rcore/keyboard.c` compares each report with the previous one to find
   the keys that went down and up.  Mapped keys call
   `button_inject_state()` (rcore/buttons.c), so debounce, long click,
   repeat and overlay/app ownership behave as for a physical button.
   Every key press and release also goes to `keyboard_service_post()`; a
   change of the modifier keys alone posts nothing.
4. `rwatch/event/keyboard_service.c` delivers a `KeyboardEvent` (type
   KeyDown, KeyUp or LinkState; usage, modifiers, character, link_state)
   to the handler registered with `keyboard_service_subscribe()`.
5. `Apps/System/settings_keyboard.c` is the Settings -> Keyboard page.

## Protocol, pairing and bonding

- Boot protocol when the keyboard offers it (Protocol Mode set to 0, Boot
  Keyboard Input Report); otherwise the standard 8-byte keyboard input
  report: modifiers, reserved, six key usages.  Reports that are not 8
  bytes (NKRO bitmap reports) and consumer/media-key reports are ignored.
- Characters use the US layout (`keyboard_usage_to_char()`); Shift selects
  the shifted character, and all modifier bits are passed on in
  `modifiers`.
- Just Works pairing: no passkey is shown or typed.
- One bonded keyboard.  The bond is stored in prefs and survives a reboot.
  While a bond exists the driver reconnects in the background when the
  keyboard reappears; "Forget keyboard" stops that.

## Subscribing from a system app

```c
#include "keyboard_service.h"

static void _on_key(KeyboardEvent *event, void *context)
{
    if (event->type == KeyboardEventKeyDown && event->character)
        APP_LOG("myapp", APP_LOG_LEVEL_INFO, "key %c", event->character);
}

keyboard_service_subscribe(_on_key, NULL);   /* window load handler */
keyboard_service_unsubscribe();              /* window unload handler */
```

The handler runs on the thread that subscribed.

## Known limits

1. Third-party (SDK) apps see button presses only; `keyboard_service` is
   not in the app ABI table (rcore/api_func_symbols.h).
2. No text entry widget exists yet; notification replies and the dictation
   API stay unimplemented.
3. US layout only.  One bonded keyboard at a time.  Pairing is legacy
   Just Works: keyboards that require a passkey or LE Secure Connections
   only do not pair.
4. Media keys, NKRO bitmap reports and mouse or touchpad reports are
   ignored.  Bluetooth Classic keyboards are not supported.  The pairing
   scan skips advertisers whose appearance is a non-keyboard HID subtype,
   so a keyboard-and-touchpad combo that advertises as a mouse is never
   picked.
5. A keyboard that uses resolvable private addresses is reconnected
   through its identity address only when it distributed an IRK and the
   SoftDevice accepts the device identity list while the phone advertiser
   is running; otherwise the last connection address is used, and the
   reconnect stops working once that address rotates.
6. `keyboard_service` has one subscriber at a time (see the comment at the
   top of rwatch/event/keyboard_service.c).
7. Not yet run on hardware.  The second link raises the SoftDevice RAM
   demand; a too-small reservation panics at boot
   (hw/drivers/nrf52_bluetooth/nrf52_bluetooth.c), and this path has not
   been exercised.
