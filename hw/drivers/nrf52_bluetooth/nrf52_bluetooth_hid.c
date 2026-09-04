/* nrf52_bluetooth_hid.c
 * BLE HID-over-GATT (HOGP) keyboard host on an nRF52 SoftDevice central link
 * RebbleOS
 *
 * The phone talks to the watch over a peripheral link (nrf52_bluetooth.c,
 * nrf52_bluetooth_ppogatt.c).  This file owns the one central link that
 * the SoftDevice is configured for (NRF_SDH_BLE_CENTRAL_LINK_COUNT 1) and
 * uses it to find a Bluetooth LE keyboard, bond with it, subscribe to its
 * input reports and hand those reports to rcore/keyboard.c.
 *
 * Context rules.  NRF_SDH_DISPATCH_MODEL is INTERRUPT, so _hid_handler()
 * runs in the SoftDevice event interrupt.  It may call sd_* functions and
 * update this file's static state, but it must not block and must not
 * call into rcore.  Everything that blocks (prefs_get, prefs_put) or calls
 * the core (keyboard_hid_boot_report, keyboard_link_state_changed) is
 * deferred to the service thread with service_submit().  The hw_keyboard_*
 * entry points run on app threads; they call sd_* functions directly (the
 * SoftDevice API is an SVC interface and may be used from any thread) and
 * protect the shared state with taskENTER_CRITICAL(), which masks the
 * SoftDevice event interrupt (service_submit() from that interrupt already
 * relies on it running at a FreeRTOS-maskable priority).  No sd_* call is
 * ever made inside a critical section: an SVC issued with BASEPRI raised
 * faults.
 *
 * State machine (see enum hid_state): idle -> scanning -> connecting ->
 * pairing -> discovering -> subscribing -> ready, with any link event able
 * to drop back to idle.  A background reconnect is "connecting" without a
 * preceding scan.  Every transition logs "HID: state a -> b".
 */

#include <debug.h>
#include "rebbleos.h"
#include "service.h"
#include "prefs.h"
#include "keyboard.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "ble_gap.h"
#include "ble_gattc.h"
#include "ble_gatts.h"
#include "ble_hci.h"
#include "ble_advdata.h"
#include "ble_srv_common.h"
#include "app_util.h"
#include "nrf52_bluetooth_internal.h"

int sfmt(char *buf, unsigned int len, const char *ifmt, ...);

#define HID_LOG(lvl_, fmt_, ...) DRV_LOG("bt", lvl_, "HID: " fmt_, ##__VA_ARGS__)
#define HID_ARRAY_SIZE(a_) (sizeof(a_) / sizeof((a_)[0]))

/***** Tunables *****/

/* Pairing scan: active, 100 ms interval, 50 ms window, 30 s timeout. */
#define HID_SCAN_INTERVAL       MSEC_TO_UNITS(100, UNIT_0_625_MS)
#define HID_SCAN_WINDOW         MSEC_TO_UNITS(50, UNIT_0_625_MS)
#define HID_SCAN_TIMEOUT        3000  /* 10 ms units: 30 s */
#define HID_CONNECT_TIMEOUT     1000  /* 10 ms units: 10 s for the connect after a scan hit */

/* Background reconnect: 2 s interval, 22.5 ms window (36 x 0.625 ms), no
 * timeout, so the initiator sits in the SoftDevice until the keyboard
 * advertises again. */
#define HID_RECONNECT_INTERVAL  MSEC_TO_UNITS(2000, UNIT_0_625_MS)
#define HID_RECONNECT_WINDOW    36

/* Connection parameters requested from the keyboard. */
#define HID_CONN_INTERVAL_MIN   MSEC_TO_UNITS(15, UNIT_1_25_MS)
#define HID_CONN_INTERVAL_MAX   MSEC_TO_UNITS(30, UNIT_1_25_MS)
#define HID_CONN_SUP_TIMEOUT    MSEC_TO_UNITS(4000, UNIT_10_MS)

#define HID_RETRY_MS            50     /* retry a busy SoftDevice call after this */
#define HID_WATCHDOG_MS         15000  /* a link that is not ready by then is dropped */
#define HID_FAIL_BACKOFF_MS     5000   /* delay before reconnecting after a failure */

#define HID_MAX_INPUTS          5      /* one Boot Keyboard Input Report plus up to 4 Report characteristics */
#define HID_MAX_REPORT_CHARS    4
#define HID_REPORT_MAX          16     /* bytes kept per input report */
#define HID_RING_SLOTS          8

#define HID_REPORT_TYPE_INPUT   1      /* Report Reference descriptor, report type field */
#define HID_PROTOCOL_MODE_BOOT  0x00

/* Appearance values 0x03C0..0x03FF are the HID category; 960 is generic HID
 * and 961 is keyboard.  Everything else in the category (mouse 962, ...) is
 * not a keyboard. */
#define HID_APPEARANCE_CATEGORY_MASK 0xFFC0
#define HID_APPEARANCE_CATEGORY      0x03C0

/***** Link state *****/

enum hid_state {
    HID_IDLE = 0,
    HID_SCANNING,
    HID_CONNECTING,
    HID_PAIRING,
    HID_DISCOVERING,
    HID_SUBSCRIBING,
    HID_READY,
};

static const char *_state_names[] = {
    "idle", "scanning", "connecting", "pairing", "discovering", "subscribing", "ready"
};

static enum hid_state _hid_state = HID_IDLE;
static uint16_t _hid_conn = BLE_CONN_HANDLE_INVALID;
static uint32_t _hid_link_gen = 0;       /* bumps on every connection; tags deferred work */
static ble_gap_addr_t _hid_peer_addr;    /* address the current link was made with */
static char _hid_name[KEYBOARD_NAME_MAX] = "";

static uint8_t _hid_connect_is_pairing = 0;   /* the pending / current connection came from a pairing scan */
static uint8_t _hid_pairing_requested = 0;    /* sd_ble_gap_authenticate() was called on this link */
static uint8_t _hid_encrypt_fallback_used = 0;/* a rejected stored key already fell back to pairing */
static uint8_t _hid_encrypted = 0;            /* BLE_GAP_EVT_CONN_SEC_UPDATE reported level >= 2 */
static uint8_t _hid_auth_done = 0;            /* BLE_GAP_EVT_AUTH_STATUS success arrived */
static uint8_t _hid_pair_after_disconnect = 0;/* hw_keyboard_pair_start() while connected: scan when the link drops */
static uint8_t _hid_forgetting = 0;           /* hw_keyboard_forget() is dropping the link: do not reconnect */
static uint8_t _hid_reconnect_backoff = 0;    /* the last link failed: delay the next background reconnect */
static uint32_t _hid_hvx_logged = 0;          /* HVX events logged at INFO so far */

/***** Bond record *****/

/* Persisted under PREFS_KEY_KEYBOARD_BOND.  Natural (not packed) layout on
 * purpose: the SoftDevice is handed pointers into this record
 * (sd_ble_gap_encrypt, sd_ble_gap_device_identities_set), the master_id
 * inside ble_gap_enc_key_t needs 2-byte alignment, and GCC's
 * -Waddress-of-packed-member would flag every such pointer.  The record is
 * only ever read back by the firmware that wrote it; a size mismatch on
 * load is treated as "no bond". */
struct hid_bond {
    uint8_t valid;
    ble_gap_addr_t addr;         /* address the keyboard connected with (may be a resolvable private address) */
    ble_gap_enc_key_t peer_enc;  /* keyboard's LTK + master id: what sd_ble_gap_encrypt() needs from the central */
    ble_gap_id_key_t peer_id;    /* keyboard's IRK + identity address, for private addresses */
    ble_gap_enc_key_t own_enc;   /* our LTK; unused by a central but kept with the bond */
    char name[KEYBOARD_NAME_MAX];
};

static struct hid_bond _bond;    /* working copy; written by the event handler, snapshotted by threads under a critical section */
static uint8_t _bond_loaded = 0;

/* Key storage the SoftDevice fills during pairing (sd_ble_gap_sec_params_reply). */
static ble_gap_enc_key_t _keys_own_enc, _keys_peer_enc;
static ble_gap_id_key_t _keys_own_id, _keys_peer_id;

/* S140: in the central role the keyset given to sd_ble_gap_sec_params_reply
 * must provide memory for the peer's keys; the own-key pointers may be NULL
 * but we want our LTK for the record too. */
static const ble_gap_sec_keyset_t _hid_keyset = {
    .keys_own = {
        .p_enc_key = &_keys_own_enc,
        .p_id_key = &_keys_own_id,
        .p_sign_key = NULL,
        .p_pk = NULL,
    },
    .keys_peer = {
        .p_enc_key = &_keys_peer_enc,
        .p_id_key = &_keys_peer_id,
        .p_sign_key = NULL,
        .p_pk = NULL,
    },
};

/* Just Works bonding: no I/O, no MITM, legacy pairing, LTK + IRK both ways. */
static const ble_gap_sec_params_t _hid_sec_params = {
    .bond = 1,
    .mitm = 0,
    .lesc = 0,
    .keypress = 0,
    .io_caps = BLE_GAP_IO_CAPS_NONE,
    .oob = 0,
    .min_key_size = 7,
    .max_key_size = 16,
    .kdist_own = { .enc = 1, .id = 1 },
    .kdist_peer = { .enc = 1, .id = 1 },
};

/***** Scanning and connecting *****/

/* S140: the scan buffer must stay alive until the scanner stops; a legacy
 * (non-extended) scan needs at least BLE_GAP_SCAN_BUFFER_MIN bytes. */
