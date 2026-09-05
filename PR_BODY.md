Bluetooth keyboard support: BLE HID (HOGP) host on the nRF52840 targets (#165)

Closes #165.

**A note on the repository.** I filed #165 here thinking RebbleOS was the firmware my Pebble Round 2 ships with. It is not: the Round 2 runs Core Devices' PebbleOS, and the matching request now lives at https://github.com/coredevices/PebbleOS/issues/1996. This PR still stands on its own for the nRF52840 boards RebbleOS supports; the design (button injection, keyboard module, boot-protocol host) is what the PebbleOS port will follow.

## What this adds

A Bluetooth LE keyboard can be paired with the watch on the nRF52840 targets (`asterix`, `asterix_vla_dvb1`, `asterix_vla_dvb2`). The watch acts as the BLE central and HID-over-GATT host: it scans, connects, bonds (Just Works), discovers the HID service, subscribes to the keyboard's input report, and turns key presses into button presses, so every existing app works unchanged.

1. Key map: Up/Down arrows = Up/Down; Enter, keypad Enter and Right arrow = Select; Escape, Left arrow and Backspace = Back. Holding a key repeats and long-presses like a physical button.
2. Settings -> Keyboard: status row, "Pair new keyboard" (30 s active scan; the first advertiser with the HID service UUID 0x1812 or the Keyboard appearance is connected and bonded), "Connect", "Forget keyboard", and a "Last key" row as a proof of life for testers.
3. System apps can subscribe to raw key events (usage ID, modifiers, printable character, link state) through `keyboard_service`, the same pattern as `connection_service`.
4. One bonded keyboard is stored in prefs (`PREFS_KEY_KEYBOARD_BOND`). While bonded and disconnected, a low duty pending `sd_ble_gap_connect` (2 s interval, 22.5 ms window) reconnects when the keyboard wakes and advertises.

## How it is layered

1. `hw/drivers/nrf52_bluetooth/nrf52_bluetooth_hid.c` (new): the central link. Scanning, connecting, pairing (`sd_ble_gap_authenticate` / `sd_ble_gap_encrypt` with the stored keys), GATT discovery with direct `sd_ble_gattc_*` calls (the SDK's `ble_db_discovery` keeps six characteristics per service, and a HID service has more), CCCD writes, notifications. Every SoftDevice event runs in interrupt context (`NRF_SDH_DISPATCH_MODEL` is INTERRUPT), so reports go into a small ring and everything that calls the core or the filesystem is deferred to the service worker thread with `service_submit()`. Every state transition and every GAP/GATTC event on the keyboard link is logged with an `HID:` prefix.
2. `hw/drivers/nrf52_bluetooth/nrf52_bluetooth.c` and `nrf52_bluetooth_ppogatt.c`: the phone-link handlers now ignore events from other connections and only treat a peripheral-role connection as the phone, so the keyboard link cannot wipe PPoGATT state or restart advertising. The RAM check after `nrf_sdh_ble_enable()` now uses the value the SoftDevice reports and prints both addresses when it panics.
3. `hw/platform/asterix/sdk_config.h`: `NRF_SDH_BLE_CENTRAL_LINK_COUNT` 1, `NRF_SDH_BLE_TOTAL_LINK_COUNT` 2. `hw/chip/nrf52840/nrf52840.lds`: application RAM origin 0x20002300 -> 0x20003000 (the SDK's one-central-plus-one-peripheral example needs 0x200028d8).
4. `rcore/buttons.c`: `button_inject_state(button, pressed)` presses or releases a button from software. It sets a virtual bit and wakes the button thread, so debounce, long-click, repeat and overlay/app ownership apply exactly as to a physical press. A software press is latched until the button thread has delivered it, and a change inside the debounce window now schedules a re-poll instead of waiting for the next interrupt. `rwatch/event/event_service.c`: `event_service_unsubscribe_thread()` now matches the command it is given, and `event_service_unsubscribe_thread_all()` removes every subscription of the thread (both removed only the first one found). `rcore/service.c`: `service_submit()` reports whether the packet was queued.
5. `rcore/keyboard.c` / `rcore/keyboard_map.c` (new): boot-report parsing and diffing, usage-to-button and usage-to-character (US layout) mapping, per-button hold counts (two keys can hold the same button), release of every held button when the link drops. `keyboard_map.c` has no firmware includes and is covered by a host unit test (`make -C tests/host`).
6. `rwatch/event/keyboard_service.c` (new) and `EventServiceCommandKeyboard`: event delivery to the subscribing app thread.
7. `Apps/System/settings_keyboard.c` (new), `Apps/System/settings.c`, `README.md`, `docs/bluetooth_keyboard.md`.

## Protocol choices

1. Boot protocol when the keyboard offers the Boot Keyboard Input Report and Protocol Mode characteristics (Protocol Mode is written to Boot, then the boot input report is subscribed). Otherwise every Report characteristic whose Report Reference descriptor says "input" is subscribed and 8-byte reports are read as the standard keyboard layout. NKRO bitmap reports and consumer-control (media key) reports are ignored; there is no Report Map parser yet.
2. Pairing is legacy Just Works (`io_caps` NONE, no MITM, no LE Secure Connections). A keyboard that insists on a passkey or on Secure Connections only does not pair.

## Testing

1. Compile-tested only. `make snowy`, `make asterix` and `make asterix_vla_dvb2` (the CI targets) build with gcc-arm-none-eabi 13.2 and nRF5 SDK 16.0.0 with no new warnings. Heap left: asterix 97,768 bytes (102,272 before; 3,328 of the difference is the RAM origin move), snowy 1,260 bytes (1,392 before; snowy compiles the core module and the Settings page but not the driver).
2. `make -C tests/host` builds `rcore/keyboard_map.c` with the native compiler and runs the mapping, parsing and diff checks.
3. Not run on hardware: no nRF52840 board was available. The first tester should expect to debug rather than type. The `HID:` log lines show every state transition (`idle -> scanning -> connecting -> pairing -> discovering -> subscribing -> ready`) and every GAP/GATTC event on the keyboard link; please attach the log from "keyboard host initialised" onward to any report. Assumptions about SoftDevice behaviour that the S140 headers do not settle are listed in the header comment and in the commit message of the driver commit (rejected stored key reported as `CONN_SEC_UPDATE` level 1; `CONN_SEC_UPDATE` before `AUTH_STATUS` in central legacy bonding; the device identity list not being held by the advertiser).

## Known limits

1. One bonded keyboard; the first HID service and at most four Report characteristics.
2. The keyboard name comes from advertising data only; an unnamed keyboard is stored as "Keyboard".
3. The pending reconnect keeps the initiator scanning at about 1.1% duty while a bonded keyboard is away; its power cost is unmeasured.
4. A keyboard that uses resolvable private addresses is reconnected through its identity address only when it distributed an IRK and the SoftDevice accepts the device identity list while the phone advertiser runs; otherwise the last connection address is used and the reconnect stops once that address rotates.
5. The pairing scan skips advertisers whose appearance is a non-keyboard HID subtype, so a keyboard-and-touchpad combo that advertises as a mouse is never picked.
6. `keyboard_service` has one subscriber at a time (see the comment in `keyboard_service.c` on how `event_service_event_trigger()` destroys events). Events travel packed in the event_service data word, so nothing is allocated per key press.

## A note on the Pebble Round 2

The Round 2 uses a SiFli SF32LB52 and ships with Core Devices' PebbleOS (NimBLE), which RebbleOS does not target, so this change only reaches the nRF52840 boards RebbleOS already supports. The PebbleOS side is tracked at https://github.com/coredevices/PebbleOS/issues/1996; the design carries over, the SoftDevice calls do not.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_016oVW6HuVvmKzZ3ayTp3vxq
