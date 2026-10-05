/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Toolbox — host side.
 *
 * The radio is on the ESP32-C6. This chip has none (spec section 36), so BLE reaches
 * the app over esp-hosted's HCI transport: a NimBLE host runs here and the C6 acts as
 * the controller. Stage 1 established that path end to end.
 *
 * This module owns that NimBLE host and offers the scanning half of the Tactility BLE
 * Toolbox in ESP-IDF terms. The transmit half (BLE Spam) is deliberately absent until
 * it can carry the same arming gate the Wi-Fi toolbox's injection uses — a scanner
 * that cannot transmit is a different risk class from a toolbox that can.
 *
 * Nothing here draws: the app polls or receives callbacks and renders from its own
 * timer, which is the same split the Wi-Fi toolbox uses and the reason its callbacks
 * are allowed to run on whatever task NimBLE happens to be using.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An advertisement, reduced to what a listing needs.
 *
 * `addr` is the six address bytes in over-the-air order (most significant last, as
 * the controller reports them); addr_str holds the human form. `name` is empty when
 * the advertiser sent none, which is common — the Tactility app shows a type or an
 * address in that case rather than pretending to know. */
typedef struct {
    uint8_t  addr[6];
    char     addr_str[18];         /* "aa:bb:cc:dd:ee:ff" */
    int8_t   rssi;
    char     name[32];             /* empty when absent */
    bool     connectable;
    bool     scannable;
    bool     directed;             /* directed advertising: someone is being paged */
    bool     is_scan_response;     /* this report is a scan response, not an advert */
    bool     random_addr;          /* the peer uses a random (not public) address */
    uint16_t company_id;           /* Bluetooth SIG company ID from the manufacturer-specific
                                    * AD field, little-endian; 0 when the advert has none */
    uint16_t service_uuid;         /* first 16-bit service UUID in the AD, 0 when none */
    uint16_t appearance;           /* GAP appearance value, 0 when none */
} ble_toolbox_adv_t;

/* A raw advertisement, for the observer screen. Unlike the listing above this keeps
 * the payload so the decoder can say what the advert actually is. */
typedef struct {
    uint8_t  addr[6];
    char     addr_str[18];
    int8_t   rssi;
    uint8_t  adv_type;             /* the controller's event type */
    uint8_t  data[64];             /* the AD structures, as received */
    uint8_t  data_len;
    uint16_t company_id;           /* Bluetooth SIG company ID from the manufacturer-specific
                                    * AD field, little-endian; 0 when the advert has none */
    uint16_t service_uuid;         /* first 16-bit service UUID in the AD, 0 when none */
    uint16_t appearance;           /* GAP appearance value, 0 when none */
} ble_toolbox_raw_adv_t;

/* Scan parameters. Channel 37/38/39 are the advertising channels; leaving a channel
 * out is the standard way to shorten a scan. */
typedef struct {
    uint16_t interval_ms;          /* 0 = module default */
    uint16_t window_ms;            /* 0 = module default; must be <= interval */
    bool     passive;              /* true: send no scan requests */
    bool     filter_duplicates;    /* true: one report per address */
} ble_toolbox_scan_params_t;

/* ---- GATT client (connection) ------------------------------------------- */

/* Connection state. */
typedef enum {
    BLE_TOOLBOX_CONN_DISCONNECTED = 0,
    BLE_TOOLBOX_CONN_CONNECTING,
    BLE_TOOLBOX_CONN_CONNECTED,
} ble_toolbox_conn_state_t;

/* A discovered GATT service, reduced to what the UI needs. The UUID is its
 * formatted text form ("1800" for the standard 16-bit ones, or the full
 * 128-bit string), so the header stays free of NimBLE types. */
typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    char     uuid_str[40];
} ble_toolbox_gatt_svc_t;

/* A discovered GATT characteristic. `properties` holds the BLE_GATT_CHR_PROP_*
 * flags (read, write, notify, ...). */
typedef struct {
    uint16_t val_handle;
    uint8_t  properties;
    char     uuid_str[40];
} ble_toolbox_gatt_chr_t;