static uint8_t _scan_buf_data[BLE_GAP_SCAN_BUFFER_MIN];
static const ble_data_t _scan_buf = { .p_data = _scan_buf_data, .len = sizeof(_scan_buf_data) };

static const ble_gap_scan_params_t _scan_params_pair = {
    .extended = 0,
    .report_incomplete_evts = 0,
    .active = 1,
    .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    .scan_phys = BLE_GAP_PHY_1MBPS,
    .interval = HID_SCAN_INTERVAL,
    .window = HID_SCAN_WINDOW,
    .timeout = HID_SCAN_TIMEOUT,
};

/* Initiator parameters after a scan hit ("active" is ignored by sd_ble_gap_connect). */
static const ble_gap_scan_params_t _scan_params_connect = {
    .extended = 0,
    .report_incomplete_evts = 0,
    .active = 0,
    .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    .scan_phys = BLE_GAP_PHY_1MBPS,
    .interval = HID_SCAN_INTERVAL,
    .window = HID_SCAN_WINDOW,
    .timeout = HID_CONNECT_TIMEOUT,
};

/* Low duty initiator for the background reconnect. */
static const ble_gap_scan_params_t _scan_params_reconnect = {
    .extended = 0,
    .report_incomplete_evts = 0,
    .active = 0,
    .filter_policy = BLE_GAP_SCAN_FP_ACCEPT_ALL,
    .scan_phys = BLE_GAP_PHY_1MBPS,
    .interval = HID_RECONNECT_INTERVAL,
    .window = HID_RECONNECT_WINDOW,
    .timeout = BLE_GAP_SCAN_TIMEOUT_UNLIMITED,
};

static const ble_gap_conn_params_t _conn_params = {
    .min_conn_interval = HID_CONN_INTERVAL_MIN,
    .max_conn_interval = HID_CONN_INTERVAL_MAX,
    .slave_latency = 0,
    .conn_sup_timeout = HID_CONN_SUP_TIMEOUT,
};

/* Candidate found by the pairing scan; we wait one more report for its name. */
static ble_gap_addr_t _cand_addr;
static uint8_t _cand_valid = 0;
static uint8_t _cand_reports = 0;
static uint32_t _scan_reports_seen = 0;

/* Device identity list entry for a keyboard with a resolvable private address. */
static ble_gap_id_key_t const *_id_list[1];

/***** GATT discovery *****/

enum hid_input_kind {
    INPUT_BOOT_KEYBOARD,   /* 0x2A22 */
    INPUT_REPORT,          /* 0x2A4D */
};

struct hid_input {
    uint8_t kind;
    uint8_t notify;
    uint8_t report_id;
    uint8_t report_type;   /* from the Report Reference descriptor; 0 when unknown */
    uint8_t subscribed;
    uint16_t decl;         /* characteristic declaration handle */
    uint16_t value;        /* characteristic value handle */
    uint16_t desc_end;     /* last handle that may hold this characteristic's descriptors; 0 until known */
    uint16_t cccd;         /* BLE_GATT_HANDLE_INVALID when not found */
    uint16_t ref;          /* Report Reference descriptor, BLE_GATT_HANDLE_INVALID when not found */
};

enum hid_disc_phase {
    DISC_NONE = 0,
    DISC_SERVICE,     /* sd_ble_gattc_primary_services_discover(0x1812) */
    DISC_CHARS,       /* sd_ble_gattc_characteristics_discover over the service */
    DISC_DESCS,       /* sd_ble_gattc_descriptors_discover per input characteristic */
    DISC_REFS,        /* sd_ble_gattc_read of each Report Reference descriptor */
    DISC_PROTO,       /* write Protocol Mode = boot (write command) */
    DISC_PROTO_WAIT,  /* waiting for BLE_GATTC_EVT_WRITE_CMD_TX_COMPLETE */
    DISC_CCCD,        /* write CCCD = notification (write request) per chosen input */
    DISC_DONE,
};

static enum hid_disc_phase _disc_phase = DISC_NONE;
static ble_gattc_handle_range_t _svc_range;
static uint16_t _proto_mode_hnd = BLE_GATT_HANDLE_INVALID;
static struct hid_input _inputs[HID_MAX_INPUTS];
static uint8_t _n_inputs = 0;
static uint8_t _n_report_chars = 0;
static struct hid_input *_last_input = NULL;  /* last characteristic seen was this input: its desc_end is still open */
static uint16_t _char_next = 0;               /* start handle for the next characteristic discovery call */
static uint16_t _desc_next = 0;               /* start handle for the next descriptor discovery call, 0 = not started */
static uint8_t _disc_idx = 0;                 /* index into _inputs for DESCS / REFS / CCCD */
static uint8_t _use_boot = 0;

/***** Input report ring (event context -> service thread) *****/

struct hid_slot {
    uint8_t len;
    uint8_t data[HID_REPORT_MAX];
};

static struct hid_slot _ring[HID_RING_SLOTS];
static volatile uint8_t _ring_head = 0;   /* next slot to write */
static volatile uint8_t _ring_tail = 0;   /* next slot to read */
static volatile uint8_t _ring_count = 0;
static volatile uint8_t _ring_drain_pending = 0;
static uint32_t _ring_dropped = 0;

/***** Deferred retry of a busy SoftDevice call *****/

enum hid_retry {
    RETRY_NONE = 0,
    RETRY_ENCRYPT,
    RETRY_AUTHENTICATE,
    RETRY_SEC_PARAMS_REPLY,
    RETRY_DISCOVERY,
};

static uint8_t _retry_what = RETRY_NONE;

/***** Small helpers *****/

static const char *_addr_str(const ble_gap_addr_t *a) {
    static char buf[24];
    sfmt(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x/%d",
         a->addr[5], a->addr[4], a->addr[3], a->addr[2], a->addr[1], a->addr[0], a->addr_type);
    return buf;
}

static int _addr_eq(const ble_gap_addr_t *a, const ble_gap_addr_t *b) {
    return a->addr_type == b->addr_type && memcmp(a->addr, b->addr, BLE_GAP_ADDR_LEN) == 0;
}

static const char *_gap_evt_names[] = {
    "CONNECTED", "DISCONNECTED", "CONN_PARAM_UPDATE", "SEC_PARAMS_REQUEST",
    "SEC_INFO_REQUEST", "PASSKEY_DISPLAY", "KEY_PRESSED", "AUTH_KEY_REQUEST",
    "LESC_DHKEY_REQUEST", "AUTH_STATUS", "CONN_SEC_UPDATE", "TIMEOUT",
    "RSSI_CHANGED", "ADV_REPORT", "SEC_REQUEST", "CONN_PARAM_UPDATE_REQUEST",
    "SCAN_REQ_REPORT", "PHY_UPDATE_REQUEST", "PHY_UPDATE",
    "DATA_LENGTH_UPDATE_REQUEST", "DATA_LENGTH_UPDATE",
    "QOS_CHANNEL_SURVEY_REPORT", "ADV_SET_TERMINATED",
};

static const char *_gattc_evt_names[] = {
    "PRIM_SRVC_DISC_RSP", "REL_DISC_RSP", "CHAR_DISC_RSP", "DESC_DISC_RSP",
    "ATTR_INFO_DISC_RSP", "CHAR_VAL_BY_UUID_READ_RSP", "READ_RSP",
    "CHAR_VALS_READ_RSP", "WRITE_RSP", "HVX", "EXCHANGE_MTU_RSP", "TIMEOUT",
    "WRITE_CMD_TX_COMPLETE",
};

static const char *_evt_name(uint16_t id) {
    if (id >= BLE_GAP_EVT_BASE && id < BLE_GAP_EVT_BASE + HID_ARRAY_SIZE(_gap_evt_names))
        return _gap_evt_names[id - BLE_GAP_EVT_BASE];
    if (id >= BLE_GATTC_EVT_BASE && id < BLE_GATTC_EVT_BASE + HID_ARRAY_SIZE(_gattc_evt_names))
        return _gattc_evt_names[id - BLE_GATTC_EVT_BASE];
    if (id >= BLE_GATTS_EVT_BASE && id <= BLE_GATTS_EVT_LAST)
        return "GATTS";
    return "?";
}

static void _set_state(enum hid_state st) {
    if (st == _hid_state)
        return;
    HID_LOG(APP_LOG_LEVEL_INFO, "state %s -> %s", _state_names[_hid_state], _state_names[st]);
    _hid_state = st;
}

/* Thread context: keyboard_link_state_changed() may only be called from a
 * thread, so every state report goes through the service thread. */
static void _svc_link_state(void *ctx) {
    KeyboardLinkState st = (KeyboardLinkState)(intptr_t)ctx;
    keyboard_link_state_changed(st, _hid_name[0] ? _hid_name : NULL);
}

static void _report_link_state(KeyboardLinkState st) {
    service_submit(_svc_link_state, (void *)(intptr_t)st, 0);
}

/* True when the bond carries a usable identity: an identity address type and
 * a non-zero IRK.  A keyboard that did not distribute an IRK leaves zeros. */
static int _bond_id_usable(void) {
    const ble_gap_id_key_t *id = &_bond.peer_id;
    if (id->id_addr_info.addr_type != BLE_GAP_ADDR_TYPE_PUBLIC &&
        id->id_addr_info.addr_type != BLE_GAP_ADDR_TYPE_RANDOM_STATIC)
        return 0;
    for (int i = 0; i < BLE_GAP_SEC_KEY_LEN; i++)
        if (id->id_info.irk[i])
            return 1;
    return 0;
}

