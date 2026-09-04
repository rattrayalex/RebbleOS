/* nrf52_bluetooth_internal.h
 * Definitions for internal components of nRF52 Bluetooth driver, including PPoGATT.  Not for external use.
 * RebbleOS
 *
 * Author: Joshua Wise <joshua@joshuawise.com>
 */
#pragma once

#include "ble_db_discovery.h"

/* XXX: really needs to be in a platform config. */
#define PPOGATT_MTU 256

/* Connection configuration tag shared by the phone link (advertising) and
 * the keyboard link (sd_ble_gap_connect); nrf_sdh_ble_default_cfg_set()
 * sizes it for NRF_SDH_BLE_TOTAL_LINK_COUNT connections. */
#define CONN_TAG 1

/* nrf52_bluetooth_ppogatt.c */
void nrf52_ppogatt_bond_complete();
void nrf52_ppogatt_discovery(ble_db_discovery_evt_t *evt);
void nrf52_ppogatt_init();

/* nrf52_bluetooth_hid.c: BLE HID (HOGP) keyboard host on a central link */
void nrf52_hid_init(void);