typedef struct {
    /* One per advertisement, from NimBLE's task. Keep it short: it runs on the host
     * task, and blocking here stalls the controller's event handling. The app latches
     * what it needs and renders elsewhere. */
    void (*on_adv)(const ble_toolbox_adv_t *adv, void *user);
    void (*on_raw)(const ble_toolbox_raw_adv_t *adv, void *user);
    void (*on_scan_state)(bool scanning, esp_err_t reason, void *user);

    /* ---- Connection callbacks. Also run on NimBLE's host task. ---- */
    void (*on_conn_state)(ble_toolbox_conn_state_t state, esp_err_t reason, void *user);
    /* One call per discovered service, then a final call with `svc == NULL` marking
     * the end of the list. */
    void (*on_conn_svc)(const ble_toolbox_gatt_svc_t *svc, void *user);
    /* One call per discovered characteristic, then a final call with `chr == NULL`. */
    void (*on_conn_chr)(const ble_toolbox_gatt_chr_t *chr, void *user);
    /* The result of a read; `data` is `len` bytes, valid only for this call. */
    void (*on_conn_read)(uint16_t val_handle, const uint8_t *data, uint16_t len, void *user);
    /* A notification or indication arrived on `attr_handle`; `data` is `len` bytes,
     * valid only for this call. */
    void (*on_conn_notify)(uint16_t attr_handle, const uint8_t *data, uint16_t len,
                           bool indication, void *user);
    /* A pairing code is required to complete the connection. `action` is one of the
     * BLE_SM_IOACT_* constants (INPUT to enter the peer's code, NUMCMP to confirm a
     * number, DISP to show ours); `numcmp` carries the number for NUMCMP. */
    void (*on_passkey)(uint16_t conn_handle, uint8_t action, uint32_t numcmp, void *user);
} ble_toolbox_host_callbacks_t;

/* Bring the NimBLE host up against the co-processor's controller, and bind the given
 * callbacks. Idempotent: later calls with callbacks replace them.
 *
 * Must not be called until esp-hosted has reset and enumerated the co-processor. The
 * Wi-Fi toolbox learned this the hard way (see ToolboxApp::init) and the same ordering
 * applies here: the hosted HCI transport does not exist before then. */
esp_err_t ble_toolbox_host_init(const ble_toolbox_host_callbacks_t *cbs, void *user);

bool ble_toolbox_host_is_ready(void);

/* Wait for the controller to answer, up to timeout_ms. ESP_OK if it did, ESP_ERR_TIMEOUT if not.
 *
 * init() deliberately waits only 5 s, because blocking a caller for most of a minute with nothing
 * to show for it is worse than returning. But measured on this board the controller does not
 * answer until ~6 s after the HCI pipe binds - it times out at 2 s and 4 s, resets, and syncs at
 * 6 s - so a 5 s wait fails on a radio that is about to work. That is what produced
 *
 *     30790  NimBLE never synced: the co-processor's controller is not answering
 *     31840  NimBLE synced with the co-processor's controller
 *
 * - init gave up one second before success.
 *
 * So the short wait stays in init() and callers who can afford to wait get this instead. Waiting
 * on the ready flag rather than on invented constants means it returns the moment the radio
 * answers, and there is no duration to get wrong. */
esp_err_t ble_toolbox_host_wait_ready(uint32_t timeout_ms);

/* Start the service if it is not running, and say whether the radio is usable yet.
 *
 *      ESP_OK                - the controller has answered; calls may proceed
 *      ESP_ERR_NOT_FINISHED  - coming up; call again later
 *      anything else         - failed outright
 *
 * Exists so a caller does not have to BLOCK waiting for the controller. On this board that
 * wait is 30 to 45 seconds, and a caller on the LVGL task that waits for it freezes the
 * whole UI for the duration — which is worse than the failure it was written to avoid. A
 * caller with a timer should poll this from the timer and keep its UI alive. */
esp_err_t ble_toolbox_host_begin(void);

/* Register or replace the callbacks without waiting for the radio. Safe to call before the
 * controller answers, which is the point: a caller subscribes first, then polls begin(). */
esp_err_t ble_toolbox_host_set_callbacks(const ble_toolbox_host_callbacks_t *cbs, void *user);

/* Start a scan. Starts the host on first use. Fails with ESP_ERR_INVALID_STATE if one
 * is already running. */
esp_err_t ble_toolbox_host_scan_start(const ble_toolbox_scan_params_t *params);

/* Stop a running scan. ESP_OK when nothing was running, so callers need not track it. */
esp_err_t ble_toolbox_host_scan_stop(void);

/* Stop the scan *watcher* without issuing scan-disable (which hangs the C6 controller).
 * The scan is left to end on its own (the controller stops it after ~10 s). */
esp_err_t ble_toolbox_host_scan_stop_no_cancel(void);

bool ble_toolbox_host_is_scanning(void);

/* Number of advertisements accepted since the last start, and the number of scan
 * starts that failed to begin — both for the status line. */
uint32_t ble_toolbox_host_adv_count(void);