static int _peer_matches_bond(const ble_gap_addr_t *peer) {
    if (!_bond.valid)
        return 0;
    if (_addr_eq(peer, &_bond.addr))
        return 1;
    /* A resolved private address is reported as the identity address. */
    if (peer->addr_id_peer && _bond_id_usable() && _addr_eq(peer, &_bond.peer_id.id_addr_info))
        return 1;
    return 0;
}

/***** Deferred work on the service thread *****/

static void _disc_step(void);
static void _reconnect_start(void);
static void _fail(const char *why);
static void _svc_drain_ring(void *ctx);

static void _svc_retry(void *ctx) {
    uint32_t gen = (uint32_t)(intptr_t)ctx;
    uint8_t what;
    int alive;
    ret_code_t rv;

    taskENTER_CRITICAL();
    alive = (gen == _hid_link_gen && _hid_conn != BLE_CONN_HANDLE_INVALID);
    what = _retry_what;
    _retry_what = RETRY_NONE;
    taskEXIT_CRITICAL();
    if (!alive || what == RETRY_NONE)
        return;

    HID_LOG(APP_LOG_LEVEL_INFO, "retrying step %d", what);
    switch (what) {
    case RETRY_ENCRYPT:
        rv = sd_ble_gap_encrypt(_hid_conn, &_bond.peer_enc.master_id, &_bond.peer_enc.enc_info);
        if (rv != NRF_SUCCESS)
            _fail("sd_ble_gap_encrypt retry failed");
        break;
    case RETRY_AUTHENTICATE:
        rv = sd_ble_gap_authenticate(_hid_conn, &_hid_sec_params);
        if (rv != NRF_SUCCESS)
            _fail("sd_ble_gap_authenticate retry failed");
        break;
    case RETRY_SEC_PARAMS_REPLY:
        rv = sd_ble_gap_sec_params_reply(_hid_conn, BLE_GAP_SEC_STATUS_SUCCESS, NULL, &_hid_keyset);
        if (rv != NRF_SUCCESS)
            _fail("sd_ble_gap_sec_params_reply retry failed");
        break;
    case RETRY_DISCOVERY:
        _disc_step();
        break;
    }
}

/* Schedule a retry of a SoftDevice call that returned NRF_ERROR_BUSY (the
 * same trick nrf52_bluetooth.c uses for the remote-name read). */
static void _defer_retry(enum hid_retry what) {
    _retry_what = what;
    service_submit(_svc_retry, (void *)(intptr_t)_hid_link_gen, pdMS_TO_TICKS(HID_RETRY_MS));
}

/* Discovery watchdog: a link that is not ready HID_WATCHDOG_MS after it
 * connected is dropped, whatever step it is stuck in. */
