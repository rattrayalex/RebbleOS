Bluetooth keyboard support: BLE HID (HOGP) host on the nRF52840 targets (#165)

Closes #165.

**A note on the repository.** I filed #165 here thinking RebbleOS was the firmware my Pebble Round 2 ships with. It is not: the Round 2 runs Core Devices' PebbleOS, and the matching request now lives at https://github.com/coredevices/PebbleOS/issues/1996. This PR still stands on its own for the nRF52840 boards RebbleOS supports; the design (button injection, keyboard module, boot-protocol host) is what the PebbleOS port will follow.

## What this adds

A Bluetooth LE keyboard can be paired with the watch on the nRF52840 targets (`asterix`, `asterix_vla_dvb1`, `asterix_vla_dvb2`). The watch acts as BLE central and HID-over-GATT host: scan, connect, Just Works bond, HID service discovery, input-report subscription, and key presses turned into button presses, so every existing app works unchanged.

1. Key map: Up/Down arrows = Up/Down; Enter, keypad Enter and Right arrow = Select; Escape, Left arrow and Backspace = Back. Holding a key repeats and long-presses like a physical button.
2. Settings -> Keyboard: status, "Pair new keyboard" (30 s scan; the first advertiser with the HID service UUID 0x1812 or the Keyboard appearance is connected and bonded), "Connect", "Forget keyboard", and a "Last key" row for testers.
3. `keyboard_service`: raw key events (usage, modifiers, character, link state) for system apps.
4. One bonded keyboard stored in prefs; a low duty pending `sd_ble_gap_connect` reconnects when the keyboard wakes and advertises.

## Layout

1. `hw/drivers/nrf52_bluetooth/nrf52_bluetooth_hid.c` (new): the central link, with direct `sd_ble_gattc_*` discovery (the SDK's `ble_db_discovery` keeps six characteristics per service, and a HID service has more). SoftDevice events run in interrupt context, so reports go through a ring to the service thread. Every state transition and every GAP/GATTC event on the keyboard link is logged with an `HID:` prefix.
2. `nrf52_bluetooth.c` / `nrf52_bluetooth_ppogatt.c`: the phone-link handlers ignore other connections and treat only a peripheral-role connection as the phone; the RAM check after `nrf_sdh_ble_enable()` now uses the value the SoftDevice reports.
3. `hw/platform/asterix/sdk_config.h`: central link count 1, total 2. `hw/chip/nrf52840/nrf52840.lds`: application RAM origin 0x20002300 -> 0x20003000.
4. `rcore/buttons.c`: `button_inject_state()`; a software press is latched until the button thread delivers it, and a change inside the debounce window schedules a re-poll. `rwatch/event/event_service.c`: `event_service_unsubscribe_thread()` honours its command and `event_service_unsubscribe_thread_all()` removes every subscription. `rcore/service.c`: `service_submit()` reports whether the packet was queued.
5. `rcore/keyboard.c` and `rcore/keyboard_map.c` (new; the latter is host-testable with `make -C tests/host`), `rwatch/event/keyboard_service.c` (new), `Apps/System/settings_keyboard.c` (new), `README.md`, `docs/bluetooth_keyboard.md`.

## Protocol choices

Boot protocol when the keyboard offers it; otherwise every Report characteristic whose Report Reference says "input", read as the standard 8-byte layout. No Report Map parser yet, so NKRO bitmap reports and media keys are ignored. Pairing is legacy Just Works; a keyboard that requires a passkey or Secure Connections only does not pair.

## Testing

Compile-tested only. `make snowy`, `make asterix` and `make asterix_vla_dvb2` (the CI targets) build with gcc-arm-none-eabi 13.2 and nRF5 SDK 16.0.0 with no new warnings; `make -C tests/host` passes. Not run on hardware: no nRF52840 board was available. The first tester should expect to debug rather than type; please attach the log from "keyboard host initialised" onward. Assumptions about SoftDevice behaviour that the S140 headers do not settle are listed in the driver's header comment and in its commit message.

## Known limits

One bonded keyboard; the first HID service and at most four Report characteristics; the name comes from advertising data only; the pending reconnect keeps the initiator at about 1.1% radio duty while a bonded keyboard is away; a keyboard with resolvable private addresses reconnects only through a distributed IRK; combos that advertise as a mouse are skipped; `keyboard_service` has one subscriber at a time. Details in `docs/bluetooth_keyboard.md`.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_016oVW6HuVvmKzZ3ayTp3vxq