/* ---- Connection (GATT client) ------------------------------------------- */

/* Connect to a peer. `addr` is the six address bytes in the controller's order
 * (least-significant first), as the scan reports them; `random_addr` is true when
 * the peer uses a random (rather than public) address. Fails with
 * ESP_ERR_INVALID_STATE if a scan is still running. */
esp_err_t ble_toolbox_host_connect(const uint8_t addr[6], bool random_addr);

/* Disconnect the current connection, or cancel a pending connect. */
esp_err_t ble_toolbox_host_disconnect(void);

/* Discover all primary services on the current connection. Results stream to
 * on_conn_svc, then a final call with a NULL service. */
esp_err_t ble_toolbox_host_discover_services(void);

/* Discover all characteristics within a service's handle range. Results stream to
 * on_conn_chr, then a final call with a NULL characteristic. */
esp_err_t ble_toolbox_host_discover_chars(uint16_t start_handle, uint16_t end_handle);

/* Read a characteristic value; the result streams to on_conn_read. */
esp_err_t ble_toolbox_host_read(uint16_t val_handle);

/* Write a characteristic value with response. */
esp_err_t ble_toolbox_host_write(uint16_t val_handle, const uint8_t *data, uint16_t len);

/* Subscribe to notifications/indications on a characteristic by writing its Client
 * Characteristic Configuration Descriptor (CCCD). `val_handle` is the characteristic's
 * value handle; the CCCD is the descriptor immediately after it. `want_indication`
 * selects indications (true) over notifications (false). */
esp_err_t ble_toolbox_host_subscribe(uint16_t val_handle, bool want_indication);

/* The current connection state. */
ble_toolbox_conn_state_t ble_toolbox_host_conn_state(void);

/* Passkey/pairing action codes, mirroring NimBLE's BLE_SM_IOACT_* values so callers do
 * not need to include NimBLE headers to interpret the on_passkey callback. */
enum {
    BLE_TOOLBOX_IOACT_INPUT  = 2,   /* enter the peer's passkey */
    BLE_TOOLBOX_IOACT_DISP   = 3,   /* display our passkey */
    BLE_TOOLBOX_IOACT_NUMCMP = 4,   /* confirm a number */
};

/* Answer a pending pairing-code request. For BLE_SM_IOACT_INPUT, `passkey` is the
 * six-digit code the user entered; for NUMCMP and DISP, `accept` says whether to
 * proceed. Returns ESP_ERR_INVALID_STATE when no request is pending. */
esp_err_t ble_toolbox_host_passkey_reply(uint32_t passkey, bool accept);

/* ---- Decoding -------------------------------------------------------------
 *
 * Pure functions over an advertisement's bytes, in the same spirit as the Wi-Fi
 * toolbox's frame builders: no radio, no state, testable by feeding them bytes.
 * The observer and AirTag screens are built on these. */

/* What an advertisement turned out to be. */
typedef enum {
    BLE_ADV_UNKNOWN = 0,
    BLE_ADV_IBEACON,
    BLE_ADV_EDDYSTONE_UID,
    BLE_ADV_EDDYSTONE_URL,
    BLE_ADV_EDDYSTONE_TLM,
    BLE_ADV_APPLE_FINDMY,          /* an offline-finding advert, i.e. AirTag-class */
    BLE_ADV_APPLE_NEARBY,          /* an Apple continuity advert, "Nearby Info" */
    BLE_ADV_SMART_GLASSES,         /* a manufacturer or appearance that suggests them */
} ble_toolbox_adv_kind_t;

/* Decoded detail. Only the fields relevant to `kind` are filled. */
typedef struct {
    ble_toolbox_adv_kind_t kind;

    /* iBeacon */
    uint8_t  uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t   tx_power;

    /* Eddystone URL */
    char     url[64];

    /* Eddystone TLM */
    uint16_t battery_mv;
    int16_t  temperature_c_x256;

    /* FindMy: the status byte distinguishes separated from near-owner. */
    uint8_t  findmy_status;
    bool     findmy_separated;

    /* Smart-glasses heuristic: why it was flagged, for the UI to show. */
    const char *reason;
} ble_toolbox_adv_info_t;

/* Decode one advertisement's AD structures. Always succeeds; an advertisement that
 * matches nothing comes back as BLE_ADV_UNKNOWN with only `kind` set. */
void ble_toolbox_adv_decode(const uint8_t *data, uint8_t len, ble_toolbox_adv_info_t *out);

const char *ble_toolbox_adv_kind_name(ble_toolbox_adv_kind_t kind);

#ifdef __cplusplus
}
#endif