static void _svc_watchdog(void *ctx) {
    uint32_t gen = (uint32_t)(intptr_t)ctx;
    int stuck;
    uint16_t conn;

    taskENTER_CRITICAL();
    conn = _hid_conn;
    stuck = (gen == _hid_link_gen && conn != BLE_CONN_HANDLE_INVALID && _hid_state != HID_READY);
    if (stuck)
        _hid_reconnect_backoff = 1;
    taskEXIT_CRITICAL();
    if (!stuck)
        return;
    HID_LOG(APP_LOG_LEVEL_ERROR, "watchdog: link not ready after %d ms (state %s, discovery phase %d); disconnecting",
            HID_WATCHDOG_MS, _state_names[_hid_state], _disc_phase);
    (void) sd_ble_gap_disconnect(conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
}

static void _svc_reconnect(void *ctx) {
    uint32_t gen = (uint32_t)(intptr_t)ctx;
    if (gen != _hid_link_gen)
        return;
    _reconnect_start();
}

/* Bond persistence.  prefs_get/prefs_put block on the flash filesystem, so
 * both only ever run here, on the service thread.  flash_init runs before
 * bluetooth_init (rcore/rebbleos.c) and rcore/tz.c already reads prefs
 * earlier in boot, so prefs is usable by the time the load callback runs. */
static void _svc_bond_load(void *ctx) {
    struct hid_bond b;
    int n = prefs_get(PREFS_KEY_KEYBOARD_BOND, &b, sizeof(b));

    if (n == (int)sizeof(b) && b.valid) {
        taskENTER_CRITICAL();
        memcpy(&_bond, &b, sizeof(_bond));
        _bond.name[sizeof(_bond.name) - 1] = 0;
        strncpy(_hid_name, _bond.name, sizeof(_hid_name) - 1);
        _bond_loaded = 1;
        taskEXIT_CRITICAL();
        HID_LOG(APP_LOG_LEVEL_INFO, "loaded bond for \"%s\" at %s (identity %s)", _bond.name,
                _addr_str(&_bond.addr), _bond_id_usable() ? "yes" : "no");
        _reconnect_start();
    } else {
        _bond_loaded = 1;
        HID_LOG(APP_LOG_LEVEL_INFO, "no stored keyboard bond (prefs_get returned %d)", n);
    }
}

static void _svc_bond_store(void *ctx) {
    struct hid_bond b;

    taskENTER_CRITICAL();
    memcpy(&b, &_bond, sizeof(b));
    taskEXIT_CRITICAL();

    int rv = prefs_put(PREFS_KEY_KEYBOARD_BOND, &b, sizeof(b));
    if (rv < 0)
        HID_LOG(APP_LOG_LEVEL_ERROR, "prefs_put(keyboard bond) failed (%d)", rv);
    else
        HID_LOG(APP_LOG_LEVEL_INFO, "bond record %s", b.valid ? "stored" : "cleared");
}

/***** Input report ring *****/

/* Event context. */
static void _ring_push(const uint8_t *data, uint16_t len) {
    if (len > HID_REPORT_MAX)
        len = HID_REPORT_MAX;
    if (_ring_count == HID_RING_SLOTS) {
        _ring_tail = (_ring_tail + 1) % HID_RING_SLOTS;
        _ring_count--;
        _ring_dropped++;
        HID_LOG(APP_LOG_LEVEL_WARNING, "report ring full; dropped the oldest report (%d dropped so far)", _ring_dropped);
    }
    struct hid_slot *slot = &_ring[_ring_head];
    memcpy(slot->data, data, len);
    slot->len = len;
    _ring_head = (_ring_head + 1) % HID_RING_SLOTS;
    _ring_count++;
    if (!_ring_drain_pending) {
        _ring_drain_pending = 1;
        service_submit(_svc_drain_ring, NULL, 0);
    }
}

/* Service thread: deliver queued reports to the core in order. */
static void _svc_drain_ring(void *ctx) {
    struct hid_slot slot;

    for (;;) {
        taskENTER_CRITICAL();
        if (_ring_count == 0) {
            _ring_drain_pending = 0;
            taskEXIT_CRITICAL();
            return;
        }
        memcpy(&slot, &_ring[_ring_tail], sizeof(slot));
        _ring_tail = (_ring_tail + 1) % HID_RING_SLOTS;
        _ring_count--;
        taskEXIT_CRITICAL();
        keyboard_hid_boot_report(slot.data, slot.len);
    }
}

/***** Link teardown *****/

static void _link_reset(void) {
    _hid_conn = BLE_CONN_HANDLE_INVALID;
    _hid_pairing_requested = 0;
    _hid_encrypt_fallback_used = 0;
    _hid_encrypted = 0;
    _hid_auth_done = 0;
    _hid_connect_is_pairing = 0;
    _retry_what = RETRY_NONE;
    _disc_phase = DISC_NONE;
    _proto_mode_hnd = BLE_GATT_HANDLE_INVALID;
    _n_inputs = 0;
    _n_report_chars = 0;
    _last_input = NULL;
    _use_boot = 0;
}

/* Something on this link went wrong: log, drop the link, and let
 * BLE_GAP_EVT_DISCONNECTED do the bookkeeping.  Never asserts: the air is
 * not under our control. */
static void _fail(const char *why) {
    HID_LOG(APP_LOG_LEVEL_ERROR, "%s (state %s); disconnecting", why, _state_names[_hid_state]);
    _hid_reconnect_backoff = 1;
    if (_hid_conn != BLE_CONN_HANDLE_INVALID) {
        ret_code_t rv = sd_ble_gap_disconnect(_hid_conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        if (rv != NRF_SUCCESS)
            HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_disconnect failed (%d)", rv);
    }
}

/***** Scanning *****/

/* Start a fresh pairing scan.  Returns 0 on success. */
static int _scan_start(void) {
    ret_code_t rv = sd_ble_gap_scan_start(&_scan_params_pair, &_scan_buf);
    if (rv != NRF_SUCCESS) {
        HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_scan_start failed (%d)", rv);
        return -1;
    }
    _cand_valid = 0;
    _cand_reports = 0;
    _scan_reports_seen = 0;
    _hid_name[0] = 0;
    _set_state(HID_SCANNING);
    _report_link_state(KeyboardLinkScanning);
    return 0;
}

/* S140: after each BLE_GAP_EVT_ADV_REPORT (data status complete) the
 * scanner is paused, and the application must call
 * sd_ble_gap_scan_start(NULL, &buf) to continue. */
static void _scan_continue(void) {
    ret_code_t rv = sd_ble_gap_scan_start(NULL, &_scan_buf);
    if (rv == NRF_SUCCESS)
        return;
    /* NRF_ERROR_INVALID_STATE here means the scanner already timed out;
     * BLE_GAP_EVT_TIMEOUT follows and finishes the job. */
    if (rv != NRF_ERROR_INVALID_STATE) {
        HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_scan_start(continue) failed (%d)", rv);
        _set_state(HID_IDLE);
        _report_link_state(KeyboardLinkDisconnected);
    }
}

static int _adv_find_name(const uint8_t *data, uint16_t len, char *out, size_t outsz) {
    uint16_t offset = 0;
    uint16_t n = ble_advdata_search(data, len, &offset, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME);
    if (n == 0) {
        offset = 0;
        n = ble_advdata_search(data, len, &offset, BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME);
    }
    if (n == 0)
        return 0;
    if (n > outsz - 1)
        n = outsz - 1;
    memcpy(out, data + offset, n);
    out[n] = 0;
    return 1;
}

static int _adv_find_appearance(const uint8_t *data, uint16_t len, uint16_t *out) {
    uint16_t offset = 0;
    uint16_t n = ble_advdata_search(data, len, &offset, BLE_GAP_AD_TYPE_APPEARANCE);
    if (n < 2)
        return 0;
    *out = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
    return 1;
}

/* Issue sd_ble_gap_connect.  S140: this stops any running scanner, even
 * when the call fails. */
static ret_code_t _connect_to(const ble_gap_addr_t *addr, const ble_gap_scan_params_t *sp, int pairing) {
    ret_code_t rv = sd_ble_gap_connect(addr, sp, &_conn_params, CONN_TAG);
    if (rv != NRF_SUCCESS) {
        HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_connect(%s) failed (%d)", _addr_str(addr), rv);
        return rv;
    }
    HID_LOG(APP_LOG_LEVEL_INFO, "%s to %s", pairing ? "connecting" : "background reconnect pending", _addr_str(addr));
    _hid_connect_is_pairing = pairing;
    _set_state(HID_CONNECTING);
    if (pairing)
        _report_link_state(KeyboardLinkConnecting);
    return NRF_SUCCESS;
}

static void _on_adv_report(const ble_gap_evt_adv_report_t *r) {
    static const ble_uuid_t hid_uuid = { .uuid = BLE_UUID_HUMAN_INTERFACE_DEVICE_SERVICE, .type = BLE_UUID_TYPE_BLE };
    const uint8_t *data = r->data.p_data;
    uint16_t len = r->data.len;
    char name[KEYBOARD_NAME_MAX];
    int have_name;
    int is_cand = 0;

    if (_hid_state != HID_SCANNING)
        return;  /* a report that raced our connect / stop; the buffer is released, nothing to continue */

    _scan_reports_seen++;
    have_name = _adv_find_name(data, len, name, sizeof(name));

    if (_cand_valid && _addr_eq(&r->peer_addr, &_cand_addr)) {
        is_cand = 1;
    } else if (r->type.connectable || r->type.scan_response) {
        uint16_t appearance = 0;
        int have_app = _adv_find_appearance(data, len, &appearance);
        int has_hid_uuid = ble_advdata_uuid_find(data, len, &hid_uuid);
        int other_hid = have_app &&
                        (appearance & HID_APPEARANCE_CATEGORY_MASK) == HID_APPEARANCE_CATEGORY &&
                        appearance != BLE_APPEARANCE_HID_KEYBOARD &&
                        appearance != BLE_APPEARANCE_GENERIC_HID;
        if (other_hid) {
            HID_LOG(APP_LOG_LEVEL_INFO, "skipping %s: HID appearance %d is not a keyboard", _addr_str(&r->peer_addr), appearance);
        } else if (has_hid_uuid || (have_app && appearance == BLE_APPEARANCE_HID_KEYBOARD)) {
            is_cand = 1;
            _cand_addr = r->peer_addr;
            _cand_valid = 1;
            _cand_reports = 0;
            _hid_name[0] = 0;
            HID_LOG(APP_LOG_LEVEL_INFO, "candidate %s rssi %d (hid uuid %d, appearance %d, %s)",
                    _addr_str(&r->peer_addr), r->rssi, has_hid_uuid, have_app ? appearance : -1,
                    r->type.scan_response ? "scan rsp" : "adv");
        }
    }

    if (!is_cand) {
        _scan_continue();
        return;
    }

    _cand_reports++;
    if (have_name) {
        strncpy(_hid_name, name, sizeof(_hid_name) - 1);
        _hid_name[sizeof(_hid_name) - 1] = 0;
        HID_LOG(APP_LOG_LEVEL_INFO, "candidate name \"%s\"", _hid_name);
    }

    /* Names usually travel in the scan response, which our active scan
     * requests; give the candidate one more report to deliver it unless it
     * is not scannable at all. */
    if (!have_name && r->type.scannable && _cand_reports < 2) {
        _scan_continue();
        return;
    }

    if (_connect_to(&_cand_addr, &_scan_params_connect, 1) != NRF_SUCCESS) {
        /* The scanner is stopped even on failure: start over, not continue. */
        _set_state(HID_IDLE);
        if (_scan_start() != 0)
            _report_link_state(KeyboardLinkDisconnected);
    }
}

static void _on_timeout(uint8_t src) {
    if (src == BLE_GAP_TIMEOUT_SRC_SCAN) {
        HID_LOG(APP_LOG_LEVEL_INFO, "pairing scan timed out after %d reports, no keyboard found", _scan_reports_seen);
    } else if (src == BLE_GAP_TIMEOUT_SRC_CONN) {
        HID_LOG(APP_LOG_LEVEL_INFO, "connect timed out (%s)", _hid_connect_is_pairing ? "pairing" : "reconnect");
    } else {
        HID_LOG(APP_LOG_LEVEL_INFO, "timeout source %d", src);
        return;
    }
    _set_state(HID_IDLE);
    _hid_connect_is_pairing = 0;
    _report_link_state(KeyboardLinkDisconnected);
    _reconnect_start();
}

/***** Background reconnect *****/

/* Issue a pending connect to the bonded keyboard.  Runs from the event
 * handler (after a disconnect), the service thread (after the bond loads,
 * or delayed after a failure) and app threads (hw_keyboard_connect). */
static void _reconnect_start(void) {
    const ble_gap_addr_t *target;
    ret_code_t rv;

    if (!_bond.valid || _hid_conn != BLE_CONN_HANDLE_INVALID || _hid_state != HID_IDLE)
        return;

    if (_hid_reconnect_backoff) {
        _hid_reconnect_backoff = 0;
        HID_LOG(APP_LOG_LEVEL_INFO, "last link failed; reconnecting in %d ms", HID_FAIL_BACKOFF_MS);
        service_submit(_svc_reconnect, (void *)(intptr_t)_hid_link_gen, pdMS_TO_TICKS(HID_FAIL_BACKOFF_MS));
        return;
    }

    target = &_bond.addr;
    if (_bond_id_usable() && !_addr_eq(&_bond.addr, &_bond.peer_id.id_addr_info)) {
        /* The keyboard connected with a resolvable private address and
         * distributed its IRK and identity address.  S140
         * (sd_ble_gap_device_identities_set, ble_gap_addr_t::addr_id_peer):
         * with the peer's IRK in the device identity list the SoftDevice
         * resolves the private addresses it hears, and sd_ble_gap_connect
         * takes the peer *identity* address; without the list entry a
         * connect to the identity address is rejected with
         * NRF_ERROR_INVALID_PARAM.  We rely on the list not being "in use"
         * by our advertiser: privacy is off and it uses no whitelist. */
        _id_list[0] = &_bond.peer_id;
        rv = sd_ble_gap_device_identities_set(_id_list, NULL, 1);
        if (rv == NRF_SUCCESS) {
            target = &_bond.peer_id.id_addr_info;
        } else {
            HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_device_identities_set failed (%d); using the last connection address", rv);
        }
    }

    rv = _connect_to(target, &_scan_params_reconnect, 0);
    if (rv == NRF_ERROR_INVALID_STATE) {
        /* An initiator is already pending (for example a connect issued
         * from another context a moment ago); nothing to do. */
        _set_state(HID_CONNECTING);
    }
}

/***** Security *****/

static void _start_authenticate(void) {
    memset(&_keys_own_enc, 0, sizeof(_keys_own_enc));
    memset(&_keys_peer_enc, 0, sizeof(_keys_peer_enc));
    memset(&_keys_own_id, 0, sizeof(_keys_own_id));
    memset(&_keys_peer_id, 0, sizeof(_keys_peer_id));
    _hid_pairing_requested = 1;
    _hid_auth_done = 0;
    /* S140: in the central role sd_ble_gap_authenticate sends the SMP
     * Pairing Request; BLE_GAP_EVT_SEC_PARAMS_REQUEST then asks for the key
     * storage. */
    ret_code_t rv = sd_ble_gap_authenticate(_hid_conn, &_hid_sec_params);
    HID_LOG(APP_LOG_LEVEL_INFO, "pairing (Just Works): sd_ble_gap_authenticate -> %d", rv);
    if (rv == NRF_ERROR_BUSY)
        _defer_retry(RETRY_AUTHENTICATE);
    else if (rv != NRF_SUCCESS)
        _fail("sd_ble_gap_authenticate failed");
}

static void _start_encrypt(void) {
    /* S140: sd_ble_gap_encrypt takes the master id and encryption info the
     * peripheral distributed when we bonded (keys_peer.p_enc_key). */
    ret_code_t rv = sd_ble_gap_encrypt(_hid_conn, &_bond.peer_enc.master_id, &_bond.peer_enc.enc_info);
    HID_LOG(APP_LOG_LEVEL_INFO, "encrypting with the stored bond: sd_ble_gap_encrypt -> %d", rv);
    if (rv == NRF_SUCCESS)
        return;
    if (rv == NRF_ERROR_BUSY) {
        _defer_retry(RETRY_ENCRYPT);
        return;
    }
    HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_encrypt failed (%d); pairing instead", rv);
    _hid_encrypt_fallback_used = 1;
    _start_authenticate();
}

static void _maybe_start_discovery(void) {
    if (_hid_state != HID_PAIRING || !_hid_encrypted)
        return;
    /* When we paired on this link, wait for BLE_GAP_EVT_AUTH_STATUS so the
     * SMP key distribution is over before ATT traffic starts. */
    if (_hid_pairing_requested && !_hid_auth_done)
        return;
    _set_state(HID_DISCOVERING);
    _disc_phase = DISC_SERVICE;
    _n_inputs = 0;
    _n_report_chars = 0;
    _last_input = NULL;
    _proto_mode_hnd = BLE_GATT_HANDLE_INVALID;
    _disc_step();
}

static void _on_sec_params_request(const ble_gap_evt_sec_params_request_t *req) {
    HID_LOG(APP_LOG_LEVEL_INFO, "peer sec params: bond %d mitm %d lesc %d io_caps %d keys %d..%d",
            req->peer_params.bond, req->peer_params.mitm, req->peer_params.lesc, req->peer_params.io_caps,
            req->peer_params.min_key_size, req->peer_params.max_key_size);
    /* S140: in the central role p_sec_params must be NULL here (they were
     * given to sd_ble_gap_authenticate); the keyset supplies the memory the
     * SoftDevice fills with the exchanged keys. */
    ret_code_t rv = sd_ble_gap_sec_params_reply(_hid_conn, BLE_GAP_SEC_STATUS_SUCCESS, NULL, &_hid_keyset);
    if (rv == NRF_SUCCESS)
        return;
    if (rv == NRF_ERROR_BUSY) {
        /* S140: the request stays pending and the reply may be repeated. */
        _defer_retry(RETRY_SEC_PARAMS_REPLY);
        return;
    }
    HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_sec_params_reply failed (%d)", rv);
    _fail("could not reply to the security parameters request");
}

static void _on_auth_status(const ble_gap_evt_auth_status_t *as) {
    HID_LOG(APP_LOG_LEVEL_INFO, "auth status 0x%02x src %d bonded %d lesc %d kdist own enc %d id %d peer enc %d id %d",
            as->auth_status, as->error_src, as->bonded, as->lesc,
            as->kdist_own.enc, as->kdist_own.id, as->kdist_peer.enc, as->kdist_peer.id);
    if (as->auth_status != BLE_GAP_SEC_STATUS_SUCCESS) {
        _fail("pairing failed");
        return;
    }
    _hid_auth_done = 1;
    if (as->bonded) {
        if (!as->kdist_peer.enc)
            HID_LOG(APP_LOG_LEVEL_WARNING, "keyboard did not distribute its LTK; reconnects will need to pair again");
        /* The record is only ever written here (event context) and read by
         * the service thread under a critical section that masks us. */
        _bond.valid = 1;
        _bond.addr = _hid_peer_addr;
        _bond.peer_enc = _keys_peer_enc;
        if (as->kdist_peer.id)
            _bond.peer_id = _keys_peer_id;
        else
            memset(&_bond.peer_id, 0, sizeof(_bond.peer_id));
        _bond.own_enc = _keys_own_enc;
        strncpy(_bond.name, _hid_name[0] ? _hid_name : "Keyboard", sizeof(_bond.name) - 1);
        _bond.name[sizeof(_bond.name) - 1] = 0;
        HID_LOG(APP_LOG_LEVEL_INFO, "bonded with \"%s\" at %s: peer ediv %04x, ltk len %d",
                _bond.name, _addr_str(&_bond.addr), _bond.peer_enc.master_id.ediv,
                _bond.peer_enc.enc_info.ltk_len);
        if (_bond_id_usable())
            HID_LOG(APP_LOG_LEVEL_INFO, "keyboard identity address %s", _addr_str(&_bond.peer_id.id_addr_info));
        service_submit(_svc_bond_store, NULL, 0);
    }
    _maybe_start_discovery();
}

static void _on_conn_sec_update(const ble_gap_evt_conn_sec_update_t *cs) {
    HID_LOG(APP_LOG_LEVEL_INFO, "conn sec update: mode %d level %d key size %d",
            cs->conn_sec.sec_mode.sm, cs->conn_sec.sec_mode.lv, cs->conn_sec.encr_key_size);
    if (cs->conn_sec.sec_mode.lv >= 2) {
        _hid_encrypted = 1;
        _maybe_start_discovery();
        return;
    }
    if (_hid_state != HID_PAIRING)
        return;  /* a security change on a link that is already past pairing; nothing to decide */
    /* Level 1 after our sd_ble_gap_encrypt means the keyboard rejected the
     * stored key (the Peer Manager reports this event pattern as
     * PIN_OR_KEY_MISSING).  Pair once more, then give up. */
    if (!_hid_pairing_requested && !_hid_encrypt_fallback_used) {
        HID_LOG(APP_LOG_LEVEL_WARNING, "keyboard rejected the stored key; pairing again");
        _hid_encrypt_fallback_used = 1;
        _start_authenticate();
        return;
    }
    _fail("link is not encrypted");
}

static void _on_sec_request(const ble_gap_evt_sec_request_t *sr) {
    HID_LOG(APP_LOG_LEVEL_INFO, "security request: bond %d mitm %d lesc %d", sr->bond, sr->mitm, sr->lesc);
    if (_hid_encrypted)
        return;  /* already secure; the SoftDevice rejects a second procedure anyway */
    if (_hid_pairing_requested)
        return;  /* our pairing is in flight; the request is satisfied by it */
    if (_peer_matches_bond(&_hid_peer_addr))
        _start_encrypt();
    else
        _start_authenticate();
}

/***** GATT discovery *****/

static struct hid_input *_input_add(uint8_t kind, const ble_gattc_char_t *c) {
    if (_n_inputs >= HID_MAX_INPUTS)
        return NULL;
    struct hid_input *in = &_inputs[_n_inputs++];
    memset(in, 0, sizeof(*in));
    in->kind = kind;
    in->notify = c->char_props.notify;
    in->decl = c->handle_decl;
    in->value = c->handle_value;
    in->desc_end = 0;
    in->cccd = BLE_GATT_HANDLE_INVALID;
    in->ref = BLE_GATT_HANDLE_INVALID;
    return in;
}

/* Choose what to subscribe to once descriptors and report references are known. */
static void _disc_choose(void) {
    struct hid_input *boot = NULL;
    int n_sub = 0;

    for (int i = 0; i < _n_inputs; i++) {
        struct hid_input *in = &_inputs[i];
        HID_LOG(APP_LOG_LEVEL_INFO, "input %d: %s value %04x cccd %04x ref %04x id %d type %d notify %d",
                i, in->kind == INPUT_BOOT_KEYBOARD ? "boot kbd" : "report", in->value, in->cccd, in->ref,
                in->report_id, in->report_type, in->notify);
        if (in->kind == INPUT_BOOT_KEYBOARD && in->notify && in->cccd != BLE_GATT_HANDLE_INVALID)
            boot = in;
    }

    if (boot && _proto_mode_hnd != BLE_GATT_HANDLE_INVALID) {
        HID_LOG(APP_LOG_LEVEL_INFO, "using boot protocol (protocol mode %04x, boot input %04x)", _proto_mode_hnd, boot->value);
        _use_boot = 1;
        _disc_idx = boot - _inputs;
        _disc_phase = DISC_PROTO;
        return;
    }

    for (int i = 0; i < _n_inputs; i++) {
        struct hid_input *in = &_inputs[i];
        if (in->kind == INPUT_REPORT && in->notify && in->cccd != BLE_GATT_HANDLE_INVALID &&
            in->report_type == HID_REPORT_TYPE_INPUT)
            n_sub++;
    }
    if (n_sub == 0) {
        _fail("no usable input report (no boot keyboard input with protocol mode, no input Report characteristic)");
        return;
    }
    HID_LOG(APP_LOG_LEVEL_INFO, "using report protocol: %d input report(s)", n_sub);
    _use_boot = 0;
    _disc_idx = 0;
    _disc_phase = DISC_CCCD;
}

static int _input_wants_cccd(const struct hid_input *in) {
    if (in->cccd == BLE_GATT_HANDLE_INVALID || !in->notify)
        return 0;
    if (_use_boot)
        return in->kind == INPUT_BOOT_KEYBOARD;
    return in->kind == INPUT_REPORT && in->report_type == HID_REPORT_TYPE_INPUT;
}

static void _disc_ready(void) {
    _disc_phase = DISC_DONE;
    _hid_reconnect_backoff = 0;
    _set_state(HID_READY);
    HID_LOG(APP_LOG_LEVEL_INFO, "keyboard \"%s\" ready", _hid_name[0] ? _hid_name : "(unnamed)");
    _report_link_state(KeyboardLinkConnected);
}

/* Issue the SoftDevice call for the current discovery phase.  One GATT
 * client procedure is outstanding at a time; every response event calls
 * back here for the next one.  State that describes the outstanding call is
 * set before the call, so a response that arrives (in event context) before
 * a thread-context caller returns sees the right phase.  Re-entrant: a busy
 * SoftDevice is retried from the service thread HID_RETRY_MS later. */
static void _disc_step(void) {
    ret_code_t rv = NRF_SUCCESS;
    static const uint8_t cccd_notify[2] = { BLE_GATT_HVX_NOTIFICATION, 0x00 };
    static const uint8_t protocol_mode_boot = HID_PROTOCOL_MODE_BOOT;

    if (_hid_conn == BLE_CONN_HANDLE_INVALID)
        return;

    switch (_disc_phase) {
    case DISC_SERVICE: {
        ble_uuid_t uuid = { .uuid = BLE_UUID_HUMAN_INTERFACE_DEVICE_SERVICE, .type = BLE_UUID_TYPE_BLE };
        rv = sd_ble_gattc_primary_services_discover(_hid_conn, BLE_GATT_HANDLE_START, &uuid);
        break;
    }
    case DISC_CHARS: {
        ble_gattc_handle_range_t range = { .start_handle = _char_next, .end_handle = _svc_range.end_handle };
        rv = sd_ble_gattc_characteristics_discover(_hid_conn, &range);
        break;
    }
    case DISC_DESCS: {
        struct hid_input *in = NULL;
        /* Advance to an input with descriptor handles still to search. */
        while (_disc_idx < _n_inputs) {
            in = &_inputs[_disc_idx];
            if (_desc_next == 0)
                _desc_next = in->value + 1;
            if (_desc_next <= in->desc_end)
                break;
            _disc_idx++;
            _desc_next = 0;
            in = NULL;
        }
        if (!in) {
            _disc_phase = DISC_REFS;
            _disc_idx = 0;
            _disc_step();
            return;
        }
        ble_gattc_handle_range_t range = { .start_handle = _desc_next, .end_handle = in->desc_end };
        rv = sd_ble_gattc_descriptors_discover(_hid_conn, &range);
        break;
    }
    case DISC_REFS: {
        while (_disc_idx < _n_inputs &&
               (_inputs[_disc_idx].kind != INPUT_REPORT || _inputs[_disc_idx].ref == BLE_GATT_HANDLE_INVALID))
            _disc_idx++;
        if (_disc_idx >= _n_inputs) {
            _disc_choose();
            if (_disc_phase == DISC_PROTO || _disc_phase == DISC_CCCD)
                _disc_step();
            return;
        }
        rv = sd_ble_gattc_read(_hid_conn, _inputs[_disc_idx].ref, 0);
        break;
    }
    case DISC_PROTO: {
        /* HOGP boot host: Protocol Mode is written with a Write Command; the
         * only completion event is BLE_GATTC_EVT_WRITE_CMD_TX_COMPLETE, which
         * we wait for so the CCCD write request follows it on air. */
        ble_gattc_write_params_t w = {
            .write_op = BLE_GATT_OP_WRITE_CMD,
            .flags = 0,
            .handle = _proto_mode_hnd,
            .offset = 0,
            .len = sizeof(protocol_mode_boot),
            .p_value = &protocol_mode_boot,
        };
        _disc_phase = DISC_PROTO_WAIT;
        _set_state(HID_SUBSCRIBING);
        rv = sd_ble_gattc_write(_hid_conn, &w);
        if (rv != NRF_SUCCESS)
            _disc_phase = DISC_PROTO;
        break;
    }
    case DISC_PROTO_WAIT:
        return;  /* waiting for the write command to leave the radio */
    case DISC_CCCD: {
        while (_disc_idx < _n_inputs && !_input_wants_cccd(&_inputs[_disc_idx]))
            _disc_idx++;
        if (_disc_idx >= _n_inputs) {
            _disc_ready();
            return;
        }
        ble_gattc_write_params_t w = {
            .write_op = BLE_GATT_OP_WRITE_REQ,
            .flags = 0,
            .handle = _inputs[_disc_idx].cccd,
            .offset = 0,
            .len = sizeof(cccd_notify),
            .p_value = cccd_notify,
        };
        _set_state(HID_SUBSCRIBING);
        rv = sd_ble_gattc_write(_hid_conn, &w);
        break;
    }
    case DISC_NONE:
    case DISC_DONE:
    default:
        return;
    }

    if (rv == NRF_SUCCESS)
        return;
    if (rv == NRF_ERROR_BUSY || rv == NRF_ERROR_RESOURCES) {
        HID_LOG(APP_LOG_LEVEL_INFO, "discovery phase %d: SoftDevice busy (%d), retrying in %d ms", _disc_phase, rv, HID_RETRY_MS);
        _defer_retry(RETRY_DISCOVERY);
        return;
    }
    HID_LOG(APP_LOG_LEVEL_ERROR, "discovery phase %d: SoftDevice call failed (%d)", _disc_phase, rv);
    _fail("GATT discovery call failed");
}

static void _on_prim_srvc_disc_rsp(const ble_gattc_evt_t *ev) {
    if (_disc_phase != DISC_SERVICE)
        return;
    const ble_gattc_evt_prim_srvc_disc_rsp_t *rsp = &ev->params.prim_srvc_disc_rsp;
    if (ev->gatt_status != BLE_GATT_STATUS_SUCCESS || rsp->count == 0) {
        HID_LOG(APP_LOG_LEVEL_ERROR, "no HID service (gatt status 0x%04x, count %d)", ev->gatt_status,
                ev->gatt_status == BLE_GATT_STATUS_SUCCESS ? rsp->count : 0);
        _fail("keyboard has no HID service");
        return;
    }
    _svc_range = rsp->services[0].handle_range;
    HID_LOG(APP_LOG_LEVEL_INFO, "HID service handles %04x..%04x", _svc_range.start_handle, _svc_range.end_handle);
    _char_next = _svc_range.start_handle;
    _disc_phase = DISC_CHARS;
    _disc_step();
}

static void _on_char_disc_rsp(const ble_gattc_evt_t *ev) {
    if (_disc_phase != DISC_CHARS)
        return;
    const ble_gattc_evt_char_disc_rsp_t *rsp = &ev->params.char_disc_rsp;
    int done = 0;

    if (ev->gatt_status == BLE_GATT_STATUS_SUCCESS) {
        uint16_t last_value = 0;
        for (int i = 0; i < rsp->count; i++) {
            const ble_gattc_char_t *c = &rsp->chars[i];
            /* The previous input's descriptors end just before this declaration. */
            if (_last_input && _last_input->desc_end == 0)
                _last_input->desc_end = c->handle_decl - 1;
            _last_input = NULL;
            last_value = c->handle_value;

            uint16_t uuid = (c->uuid.type == BLE_UUID_TYPE_BLE) ? c->uuid.uuid : 0;
            HID_LOG(APP_LOG_LEVEL_INFO, "char uuid %04x (type %d) decl %04x value %04x props r%d w%d wnr%d n%d",
                    uuid, c->uuid.type, c->handle_decl, c->handle_value,
                    c->char_props.read, c->char_props.write, c->char_props.write_wo_resp, c->char_props.notify);
            switch (uuid) {
            case BLE_UUID_PROTOCOL_MODE_CHAR:
                _proto_mode_hnd = c->handle_value;
                break;
            case BLE_UUID_BOOT_KEYBOARD_INPUT_REPORT_CHAR:
                if (c->char_props.notify)
                    _last_input = _input_add(INPUT_BOOT_KEYBOARD, c);
                break;
            case BLE_UUID_REPORT_CHAR:
                if (c->char_props.notify && _n_report_chars < HID_MAX_REPORT_CHARS) {
                    _last_input = _input_add(INPUT_REPORT, c);
                    if (_last_input)
                        _n_report_chars++;
                }
                break;
            default:
                break;
            }
        }
        if (rsp->count == 0 || last_value >= _svc_range.end_handle)
            done = 1;
        else
            _char_next = last_value + 1;
    } else if (ev->gatt_status == BLE_GATT_STATUS_ATTERR_ATTRIBUTE_NOT_FOUND) {
        done = 1;
    } else {
        HID_LOG(APP_LOG_LEVEL_ERROR, "characteristic discovery failed, gatt status 0x%04x", ev->gatt_status);
        _fail("characteristic discovery failed");
        return;
    }

    if (!done) {
        _disc_step();
        return;
    }
    if (_last_input && _last_input->desc_end == 0)
        _last_input->desc_end = _svc_range.end_handle;
    _last_input = NULL;
    HID_LOG(APP_LOG_LEVEL_INFO, "%d input characteristic(s), protocol mode %04x", _n_inputs, _proto_mode_hnd);
    if (_n_inputs == 0) {
        _fail("HID service has no notifying input characteristic");
        return;
    }
    _disc_phase = DISC_DESCS;
    _disc_idx = 0;
    _desc_next = 0;
    _disc_step();
}

static void _on_desc_disc_rsp(const ble_gattc_evt_t *ev) {
    if (_disc_phase != DISC_DESCS || _disc_idx >= _n_inputs)
        return;
    struct hid_input *in = &_inputs[_disc_idx];
    const ble_gattc_evt_desc_disc_rsp_t *rsp = &ev->params.desc_disc_rsp;

    if (ev->gatt_status == BLE_GATT_STATUS_SUCCESS && rsp->count > 0) {
        uint16_t last = _desc_next;
        for (int i = 0; i < rsp->count; i++) {
            const ble_gattc_desc_t *d = &rsp->descs[i];
            last = d->handle;
            if (d->uuid.type != BLE_UUID_TYPE_BLE)
                continue;
            if (d->uuid.uuid == BLE_UUID_DESCRIPTOR_CLIENT_CHAR_CONFIG)
                in->cccd = d->handle;
            else if (d->uuid.uuid == BLE_UUID_REPORT_REF_DESCR)
                in->ref = d->handle;
        }
        _desc_next = last + 1;
    } else {
        /* Attribute-not-found (or any error) ends this characteristic's range. */
        if (ev->gatt_status != BLE_GATT_STATUS_SUCCESS && ev->gatt_status != BLE_GATT_STATUS_ATTERR_ATTRIBUTE_NOT_FOUND)
            HID_LOG(APP_LOG_LEVEL_WARNING, "descriptor discovery for value %04x: gatt status 0x%04x", in->value, ev->gatt_status);
        _desc_next = in->desc_end + 1;
    }
    _disc_step();
}

static void _on_read_rsp(const ble_gattc_evt_t *ev) {
    if (_disc_phase != DISC_REFS || _disc_idx >= _n_inputs)
        return;
    struct hid_input *in = &_inputs[_disc_idx];
    const ble_gattc_evt_read_rsp_t *rsp = &ev->params.read_rsp;

    if (ev->gatt_status == BLE_GATT_STATUS_SUCCESS && rsp->handle == in->ref && rsp->len >= 2) {
        in->report_id = rsp->data[0];
        in->report_type = rsp->data[1];
    } else {
        HID_LOG(APP_LOG_LEVEL_WARNING, "report reference %04x read: gatt status 0x%04x len %d", in->ref, ev->gatt_status,
                ev->gatt_status == BLE_GATT_STATUS_SUCCESS ? rsp->len : 0);
        in->report_type = 0;
    }
    _disc_idx++;
    _disc_step();
}

static void _on_write_rsp(const ble_gattc_evt_t *ev) {
    if (_disc_phase != DISC_CCCD || _disc_idx >= _n_inputs)
        return;
    struct hid_input *in = &_inputs[_disc_idx];
    const ble_gattc_evt_write_rsp_t *rsp = &ev->params.write_rsp;

    if (ev->gatt_status != BLE_GATT_STATUS_SUCCESS) {
        HID_LOG(APP_LOG_LEVEL_ERROR, "CCCD %04x write rejected, gatt status 0x%04x", in->cccd, ev->gatt_status);
        _fail("keyboard rejected the notification subscription");
        return;
    }
    if (rsp->handle != in->cccd)
        HID_LOG(APP_LOG_LEVEL_WARNING, "write response for %04x, expected CCCD %04x", rsp->handle, in->cccd);
    in->subscribed = 1;
    HID_LOG(APP_LOG_LEVEL_INFO, "subscribed to %s value %04x (report id %d)",
            in->kind == INPUT_BOOT_KEYBOARD ? "boot keyboard input" : "input report", in->value, in->report_id);
    _disc_idx++;
    _disc_step();
}

static void _on_hvx(const ble_gattc_evt_t *ev) {
    const ble_gattc_evt_hvx_t *hvx = &ev->params.hvx;

    if (hvx->type == BLE_GATT_HVX_INDICATION) {
        /* HID uses notifications; confirm a stray indication so the peer
         * does not stall, then ignore it. */
        (void) sd_ble_gattc_hv_confirm(_hid_conn, hvx->handle);
        return;
    }
    for (int i = 0; i < _n_inputs; i++) {
        if (_inputs[i].subscribed && _inputs[i].value == hvx->handle) {
            _ring_push(hvx->data, hvx->len);
            return;
        }
    }
    HID_LOG(APP_LOG_LEVEL_INFO, "notification from unsubscribed handle %04x (%d bytes)", hvx->handle, hvx->len);
}

/***** Connection events *****/

static void _on_connected(const ble_gap_evt_t *gap) {
    const ble_gap_evt_connected_t *c = &gap->params.connected;

    if (_hid_conn != BLE_CONN_HANDLE_INVALID) {
        /* Cannot happen with one central link configured; keep the first. */
        HID_LOG(APP_LOG_LEVEL_ERROR, "second central link %d while %d is up; dropping it", gap->conn_handle, _hid_conn);
        (void) sd_ble_gap_disconnect(gap->conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        return;
    }
    if (_hid_state == HID_SCANNING) {
        /* A background reconnect completed just as hw_keyboard_pair_start()
         * cancelled it; the user wants a new keyboard, so drop this link
         * and keep scanning.  This also happens if the bonded keyboard
         * itself wakes up mid-scan. */
        HID_LOG(APP_LOG_LEVEL_INFO, "connection from %s during a pairing scan; dropping it", _addr_str(&c->peer_addr));
        (void) sd_ble_gap_disconnect(gap->conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        return;
    }

    int pairing = _hid_connect_is_pairing;
    _link_reset();
    _hid_conn = gap->conn_handle;
    _hid_link_gen++;
    _hid_peer_addr = c->peer_addr;
    _hid_connect_is_pairing = pairing;

    int bonded = _peer_matches_bond(&c->peer_addr);
    if (bonded && !_hid_name[0])
        strncpy(_hid_name, _bond.name, sizeof(_hid_name) - 1);

    HID_LOG(APP_LOG_LEVEL_INFO, "connected (handle %d) to %s%s, interval %d x 1.25 ms, %s",
            _hid_conn, _addr_str(&c->peer_addr), c->peer_addr.addr_id_peer ? " (resolved)" : "",
            c->conn_params.max_conn_interval, bonded ? "matches the stored bond" : "no bond");

    _set_state(HID_PAIRING);
    if (!pairing)
        _report_link_state(KeyboardLinkConnecting);
    service_submit(_svc_watchdog, (void *)(intptr_t)_hid_link_gen, pdMS_TO_TICKS(HID_WATCHDOG_MS));

    if (bonded)
        _start_encrypt();
    else
        _start_authenticate();
}

static void _on_disconnected(const ble_gap_evt_t *gap) {
    HID_LOG(APP_LOG_LEVEL_INFO, "disconnected, HCI reason 0x%02x (state %s)", gap->params.disconnected.reason,
            _state_names[_hid_state]);
    _link_reset();
    _hid_link_gen++;   /* orphan the watchdog and any pending retry */
    _set_state(HID_IDLE);

    if (_hid_pair_after_disconnect) {
        _hid_pair_after_disconnect = 0;
        if (_scan_start() == 0)
            return;
    }
    _report_link_state(KeyboardLinkDisconnected);
    if (_hid_forgetting) {
        _hid_forgetting = 0;
        return;
    }
    _reconnect_start();
}

/***** SoftDevice event observer *****/

static void _hid_handler(const ble_evt_t *evt, void *context) {
    uint16_t id = evt->header.evt_id;
    /* conn_handle is the first member of every module's event struct. */
    uint16_t conn = evt->evt.common_evt.conn_handle;
    ret_code_t rv;

    switch (id) {
    case BLE_GAP_EVT_CONNECTED:
        /* The phone's peripheral link belongs to nrf52_bluetooth.c. */
        if (evt->evt.gap_evt.params.connected.role != BLE_GAP_ROLE_CENTRAL)
            return;
        _on_connected(&evt->evt.gap_evt);
        return;
    case BLE_GAP_EVT_ADV_REPORT:
        _on_adv_report(&evt->evt.gap_evt.params.adv_report);
        return;
    case BLE_GAP_EVT_TIMEOUT: {
        uint8_t src = evt->evt.gap_evt.params.timeout.src;
        /* Scan and connect timeouts carry no connection; only we scan or initiate. */
        if (src == BLE_GAP_TIMEOUT_SRC_SCAN || src == BLE_GAP_TIMEOUT_SRC_CONN) {
            HID_LOG(APP_LOG_LEVEL_INFO, "evt TIMEOUT src %d", src);
            _on_timeout(src);
            return;
        }
        break;
    }
    default:
        break;
    }

    if (_hid_conn == BLE_CONN_HANDLE_INVALID || conn != _hid_conn)
        return;

    /* Every event for the keyboard link is logged, so a tester's first log
     * shows where a sequence stops.  Notifications drop to DEBUG after the
     * first few, since every key press produces two of them. */
    if (id == BLE_GATTC_EVT_HVX && _hid_hvx_logged >= 8)
        DRV_LOG("bt", APP_LOG_LEVEL_DEBUG, "HID: evt 0x%04x HVX", id);
    else
        HID_LOG(APP_LOG_LEVEL_INFO, "evt 0x%04x %s", id, _evt_name(id));

    switch (id) {
    case BLE_GAP_EVT_DISCONNECTED:
        _on_disconnected(&evt->evt.gap_evt);
        break;
    case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
        _on_sec_params_request(&evt->evt.gap_evt.params.sec_params_request);
        break;
    case BLE_GAP_EVT_AUTH_STATUS:
        _on_auth_status(&evt->evt.gap_evt.params.auth_status);
        break;
    case BLE_GAP_EVT_CONN_SEC_UPDATE:
        _on_conn_sec_update(&evt->evt.gap_evt.params.conn_sec_update);
        break;
    case BLE_GAP_EVT_SEC_REQUEST:
        _on_sec_request(&evt->evt.gap_evt.params.sec_request);
        break;
    case BLE_GAP_EVT_PASSKEY_DISPLAY:
    case BLE_GAP_EVT_AUTH_KEY_REQUEST:
        /* Just Works only: no passkey to show or enter.  Reject so the
         * procedure ends with an AUTH_STATUS failure instead of hanging. */
        HID_LOG(APP_LOG_LEVEL_ERROR, "keyboard wants a passkey; only Just Works pairing is supported");
        (void) sd_ble_gap_auth_key_reply(_hid_conn, BLE_GAP_AUTH_KEY_TYPE_NONE, NULL);
        break;
    case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST: {
        /* S140: as the central we accept by running the update with the
         * requested parameters (NULL would reject). */
        const ble_gap_conn_params_t *cp = &evt->evt.gap_evt.params.conn_param_update_request.conn_params;
        HID_LOG(APP_LOG_LEVEL_INFO, "conn param request: interval %d..%d latency %d timeout %d",
                cp->min_conn_interval, cp->max_conn_interval, cp->slave_latency, cp->conn_sup_timeout);
        rv = sd_ble_gap_conn_param_update(_hid_conn, cp);
        if (rv != NRF_SUCCESS) {
            HID_LOG(APP_LOG_LEVEL_WARNING, "sd_ble_gap_conn_param_update(accept) failed (%d); rejecting", rv);
            (void) sd_ble_gap_conn_param_update(_hid_conn, NULL);
        }
        break;
    }
    case BLE_GAP_EVT_CONN_PARAM_UPDATE: {
        const ble_gap_conn_params_t *cp = &evt->evt.gap_evt.params.conn_param_update.conn_params;
        HID_LOG(APP_LOG_LEVEL_INFO, "conn params now interval %d latency %d timeout %d",
                cp->max_conn_interval, cp->slave_latency, cp->conn_sup_timeout);
        break;
    }
    case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
        const ble_gap_phys_t phys = { .rx_phys = BLE_GAP_PHY_AUTO, .tx_phys = BLE_GAP_PHY_AUTO };
        rv = sd_ble_gap_phy_update(_hid_conn, &phys);
        if (rv != NRF_SUCCESS)
            HID_LOG(APP_LOG_LEVEL_WARNING, "sd_ble_gap_phy_update failed (%d)", rv);
        break;
    }
    case BLE_GATTS_EVT_SYS_ATTR_MISSING:
        /* The keyboard may also act as a GATT client of our server.  S140
         * raises this once per connection before serving such a request;
         * nrf52_bluetooth.c answers it for the phone link only. */
        rv = sd_ble_gatts_sys_attr_set(_hid_conn, NULL, 0, 0);
        if (rv != NRF_SUCCESS)
            HID_LOG(APP_LOG_LEVEL_WARNING, "sd_ble_gatts_sys_attr_set failed (%d)", rv);
        break;
    case BLE_GAP_EVT_PHY_UPDATE:
    case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST:  /* answered by nrf_ble_gatt for every link */
    case BLE_GAP_EVT_DATA_LENGTH_UPDATE:
    case BLE_GAP_EVT_TIMEOUT:                     /* authenticated payload timeout; the link drops on its own */
    case BLE_GAP_EVT_SEC_INFO_REQUEST:            /* peripheral role only */
    case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST:      /* answered by nrf_ble_gatt */
        break;
    case BLE_GATTC_EVT_PRIM_SRVC_DISC_RSP:
        _on_prim_srvc_disc_rsp(&evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_CHAR_DISC_RSP:
        _on_char_disc_rsp(&evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_DESC_DISC_RSP:
        _on_desc_disc_rsp(&evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_READ_RSP:
        _on_read_rsp(&evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_WRITE_RSP:
        _on_write_rsp(&evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_WRITE_CMD_TX_COMPLETE:
        if (_disc_phase == DISC_PROTO_WAIT) {
            _disc_phase = DISC_CCCD;
            _disc_step();
        }
        break;
    case BLE_GATTC_EVT_HVX:
        _hid_hvx_logged++;
        _on_hvx(&evt->evt.gattc_evt);
        break;
    case BLE_GATTC_EVT_TIMEOUT:
        /* S140: after an ATT timeout no GATT procedure can run on this link. */
        _fail("GATT procedure timed out");
        break;
    default:
        break;
    }
}

/***** Core -> driver requests (strong definitions; rcore/keyboard.c has weak defaults) *****/

bool hw_keyboard_is_supported(void) {
    return true;
}

bool hw_keyboard_has_bond(void) {
    return _bond.valid != 0;
}

/* Called from app threads.  sd_* calls are fine here; the state checks are
 * done under a critical section and the resulting transitions arrive as
 * SoftDevice events.  A pairing scan already running returns -1. */
int hw_keyboard_pair_start(void) {
    uint16_t conn;
    enum hid_state st;

    taskENTER_CRITICAL();
    st = _hid_state;
    conn = _hid_conn;
    if (st != HID_SCANNING) {
        _hid_forgetting = 0;
        _hid_reconnect_backoff = 0;
        _hid_pair_after_disconnect = (conn != BLE_CONN_HANDLE_INVALID);
    }
    taskEXIT_CRITICAL();
    if (st == HID_SCANNING) {
        HID_LOG(APP_LOG_LEVEL_INFO, "pair_start: a pairing scan is already running");
        return -1;
    }

    /* S140: sd_ble_gap_connect_cancel ends a pending initiator and
     * sd_ble_gap_scan_stop a scanner; both return NRF_ERROR_INVALID_STATE
     * when there is nothing to stop, and neither generates an event. */
    (void) sd_ble_gap_connect_cancel();
    (void) sd_ble_gap_scan_stop();
    if (st == HID_CONNECTING)
        _set_state(HID_IDLE);

    if (conn != BLE_CONN_HANDLE_INVALID) {
        /* One central link only: drop the current keyboard, and scan once
         * BLE_GAP_EVT_DISCONNECTED arrives (_hid_pair_after_disconnect). */
        HID_LOG(APP_LOG_LEVEL_INFO, "pair_start: disconnecting the current keyboard first");
        ret_code_t rv = sd_ble_gap_disconnect(conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        if (rv != NRF_SUCCESS) {
            HID_LOG(APP_LOG_LEVEL_ERROR, "sd_ble_gap_disconnect failed (%d)", rv);
            _hid_pair_after_disconnect = 0;
            return -1;
        }
        _report_link_state(KeyboardLinkScanning);
        return 0;
    }

    return _scan_start();
}

int hw_keyboard_connect(void) {
    enum hid_state st;
    uint16_t conn;

    if (!_bond.valid) {
        HID_LOG(APP_LOG_LEVEL_INFO, "connect: no bonded keyboard");
        return -1;
    }
    taskENTER_CRITICAL();
    st = _hid_state;
    conn = _hid_conn;
    _hid_forgetting = 0;
    _hid_reconnect_backoff = 0;
    taskEXIT_CRITICAL();
    if (conn != BLE_CONN_HANDLE_INVALID || st == HID_CONNECTING)
        return 0;  /* already connected, or the reconnect initiator is pending */
    if (st == HID_SCANNING) {
        HID_LOG(APP_LOG_LEVEL_INFO, "connect: a pairing scan is running");
        return -1;
    }
    _reconnect_start();
    return (_hid_state == HID_CONNECTING) ? 0 : -1;
}

/* Forget the bonded keyboard.  The bond is invalidated in RAM here and
 * persisted from the service thread: prefs_put blocks on the flash
 * filesystem, and although rcore/bluetooth.c already writes the RDB from an
 * app thread (bluetooth_bond_acknowledge), routing every bond write through
 * the service thread keeps the UI thread off the flash and serialises the
 * writes with the store that pairing triggers. */
int hw_keyboard_forget(void) {
    uint16_t conn;
    enum hid_state st;

    taskENTER_CRITICAL();
    _bond.valid = 0;
    conn = _hid_conn;
    st = _hid_state;
    _hid_forgetting = (conn != BLE_CONN_HANDLE_INVALID);
    _hid_pair_after_disconnect = 0;
    _hid_reconnect_backoff = 0;
    taskEXIT_CRITICAL();

    (void) sd_ble_gap_connect_cancel();
    (void) sd_ble_gap_scan_stop();
    if (st == HID_CONNECTING || st == HID_SCANNING)
        _set_state(HID_IDLE);
    /* Drop the identity list entry; fails harmlessly if a role holds it. */
    (void) sd_ble_gap_device_identities_set(NULL, NULL, 0);

    if (conn != BLE_CONN_HANDLE_INVALID) {
        ret_code_t rv = sd_ble_gap_disconnect(conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        if (rv != NRF_SUCCESS)
            HID_LOG(APP_LOG_LEVEL_ERROR, "forget: sd_ble_gap_disconnect failed (%d)", rv);
    } else {
        _report_link_state(KeyboardLinkDisconnected);
    }
    _hid_name[0] = 0;
    service_submit(_svc_bond_store, NULL, 0);
    HID_LOG(APP_LOG_LEVEL_INFO, "bond forgotten");
    return 0;
}

/***** Init *****/

/* Called from hw_bluetooth_init() (BT thread) after the SoftDevice is
 * enabled, the pairing handler is registered and PPoGATT is set up. */
void nrf52_hid_init(void) {
    NRF_SDH_BLE_OBSERVER(hid_observer, 3 /* priority */, _hid_handler, NULL);

    _link_reset();
    _hid_state = HID_IDLE;
    memset(&_bond, 0, sizeof(_bond));

    /* The bond is loaded lazily on the service thread (prefs_get blocks);
     * that callback also starts the background reconnect. */
    service_submit(_svc_bond_load, NULL, 0);
    HID_LOG(APP_LOG_LEVEL_INFO, "keyboard host initialised (%d central link, scan buffer %d bytes)",
            NRF_SDH_BLE_CENTRAL_LINK_COUNT, (int)sizeof(_scan_buf_data));
}
