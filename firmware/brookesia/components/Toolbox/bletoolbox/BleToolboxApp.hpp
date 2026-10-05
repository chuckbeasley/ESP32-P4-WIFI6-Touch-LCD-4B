/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Toolbox — phone app. See ble_toolbox_host.h for where the radio actually is.
 *
 * Ported from the Tactility app of the same name. Its four screens are Scan,
 * Observer, AirTag Monitor and Spam; this app carries the first three. Spam
 * transmits, and is being brought over behind the same arming gate the Wi-Fi
 * toolbox's injection uses rather than as an ordinary button.
 */
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

#include "ble_toolbox_host.h"

class BleToolboxApp : public ESP_Brookesia_PhoneApp
{
public:
    BleToolboxApp();
    ~BleToolboxApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    /* Builds the screens, walks the menu, starts a scan and reports what arrived.
     *
     * Exists because a synthetic LV_EVENT_CLICKED does not carry the current-target
     * pointer the launcher's own handler reads, so an app cannot be opened from
     * outside by clicking its icon — the Wi-Fi toolbox hit the same wall. Driving run()
     * and showScreen() directly exercises the parts that a tap would, which is the
     * navigation and the scan, and is what the startup check calls. */
    void selfTest(void);

    /* For the self-test's report. An index rather than the Screen enum, which is
     * private — naming it here would not compile. */
    int currentScreenIndex(void) const { return (int)active_screen; }
    uint32_t totalAdvertisements(void) const { return scan_total; }
    int latchedDevices(void) const { return scan_latch_len; }

    int  trackerAdverts(void) const { return (int)tag_total; }
    int  glassesAdverts(void) const { return (int)glasses_total; }

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    /* Exercises the spam gate. The claims worth testing are the negative ones — that an
     * arm without acknowledgement is refused, that a disarmed or expired transmitter
     * stops — because those are the properties the safety of this feature rests on and
     * they are exactly what a UI test cannot show. Part of the startup check. */
    void spamSelfTest(void);
#endif

private:
    /* Sub-screens live inside one default screen as panels rather than as separate
     * LVGL screens, so the core's visual-area and teardown handling applies once. */
    enum Screen {
        SCREEN_MENU = 0,
        SCREEN_SCAN,
        SCREEN_OBSERVER,
        SCREEN_AIRTAG,
        SCREEN_RADAR,
        SCREEN_CONNECT,
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
        SCREEN_SPAM,
#endif
        SCREEN_COUNT,
    };

    void buildMenu(void);
    void buildScan(void);
    void buildObserver(void);
    void buildAirTag(void);
    void buildRadar(void);
    void buildConnect(void);
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    void buildSpam(void);
    void refreshSpam(void);
    void askSpamArm(void);
#endif
    void showScreen(Screen screen);

    /* Bring the BLE service up on first use. Deliberately not done in init(): the
     * Wi-Fi toolbox established that touching esp-hosted at app-install time, before
     * the co-processor has been enumerated, breaks the Wi-Fi service's own bring-up. */
    bool ensureService(void);

    void refreshScan(void);
    void refreshObserver(void);
    void refreshAirTag(void);
    void refreshRadar(void);
    void radarReveal(void);
    void updateRadarToggle(void);
    void refreshConnect(void);
    void updateStatus(void);

    /* Clear every counter and list the three screens show, so a Start begins a new run
     * rather than adding to the last one. */
    void resetCounters(void);

    static void onEvent(lv_event_t *e);
    static void onTick(lv_timer_t *timer);
    static void onScanRowClick(lv_event_t *e);
    static void onConnRowClick(lv_event_t *e);

    /* Stop the scan and initiate a connection to a scanned device, then open the
     * connect screen. */
    void connectTo(const ble_toolbox_adv_t *adv);

    /* Callbacks from the BLE service, which run on NimBLE's task. They latch and
     * return; nothing here touches LVGL. */
    static void onAdv(const ble_toolbox_adv_t *adv, void *user);
    static void onRawAdv(const ble_toolbox_raw_adv_t *adv, void *user);
    static void onScanState(bool scanning, esp_err_t reason, void *user);
    static void onObserverScroll(lv_event_t *e);
    static void onConnState(ble_toolbox_conn_state_t state, esp_err_t reason, void *user);
    static void onConnSvc(const ble_toolbox_gatt_svc_t *svc, void *user);
    static void onConnChr(const ble_toolbox_gatt_chr_t *chr, void *user);
    static void onConnRead(uint16_t val_handle, const uint8_t *data, uint16_t len, void *user);
    static void onConnNotify(uint16_t attr_handle, const uint8_t *data, uint16_t len,
                             bool indication, void *user);
    static void onPasskey(uint16_t conn_handle, uint8_t action, uint32_t numcmp, void *user);
    void showPasskey(void);
    void closePasskey(void);
    static void onPasskeyEvent(lv_event_t *e);

    void latchAdv(const ble_toolbox_adv_t *adv);
    void renderObserverRows(void);
    void latchRaw(const ble_toolbox_raw_adv_t *adv);
    void latchSvc(const ble_toolbox_gatt_svc_t *svc);
    void latchChr(const ble_toolbox_gatt_chr_t *chr);
    void latchRead(uint16_t val_handle, const uint8_t *data, uint16_t len);
    void latchNotify(uint16_t attr_handle, const uint8_t *data, uint16_t len, bool indication);

    lv_obj_t *screens[SCREEN_COUNT];

    /* Status strip across the top of every screen. */
    lv_obj_t  *status_label;

    /* Scan */
    lv_obj_t  *scan_state_label;
    lv_obj_t  *scan_count_label;
    lv_obj_t  *scan_list;
    lv_obj_t  *scan_start_btn;
    lv_obj_t  *scan_stop_btn;

    /* Observer */
    lv_obj_t  *obs_count_label;
    lv_obj_t  *obs_log;
    lv_obj_t  *obs_start_btn;
    lv_obj_t  *obs_stop_btn;

    /* AirTag monitor */
    lv_obj_t  *tag_count_label;
    lv_obj_t  *tag_list;
    lv_obj_t  *tag_start_btn;
    lv_obj_t  *tag_stop_btn;
    lv_obj_t  *tag_glasses_label;

    /* Proximity radar */
    lv_obj_t  *radar_area;       /* the circle; blips are children of radar_blips */
    lv_obj_t  *radar_blips;      /* transparent overlay that holds the blips */
    lv_obj_t  *radar_sweep;      /* the rotating sweep line */
    lv_obj_t  *radar_toggle_btn; /* single Start/Stop Scan toggle */
    int        radar_sweep_angle;/* 0.1-degree units */
    int        radar_size;       /* circle diameter in px, from the visual area */
    lv_obj_t  *radar_blip_objs[40]; /* persistent blips, revealed by the sweep */
    lv_obj_t  *radar_blip_labels[40]; /* device-name label next to each blip */
    int        radar_blip_count;

    /* Connected-device GATT browser */
    lv_obj_t  *conn_state_label;
    lv_obj_t  *conn_list;
    lv_obj_t  *conn_value_label;
    lv_obj_t  *conn_disconnect_btn;
    lv_obj_t  *conn_back_btn;

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    /* Spam. Present only when the transmit path is compiled in, so a build without it
     * has no widgets to hide and no state to keep consistent with an absent backend. */
    lv_obj_t  *spam_family_dd;
    lv_obj_t  *spam_state_label;
    lv_obj_t  *spam_count_label;
    lv_obj_t  *spam_payload_label;
    lv_obj_t  *spam_start_btn;
    lv_obj_t  *spam_stop_btn;

    /* The arming dialog. Created on lv_layer_top(), NOT on the app's default screen, so
     * the core's cleanDefaultScreen() does not reach it: if the app closes with the
     * dialog open, this pointer is the only handle on it and the dialog would sit over
     * the launcher for good. close() deletes it. The Wi-Fi app learned this the hard way
     * and its arm_modal is the same arrangement. */
    lv_obj_t  *spam_modal;

    static void onSpamArmConfirmed(lv_event_t *e);
    static void onSpamArmCancelled(lv_event_t *e);
#endif

    lv_timer_t *ui_timer;

    Screen active_screen;
    bool   service_up;
    bool   scanning;

    /* Latches. The service's callbacks run on NimBLE's task and must not touch LVGL,
     * so they append here and the UI timer drains these at its own pace.
     *
     * The sizes are what a screen can usefully show, not what the radio can receive: this
     * room produces 700-odd advertisements in seconds, and a list is a view of the newest
     * few rather than an archive. The total is counted separately and reported, so nothing
     * is lost from the numbers even when the list is full.
     *
     * kObsCap was 12, which for a sniffer is too few to see a pattern in - a dozen frames
     * is under a second of traffic here. It is now 60, which is a few seconds of real
     * traffic and about what will fit on the screen without an unreasonable scroll. */
    enum { kScanCap = 40, kTagCap = 40, kObsCap = 1024, kObsRowPool = 12 };

    struct ScanEntry {
        ble_toolbox_adv_t adv;
    };
    ScanEntry scan_latch[kScanCap];
    volatile int scan_latch_len;
    volatile uint32_t scan_total;

    struct ObsEntry {
        ble_toolbox_raw_adv_t raw;
        ble_toolbox_adv_info_t info;
    };
    ObsEntry *obs_latch;           /* PSRAM-backed, kObsCap entries, allocated in init() */
    volatile int obs_latch_len;
    volatile uint32_t obs_total;

    /* Virtualized renderer: a fixed pool of row widgets repositioned over the visible
     * slice of the latch, so UI cost is constant regardless of how many entries exist. */
    struct ObsRowWidget {
        lv_obj_t *row;
        lv_obj_t *name;
        lv_obj_t *kind;
        lv_obj_t *vt;
        lv_obj_t *addr;
        lv_obj_t *rssi;
    };
    ObsRowWidget obs_row[kObsRowPool];
    lv_obj_t *obs_spacer;          /* transparent child whose height = obs_latch_len * row */
    lv_obj_t *obs_empty;           /* "nothing heard yet" placeholder */

    /* AirTag-class adverts, kept apart from the general listing because that is the
     * whole point of the screen. */
    struct TagEntry {
        ble_toolbox_adv_t adv;
    };
    TagEntry tag_latch[kTagCap];
    volatile int tag_latch_len;
    volatile uint32_t tag_total;
    volatile uint32_t glasses_total;

    /* GATT browser latches, filled by the connection callbacks and drained by the timer. */
    enum { kSvcCap = 32, kChrCap = 64 };
    ble_toolbox_gatt_svc_t svc_latch[kSvcCap];
    volatile int svc_latch_len;
    ble_toolbox_gatt_chr_t chr_latch[kChrCap];
    volatile int chr_latch_len;
    volatile int conn_nav;              /* 0 = services, 1 = characteristics */
    uint16_t conn_svc_start;            /* the service whose characteristics are shown */
    uint16_t conn_svc_end;
    volatile ble_toolbox_conn_state_t conn_state;
    volatile int conn_fail_status;       /* BLE status of the last failed connect (0 = none) */
    uint8_t conn_addr[6];                /* address of the connected peer (for the Scan toggle) */
    bool conn_random;                    /* its address type */
    char conn_read_buf[96];
    volatile uint16_t conn_read_len;
    volatile uint16_t conn_read_handle;
    char conn_notify_buf[96];            /* latest notification/indication, hex + key */
    volatile uint16_t conn_notify_handle;

    /* Pairing-code request. Latched on NimBLE's task, drained on the timer. */
    volatile bool passkey_pending;
    uint8_t passkey_action;        /* BLE_SM_IOACT_* */
    uint32_t passkey_numcmp;       /* the number for NUMCMP/DISP */
    lv_obj_t *passkey_modal;       /* modal on lv_layer_top(); nullptr when hidden */
    lv_obj_t *passkey_value;       /* textarea (INPUT) or label (NUMCMP/DISP) */

    /* Set by the callbacks, cleared by the timer. */
    volatile bool list_dirty;
    /* lv_tick_get() at the last full-list rebuild, to throttle it while scanning. */
    uint32_t last_list_rebuild_ms;
    /* Auto-subscribe is driven from the timer (not NimBLE's task) so the GATT client
     * operations run on the app's own task, the same as the manual Subscribe button. */
    volatile bool auto_discover_chars;
    volatile bool auto_subscribe;
};
