/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Toolbox — phone app. See BleToolboxApp.hpp for scope.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "BleToolboxApp.hpp"
#include "ble_toolbox_spam.h"
#include "toolbox_ui.hpp"
#include "toolbox_icons.hpp"
#include "bsp/esp-bsp.h"

static const char *TAG = "BleToolbox";

/* ---- Deferred connect -----------------------------------------------------
 *
 * The connect runs on a background task so it can wait out an in-progress scan
 * (the host refuses to connect while the controller is scanning, and scan-disable
 * hangs the C6). The tap handler only latches the address and returns immediately,
 * so the LVGL/touch task is never blocked. The task performs the connect once the
 * scan has ended on its own. */
static uint8_t        g_pending_addr[6];
static bool           g_pending_random;
static volatile bool  g_pending_connect;
static volatile bool  g_show_connect;
static int            g_conn_retries;

static void tb_connect_task(void *arg)
{
    (void)arg;
    while (true) {
        if (g_pending_connect) {
            g_pending_connect = false;
            const esp_err_t err = ble_toolbox_host_connect(g_pending_addr, g_pending_random);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- Bluetooth keyboard → LVGL keypad input -------------------------------
 *
 * A BLE HID keyboard reports key codes in notifications. Those are mapped to LVGL
 * key events and fed through a keypad input device, so the keys navigate and select
 * in the UI like a wired keyboard would. The key state is written on NimBLE's task
 * and read on the LVGL input timer; a single uint32_t + bool is safe enough here. */

static uint32_t g_lv_key = 0;           /* LV_KEY_* of the currently held key */
static bool g_lv_key_pressed = false;
static lv_indev_t *g_lv_keypad_indev = nullptr;
static lv_group_t *g_lv_group = nullptr; /* focus group driven by the BLE keyboard */

static uint32_t tb_hid_to_lv_key(uint8_t hid)
{
    switch (hid) {
    case 0x52: return LV_KEY_UP;         /* keyboard up arrow */
    case 0x51: return LV_KEY_DOWN;       /* keyboard down arrow */
    case 0x50: return LV_KEY_LEFT;       /* keyboard left arrow */
    case 0x4F: return LV_KEY_RIGHT;      /* keyboard right arrow */
    case 0x28: return LV_KEY_ENTER;
    case 0x29: return LV_KEY_ESC;
    case 0x2A: return LV_KEY_BACKSPACE;
    case 0x2B: return LV_KEY_NEXT;       /* Tab */
    default: return 0;
    }
}

static void tb_keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->key = g_lv_key;
    data->state = g_lv_key_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;   /* one key state per read; never loop */
}

static void tb_group_add(lv_obj_t *obj)
{
    if (obj != nullptr && g_lv_group != nullptr) {
        lv_group_add_obj(g_lv_group, obj);
        /* A visible focus ring, so the keyboard's current selection is obvious. */
        static lv_style_t focus_style;
        static bool focus_style_init = false;
        if (!focus_style_init) {
            lv_style_init(&focus_style);
            lv_style_set_outline_width(&focus_style, 3);
            lv_style_set_outline_color(&focus_style, lv_palette_main(LV_PALETTE_BLUE));
            lv_style_set_outline_pad(&focus_style, 3);
            focus_style_init = true;
        }
        lv_obj_add_style(obj, &focus_style, LV_STATE_FOCUSED);
    }
}

/* Launcher icon: the Bluetooth rune on the same blue the Wi-Fi toolbox uses, so the pair
 * reads as a set. Painted, not converted — see toolbox_icons.hpp for why, and for the
 * pixel-layout trap.
 *
 * The vertices are Bluetooth.svg's own path, transformed to the icon. That file settled a
 * question a dozen hand-derived attempts got wrong: the rune is ONE continuous polyline of
 * six segments, with the stem as part of the path, not a stem with chevrons or crossing
 * diagonals attached. See the header for the SVG coordinates. */
static const toolbox_pt_t k_bt_points[] = {
    { 35.2f, 33.2f },   /* SVG (157,330)  - the left end */
    { 76.8f, 75.1f },   /* SVG (462,637)  - right vertex, upper arm */
    { 56.8f, 99.4f },   /* SVG (315,815)  - stem bottom */
    { 56.8f, 12.6f },   /* SVG (315,179)  - stem top, straight up the stem */
    { 76.8f, 35.8f },   /* SVG (462,349)  - right vertex, lower arm */
    { 35.2f, 76.6f },   /* SVG (157,648)  - back to the left */
};

static toolbox_icon_state_t s_icon_state;
static const toolbox_bt_glyph_t k_bt_glyph = {
    .pts = k_bt_points,
    .count = (int)(sizeof(k_bt_points) / sizeof(k_bt_points[0])),
    .half_width = 3.62f,        /* the SVG's stroke-width 53, scaled */
};

static const lv_image_dsc_t *ble_launcher_icon(void)
{
    return toolbox_icon_paint(&s_icon_state, 0x49, 0x8B, 0xE8,
                              toolbox_icon_is_bluetooth, (void *)&k_bt_glyph, TAG);
}

/* The instance the event handler reads. See onEvent for why this exists rather than
 * the event's user_data carrying it. */
static BleToolboxApp *g_app = nullptr;

/* Action codes. The bands are the Wi-Fi app's convention: menu and back below 100,
 * per-screen controls above it, so a glance at a number says which screen it is from
 * and the two apps cannot collide by accident. */
#define ACT_MENU_SCAN       1
#define ACT_MENU_OBSERVER   2
#define ACT_MENU_AIRTAG     3
#define ACT_BACK          100

#define ACT_SCAN_START    201
#define ACT_SCAN_STOP     202
#define ACT_OBS_START     301
#define ACT_OBS_STOP      302
#define ACT_TAG_START     401
#define ACT_TAG_STOP      402
#define ACT_MENU_SPAM     4
#define ACT_SPAM_START    501
#define ACT_SPAM_STOP     502

#define ACT_CONN_DISCONNECT 601
#define ACT_CONN_SERVICES   602
#define ACT_CONN_WRITE      603
#define ACT_CONN_SUBSCRIBE  604

BleToolboxApp::BleToolboxApp():
    ESP_Brookesia_PhoneApp("BLE Toolbox", nullptr, true),
    status_label(nullptr),
    scan_state_label(nullptr),
    scan_count_label(nullptr),
    scan_list(nullptr),
    scan_start_btn(nullptr),
    scan_stop_btn(nullptr),
    obs_count_label(nullptr),
    obs_log(nullptr),
    obs_start_btn(nullptr),
    obs_stop_btn(nullptr),
    tag_count_label(nullptr),
    tag_list(nullptr),
    tag_start_btn(nullptr),
    tag_stop_btn(nullptr),
    tag_glasses_label(nullptr),
    conn_state_label(nullptr),
    conn_list(nullptr),
    conn_value_label(nullptr),
    conn_disconnect_btn(nullptr),
    conn_back_btn(nullptr),
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    spam_family_dd(nullptr),
    spam_state_label(nullptr),
    spam_count_label(nullptr),
    spam_payload_label(nullptr),
    spam_start_btn(nullptr),
    spam_stop_btn(nullptr),
    spam_modal(nullptr),
#endif
    ui_timer(nullptr),
    active_screen(SCREEN_MENU),
    service_up(false),
    scanning(false),
    scan_latch_len(0),
    scan_total(0),
    obs_latch_len(0),
    obs_total(0),
    tag_latch_len(0),
    tag_total(0),
    glasses_total(0),
    svc_latch_len(0),
    chr_latch_len(0),
    conn_nav(0),
    conn_svc_start(0),
    conn_svc_end(0),
    conn_state(BLE_TOOLBOX_CONN_DISCONNECTED),
    conn_fail_status(0),
    conn_read_len(0),
    conn_read_handle(0),
    conn_notify_handle(0),
    list_dirty(false),
    auto_discover_chars(false),
    auto_subscribe(false)
{
    memset(screens, 0, sizeof(screens));
}

BleToolboxApp::~BleToolboxApp()
{
}

bool BleToolboxApp::init(void)
{
    ESP_LOGI(TAG, "init");
    g_app = this;

    /* The launcher icon has to be set here, not in run(). The core builds the home
     * screen during start(), which is after every app's init() but before any app's
     * run(), and the launcher's icon widget takes the image resource when it is
     * created — so an icon assigned in run() arrives after the widget that shows it.
     *
     * Painting it here is safe: it touches no radio and registers nothing, so it is
     * not the app-install ordering problem described below. */
    const lv_image_dsc_t *icon = ble_launcher_icon();
    if (icon != nullptr) {
        const ESP_Brookesia_StyleImage_t image = ESP_BROOKESIA_STYLE_IMAGE(icon);
        setLauncherIconImage(image);
    }

    /* Nothing is registered with the co-processor here. The Wi-Fi toolbox found that
     * subscribing at app-install time — roughly 3 s in, before esp-hosted has reset
     * and enumerated the co-processor — makes the Wi-Fi service's own esp_wifi_init()
     * fail and the app abort on it. Bringing NimBLE up against a controller that has
     * not been enumerated yet is the same class of mistake, so it waits for run(). */
    return true;
}

bool BleToolboxApp::run(void)
{
    lv_area_t area = getVisualArea();

    /* One container per screen, created up front so switching is a visibility change
     * rather than a rebuild.
     *
     * These are plain viewports holding exactly one panel each: no layout is set, so
     * an object with no flex or grid flow leaves its children where lv_obj_align puts
     * them, which is what the panels need. The Wi-Fi app learned this the hard way —
     * flex columns on the containers took over the child's placement once the panels
     * were nested inside them.
     *
     * The menu is the exception: it draws into its container directly and gives it a
     * flex column of its own. */
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (screens[i] == nullptr) {
            screens[i] = lv_obj_create(lv_screen_active());
            lv_obj_set_size(screens[i], area.x2 - area.x1 + 1, area.y2 - area.y1 + 1);
            lv_obj_align(screens[i], LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_pad_all(screens[i], 0, 0);
            lv_obj_set_style_border_width(screens[i], 0, 0);
            lv_obj_clear_flag(screens[i], LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    buildMenu();
    buildScan();
    buildObserver();
    buildAirTag();
    buildConnect();
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    buildSpam();
#endif

    showScreen(SCREEN_MENU);

    if (ui_timer == nullptr) {
        /* 500 ms: fast enough that the lists keep up with a scan and the spam countdown
         * is honest, slow enough to be free. The only place LVGL is touched from this
         * app's state — and while the spam screen is transmitting, also the clock that
         * drives ble_toolbox_spam_tick(). */
        /* 100 ms: fast enough that keyboard notifications redraw promptly, while the
         * scan/observer list rebuilds are still throttled by list_dirty rather than
         * rebuilt every tick. */
        ui_timer = lv_timer_create(onTick, 100, this);
    }

    lv_obj_clear_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

    /* A keypad input device lets a connected BLE HID keyboard drive focus and selection.
     * The key state is shared with the notification handler above. */
    if (g_lv_keypad_indev == nullptr) {
        g_lv_keypad_indev = lv_indev_create();
        lv_indev_set_type(g_lv_keypad_indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(g_lv_keypad_indev, tb_keypad_read);
        lv_indev_set_disp(g_lv_keypad_indev, lv_disp_get_default());
    }
    if (g_lv_group == nullptr) {
        g_lv_group = lv_group_create();
    }
    if (g_lv_keypad_indev != nullptr && g_lv_group != nullptr) {
        lv_indev_set_group(g_lv_keypad_indev, g_lv_group);
    }

    static TaskHandle_t connect_task = nullptr;
    if (connect_task == nullptr) {
        xTaskCreate(tb_connect_task, "ble_connect", 4096, nullptr, 3, &connect_task);
    }

    return true;
}

bool BleToolboxApp::back(void)
{
    if (active_screen != SCREEN_MENU) {
        showScreen(SCREEN_MENU);
        return true;
    }

    notifyCoreClosed();
    return true;
}

bool BleToolboxApp::close(void)
{
    /* A scan outliving the screen is the kind of leftover the spec rules out for the
     * Wi-Fi toolbox (section 9), and a radio left scanning is the same fault: it costs
     * power and it keeps the co-processor busy for an app nobody is looking at. */
    if (scanning) {
        (void)ble_toolbox_host_scan_stop();
        scanning = false;
    }

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    /* Disarm first, and unconditionally. The tick enforces the expiry too, but the app
     * being closed means nobody is watching the countdown, and a transmitter running
     * behind a closed app is the one outcome that must not be possible. */
    if (ble_toolbox_spam_is_armed()) {
        (void)ble_toolbox_spam_disarm();
    }

    /* The arming dialog lives on lv_layer_top(), not on this app's default screen, so
     * neither the core's cleanDefaultScreen() nor anything else will remove it. Closing
     * with the dialog open would otherwise leave the mask covering the launcher. */
    if (spam_modal != nullptr) {
        lv_obj_del(spam_modal);
        spam_modal = nullptr;
    }
#endif

    if (ui_timer != nullptr) {
        lv_timer_del(ui_timer);
        ui_timer = nullptr;
    }

    /* Every LVGL object this app created lives under the core's default screen, which
     * the framework deletes when the app closes. The pointers to them have to go in
     * the same breath, or the next run() sees a non-null pointer, skips rebuilding,
     * and hands freed objects to lv_obj_add_event_cb. */
    for (int i = 0; i < SCREEN_COUNT; i++) {
        screens[i] = nullptr;
    }

    status_label = nullptr;

    scan_state_label = nullptr;
    scan_count_label = nullptr;
    scan_list = nullptr;
    scan_start_btn = nullptr;
    scan_stop_btn = nullptr;

    obs_count_label = nullptr;
    obs_log = nullptr;
    obs_start_btn = nullptr;
    obs_stop_btn = nullptr;

    tag_count_label = nullptr;
    tag_list = nullptr;
    tag_start_btn = nullptr;
    tag_stop_btn = nullptr;
    tag_glasses_label = nullptr;

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    spam_family_dd = nullptr;
    spam_state_label = nullptr;
    spam_count_label = nullptr;
    spam_payload_label = nullptr;
    spam_start_btn = nullptr;
    spam_stop_btn = nullptr;
    /* spam_modal was already deleted above and nulled with it. */
#endif

    active_screen = SCREEN_MENU;

    return true;
}

bool BleToolboxApp::ensureService(void)
{
    if (service_up) {
        return true;
    }

    ble_toolbox_host_callbacks_t cbs = {};
    cbs.on_adv = onAdv;
    cbs.on_raw = onRawAdv;
    cbs.on_scan_state = onScanState;
    cbs.on_conn_state = onConnState;
    cbs.on_conn_svc = onConnSvc;
    cbs.on_conn_chr = onConnChr;
    cbs.on_conn_read = onConnRead;
    cbs.on_conn_notify = onConnNotify;

    const esp_err_t err = ble_toolbox_host_init(&cbs, this);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE service init failed: %s", esp_err_to_name(err));
        return false;
    }

    service_up = true;
    ESP_LOGI(TAG, "BLE service brought up on first use");
    return true;
}

/* ---- Service callbacks: latch only ---------------------------------------
 *
 * These run on NimBLE's host task. Touching LVGL from here would race the UI task,
 * so they copy what they need into the latches and set a flag; the timer drains them.
 * They also must not block — a slow callback stalls the controller's event handling
 * and costs advertisements. */

void BleToolboxApp::latchAdv(const ble_toolbox_adv_t *adv)
{
    scan_total++;

    /* The Scan screen is for connecting, so only connectable devices are kept. A
     * non-connectable beacon is noise here (it is the Observer's job to log it). */
    if (!adv->connectable) {
        return;
    }

    /* Duplicate filtering is on in the scan parameters, but a device that stops and
     * restarts advertising reappears; replacing its existing row keeps the list
     * showing devices rather than history. */
    int slot = -1;
    for (int i = 0; i < scan_latch_len; i++) {
        if (memcmp(scan_latch[i].adv.addr, adv->addr, 6) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* New device: append to the end. The list is deliberately NOT sorted — a
         * stable list is what lets the user aim at a Connect button without the row
         * moving under their finger. Once full, further new devices are dropped. */
        if (scan_latch_len >= kScanCap) {
            return;
        }
        slot = scan_latch_len++;
    }

    /* An existing device updates in place: the RSSI changes but the row stays put. */
    scan_latch[slot].adv = *adv;
}

void BleToolboxApp::latchRaw(const ble_toolbox_raw_adv_t *raw)
{
    obs_total++;

    ble_toolbox_adv_info_t info;
    ble_toolbox_adv_decode(raw->data, raw->data_len, &info);

    /* Two screens want different subsets, so the raw latch keeps everything and each
     * screen filters. The observer log is newest-first, which is what a sniffer is
     * read for. */
    if (obs_latch_len < kObsCap) {
        obs_latch_len++;
    }
    for (int i = obs_latch_len - 1; i > 0; i--) {
        obs_latch[i] = obs_latch[i - 1];
    }
    obs_latch[0].raw = *raw;
    obs_latch[0].info = info;

    if (info.kind == BLE_ADV_APPLE_FINDMY) {
        tag_total++;

        int slot = -1;
        for (int i = 0; i < tag_latch_len; i++) {
            if (memcmp(tag_latch[i].adv.addr, raw->addr, 6) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            if (tag_latch_len < kScanCap) {
                slot = tag_latch_len++;
                memset(&tag_latch[slot].adv, 0, sizeof(tag_latch[slot].adv));
            }
        }
        if (slot >= 0) {
            memcpy(tag_latch[slot].adv.addr, raw->addr, 6);
            snprintf(tag_latch[slot].adv.addr_str, sizeof(tag_latch[slot].adv.addr_str),
                     "%s", raw->addr_str);
            tag_latch[slot].adv.rssi = raw->rssi;
        }
    } else if (info.kind == BLE_ADV_SMART_GLASSES) {
        glasses_total++;
    }

    list_dirty = true;
}

void BleToolboxApp::onAdv(const ble_toolbox_adv_t *adv, void *user)
{
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app != nullptr) {
        app->latchAdv(adv);
    }
}

void BleToolboxApp::onRawAdv(const ble_toolbox_raw_adv_t *adv, void *user)
{
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app != nullptr) {
        app->latchRaw(adv);
    }
}

void BleToolboxApp::onScanState(bool scanning_now, esp_err_t reason, void *user)
{
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app == nullptr) {
        return;
    }

    app->scanning = scanning_now;

    if (!scanning_now && reason != ESP_OK) {
        /* The controller reset under us. Worth saying rather than leaving a screen
         * that looks like it is still listening. */
        app->service_up = false;
    }
}

void BleToolboxApp::onConnState(ble_toolbox_conn_state_t state, esp_err_t reason, void *user)
{
    (void)reason;
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app == nullptr) {
        return;
    }

    app->conn_state = state;

    if (state == BLE_TOOLBOX_CONN_DISCONNECTED) {
        app->conn_fail_status = (int)reason;   /* BLE status on a failed connect, 0 otherwise */
        g_lv_key = 0;                          /* a dropped connection releases any held key */
        g_lv_key_pressed = false;

        /* Auto-retry a failed connect (reason != 0): the controller's scan-stop and the
         * peer's accept are both intermittent, so one attempt often fails. The connect task
         * re-waits for the scan auto-stop and re-connects. Bounded to avoid a storm. */
        if ((int)reason != 0 && g_conn_retries < 3) {
            g_conn_retries++;
            g_pending_connect = true;
            ESP_LOGW(TAG, "connect failed (%d); auto-retry %d/3", (int)reason, g_conn_retries);
        } else if ((int)reason == 0) {
            g_conn_retries = 0;                 /* a clean disconnect resets the count */
        }
    } else if (state == BLE_TOOLBOX_CONN_CONNECTED) {
        app->conn_fail_status = 0;
        g_conn_retries = 0;
    }

    if (state == BLE_TOOLBOX_CONN_CONNECTED) {
        /* A fresh connection begins at the service level, auto-discovered. */
        app->conn_nav = 0;
        app->svc_latch_len = 0;
        app->chr_latch_len = 0;
        g_show_connect = true;   /* switch to the connect screen on the next tick */
        (void)ble_toolbox_host_discover_services();
    }
    app->list_dirty = true;
}

void BleToolboxApp::onConnSvc(const ble_toolbox_gatt_svc_t *svc, void *user)
{
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app != nullptr) {
        app->latchSvc(svc);
    }
}

void BleToolboxApp::onConnChr(const ble_toolbox_gatt_chr_t *chr, void *user)
{
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app != nullptr) {
        app->latchChr(chr);
    }
}

void BleToolboxApp::onConnRead(uint16_t val_handle, const uint8_t *data, uint16_t len, void *user)
{
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app != nullptr) {
        app->latchRead(val_handle, data, len);
    }
}

void BleToolboxApp::latchSvc(const ble_toolbox_gatt_svc_t *svc)
{
    if (svc != nullptr && svc_latch_len < (int)kSvcCap) {
        svc_latch[svc_latch_len++] = *svc;
    } else if (svc == nullptr) {
        /* Services complete: mark the HID service for the timer, which then discovers
         * its characteristics. The GATT client operation runs on the app's own task —
         * calling it here on NimBLE's task would nest two client procedures. */
        for (int i = 0; i < svc_latch_len; i++) {
            if (strstr(svc_latch[i].uuid_str, "1812") != nullptr) {
                conn_svc_start = svc_latch[i].start_handle;
                conn_svc_end = svc_latch[i].end_handle;
                chr_latch_len = 0;
                auto_discover_chars = true;
                break;
            }
        }
    }
    list_dirty = true;          /* the NULL sentinel also lands here */
}

void BleToolboxApp::latchChr(const ble_toolbox_gatt_chr_t *chr)
{
    if (chr != nullptr && chr_latch_len < (int)kChrCap) {
        chr_latch[chr_latch_len++] = *chr;
    } else if (chr == nullptr) {
        /* Characteristics complete: mark the Report characteristic for the timer, which
         * then subscribes to it. */
        for (int i = 0; i < chr_latch_len; i++) {
            if (strstr(chr_latch[i].uuid_str, "2a4d") != nullptr) {
                conn_read_handle = chr_latch[i].val_handle;
                auto_subscribe = true;
                break;
            }
        }
    }
    list_dirty = true;
}

void BleToolboxApp::latchRead(uint16_t val_handle, const uint8_t *data, uint16_t len)
{
    conn_read_handle = val_handle;
    conn_read_len = 0;
    conn_read_buf[0] = '\0';

    if (data != nullptr && len > 0) {
        if (len > sizeof(conn_read_buf) - 1) {
            len = sizeof(conn_read_buf) - 1;
        }
        memcpy(conn_read_buf, data, len);
        conn_read_buf[len] = '\0';
        conn_read_len = len;
    }
    list_dirty = true;
}

void BleToolboxApp::onConnNotify(uint16_t attr_handle, const uint8_t *data, uint16_t len,
                                 bool indication, void *user)
{
    BleToolboxApp *app = (BleToolboxApp *)user;
    if (app != nullptr) {
        app->latchNotify(attr_handle, data, len, indication);
    }
}

/* A HID keyboard usage ID as a printable name; empty for "no key". */
static const char *tb_hid_key_name(uint8_t code)
{
    switch (code) {
    case 0x28: return "Enter";
    case 0x29: return "Esc";
    case 0x2A: return "Backspace";
    case 0x2B: return "Tab";
    case 0x2C: return "Space";
    case 0x2D: return "-";
    case 0x2E: return "=";
    case 0x2F: return "[";
    case 0x30: return "]";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x4F: return "Right";
    case 0x50: return "Left";
    case 0x51: return "Down";
    case 0x52: return "Up";
    default: break;
    }
    static char one[2];
    if (code >= 0x04 && code <= 0x1D) {
        one[0] = (char)('a' + (code - 0x04));
        one[1] = '\0';
        return one;
    }
    if (code >= 0x1E && code <= 0x27) {
        one[0] = "1234567890"[code - 0x1E];
        one[1] = '\0';
        return one;
    }
    return "";
}

void BleToolboxApp::latchNotify(uint16_t attr_handle, const uint8_t *data, uint16_t len,
                                bool indication)
{
    conn_notify_handle = attr_handle;
    (void)indication;

    char *p = conn_notify_buf;
    const char *const end = conn_notify_buf + sizeof(conn_notify_buf) - 1;

    /* Hex, then a best-effort HID keyboard decoding. */
    for (uint16_t i = 0; i < len && p + 4 < end; i++) {
        const int w = snprintf(p, end - p, "%02x ", data[i]);
        if (w <= 0) {
            break;
        }
        p += w;
    }

    bool any_key = false;
    if (len >= 3) {
        const uint8_t mods = data[0];
        for (uint16_t i = 2; i < len; i++) {
            if (data[i] != 0) {
                any_key = true;
                break;
            }
        }
        if (mods != 0) {
            any_key = true;
        }

        if (p + 1 < end) {
            *p++ = '|';
            *p++ = ' ';
        }
        if (any_key) {
            int keys = 0;
            for (uint16_t i = 2; i < len && keys < 6; i++) {
                const uint8_t code = data[i];
                const char *name = tb_hid_key_name(code);
                if (name[0] == '\0') {
                    continue;
                }
                if (keys > 0 && p + 1 < end) {
                    *p++ = '+';
                }
                const size_t n = strlen(name);
                if (p + n < end) {
                    memcpy(p, name, n);
                    p += n;
                    keys++;
                }
            }
        } else if (p + 8 < end) {
            memcpy(p, "(release)", 9);
            p += 9;
        }
    }

    *p = '\0';

    /* Route the first pressed key into the LVGL keypad input device, so a keyboard
     * drives focus and selection like a wired one. */
    if (len >= 3) {
        const uint32_t lv = tb_hid_to_lv_key(data[2]);
        g_lv_key = lv;
        g_lv_key_pressed = (lv != 0);
        if (lv != 0) {
            ESP_LOGW(TAG, "key hid=0x%02x -> lv=0x%lx %s", data[2], (unsigned long)lv,
                     g_lv_key_pressed ? "pressed" : "released");
        }
    } else {
        g_lv_key_pressed = false;
    }

    list_dirty = true;

    /* Update the value label right away so a key press shows without waiting for the
     * next timer tick. The display lock is a best-effort try-lock here (this runs on
     * NimBLE's task); when it fails, refreshConnect still renders it on the timer. */
    if (conn_value_label != nullptr && bsp_display_lock(0)) {
        char v[128];
        snprintf(v, sizeof(v), "0x%04x: %s", conn_notify_handle, conn_notify_buf);
        lv_label_set_text(conn_value_label, v);
        bsp_display_unlock();
    }
}

/* ---- Status and refresh -------------------------------------------------- */

void BleToolboxApp::updateStatus(void)
{
    if (status_label == nullptr) {
        return;
    }

    char text[96];
    snprintf(text, sizeof(text), "%s  -  %s  -  %lu adv",
             service_up ? "BLE ready" : "BLE down",
             scanning ? "scanning" : "idle",
             (unsigned long)scan_total);
    lv_label_set_text(status_label, text);
}

void BleToolboxApp::refreshScan(void)
{
    if (scan_list == nullptr) {
        return;
    }

    /* Rebuilding the list would otherwise snap the scroll back to the top. Remember
     * where the user was and restore it after the rows are rebuilt. */
    const lv_coord_t saved_scroll = lv_obj_get_scroll_y(scan_list);
    lv_obj_clean(scan_list);

    if (scan_latch_len == 0) {
        lv_obj_t *empty = lv_label_create(scan_list);
        lv_label_set_text(empty, "no connectable devices yet");
        return;
    }

    for (int i = 0; i < scan_latch_len; i++) {
        const ble_toolbox_adv_t *a = &scan_latch[i].adv;

        lv_obj_t *row = lv_obj_create(scan_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 52);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, a->name[0] != '\0' ? a->name : "(no name)");
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -8);

        lv_obj_t *addr = lv_label_create(row);
        lv_obj_set_style_text_font(addr, TOOLBOX_FONT_DETAIL, 0);
        lv_label_set_text(addr, a->addr_str);
        lv_obj_align(addr, LV_ALIGN_LEFT_MID, 0, 10);

        /* The Connect button is the action; the row itself is not clickable. */
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 84, 36);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t *btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, "Connect");
        lv_obj_center(btn_label);
        lv_obj_add_event_cb(btn, onScanRowClick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tb_group_add(btn);
    }

    char count[48];
    snprintf(count, sizeof(count), "%d device%s", scan_latch_len, scan_latch_len == 1 ? "" : "s");
    lv_label_set_text(scan_count_label, count);

    /* Restore the scroll position the user was at before this rebuild. */
    lv_obj_update_layout(scan_list);
    lv_obj_scroll_to_y(scan_list, saved_scroll, LV_ANIM_OFF);
}

void BleToolboxApp::onScanRowClick(lv_event_t *e)
{
    const int slot = (int)(intptr_t)lv_event_get_user_data(e);
    BleToolboxApp *app = g_app;
    ESP_LOGW(TAG, "scan row click slot=%d len=%d", slot,
             app != nullptr ? (int)app->scan_latch_len : -1);
    if (app == nullptr || slot < 0 || slot >= app->scan_latch_len) {
        return;
    }
    app->connectTo(&app->scan_latch[slot].adv);
}

void BleToolboxApp::connectTo(const ble_toolbox_adv_t *adv)
{
    ESP_LOGW(TAG, "connectTo %s connectable=%d random=%d", adv->addr_str, (int)adv->connectable,
             (int)adv->random_addr);
    if (!adv->connectable) {
        ESP_LOGW(TAG, "  not connectable; ignoring");
        if (scan_state_label != nullptr) {
            lv_label_set_text(scan_state_label, "not connectable");
        }
        return;
    }

    /* Latch the address; the connect task waits (blocked on the scan-done semaphore) for
     * the scan's 5 s auto-stop, then connects. The tap returns immediately so the UI stays
     * responsive. The watcher is deliberately left running until the auto-stop lands. */
    scanning = false;

    svc_latch_len = 0;
    chr_latch_len = 0;
    conn_nav = 0;
    conn_read_len = 0;
    conn_read_handle = 0;
    conn_notify_handle = 0;
    conn_notify_buf[0] = '\0';
    auto_discover_chars = false;
    auto_subscribe = false;

    conn_state = BLE_TOOLBOX_CONN_CONNECTING;
    conn_fail_status = 0;
    g_show_connect = false;
    g_conn_retries = 0;
    memcpy(g_pending_addr, adv->addr, 6);
    g_pending_random = adv->random_addr;
    g_pending_connect = true;

    if (scan_state_label != nullptr) {
        lv_label_set_text(scan_state_label, "connecting...");
    }
}

void BleToolboxApp::onConnRowClick(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    BleToolboxApp *app = g_app;
    if (app == nullptr || app->conn_state != BLE_TOOLBOX_CONN_CONNECTED) {
        return;
    }

    if (app->conn_nav == 0) {
        /* A service: discover its characteristics. */
        if (idx < 0 || idx >= app->svc_latch_len) {
            return;
        }
        app->conn_svc_start = app->svc_latch[idx].start_handle;
        app->conn_svc_end = app->svc_latch[idx].end_handle;
        app->chr_latch_len = 0;
        app->conn_nav = 1;
        (void)ble_toolbox_host_discover_chars(app->conn_svc_start, app->conn_svc_end);
    } else {
        /* A characteristic: read its value. */
        if (idx < 0 || idx >= app->chr_latch_len) {
            return;
        }
        app->conn_read_handle = app->chr_latch[idx].val_handle;
        (void)ble_toolbox_host_read(app->chr_latch[idx].val_handle);
    }
}

void BleToolboxApp::refreshConnect(void)
{
    if (conn_state_label != nullptr) {
        char failbuf[40];
        const char *txt = "disconnected";
        if (conn_state == BLE_TOOLBOX_CONN_CONNECTING) {
            txt = "connecting...";
        } else if (conn_state == BLE_TOOLBOX_CONN_CONNECTED) {
            txt = "connected";
        } else if (conn_fail_status != 0) {
            snprintf(failbuf, sizeof(failbuf), "connect failed (0x%x)", (unsigned)conn_fail_status);
            txt = failbuf;
        }
        lv_label_set_text(conn_state_label, txt);
    }

    if (conn_list == nullptr) {
        return;
    }

    lv_obj_clean(conn_list);

    if (conn_nav == 0) {
        if (svc_latch_len == 0) {
            lv_obj_t *l = lv_label_create(conn_list);
            lv_label_set_text(l, conn_state == BLE_TOOLBOX_CONN_CONNECTED
                                 ? "discovering services..." : "not connected");
            return;
        }
        for (int i = 0; i < svc_latch_len; i++) {
            lv_obj_t *row = lv_obj_create(conn_list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 44);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, onConnRowClick, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *l = lv_label_create(row);
            char txt[64];
            snprintf(txt, sizeof(txt), "%s  [0x%04x-0x%04x]", svc_latch[i].uuid_str,
                     svc_latch[i].start_handle, svc_latch[i].end_handle);
            lv_label_set_text(l, txt);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        }
    } else {
        if (chr_latch_len == 0) {
            lv_obj_t *l = lv_label_create(conn_list);
            lv_label_set_text(l, "no characteristics");
            return;
        }
        for (int i = 0; i < chr_latch_len; i++) {
            lv_obj_t *row = lv_obj_create(conn_list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 44);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, onConnRowClick, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *l = lv_label_create(row);
            char txt[80];
            snprintf(txt, sizeof(txt), "%s  h=0x%04x", chr_latch[i].uuid_str,
                     chr_latch[i].val_handle);
            lv_label_set_text(l, txt);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        }
    }

    if (conn_value_label != nullptr) {
        if (conn_notify_handle != 0 && conn_notify_buf[0] != '\0') {
            char v[128];
            snprintf(v, sizeof(v), "0x%04x: %s", conn_notify_handle, conn_notify_buf);
            lv_label_set_text(conn_value_label, v);
        } else if (conn_read_len > 0) {
            char v[128];
            snprintf(v, sizeof(v), "0x%04x: %s", conn_read_handle, conn_read_buf);
            lv_label_set_text(conn_value_label, v);
        } else if (conn_read_handle != 0) {
            char v[32];
            snprintf(v, sizeof(v), "0x%04x: (empty)", conn_read_handle);
            lv_label_set_text(conn_value_label, v);
        }
    }
}

void BleToolboxApp::refreshObserver(void)
{
    if (obs_log == nullptr) {
        return;
    }

    /* Same row layout as the Scan screen: the decoded advertisement kind sits where the Scan
     * puts the device name (top line), the address is the bottom line, and the RSSI is
     * right-aligned. The ORDER is deliberately unchanged: the Observer stays newest-first. */
    lv_obj_clean(obs_log);

    if (obs_latch_len == 0) {
        toolbox_make_log_line(obs_log, "nothing heard yet");
    } else {
        for (int i = 0; i < obs_latch_len; i++) {
            const ObsEntry *e = &obs_latch[i];

            lv_obj_t *row = lv_obj_create(obs_log);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 52);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *kind = lv_label_create(row);
            lv_label_set_text(kind, ble_toolbox_adv_kind_name(e->info.kind));
            lv_obj_align(kind, LV_ALIGN_LEFT_MID, 0, -8);

            lv_obj_t *addr = lv_label_create(row);
            lv_obj_set_style_text_font(addr, TOOLBOX_FONT_DETAIL, 0);
            lv_label_set_text(addr, e->raw.addr_str);
            lv_obj_align(addr, LV_ALIGN_LEFT_MID, 0, 10);

            lv_obj_t *rssi = lv_label_create(row);
            lv_obj_set_style_text_font(rssi, TOOLBOX_FONT_DETAIL, 0);
            char r[32];
            snprintf(r, sizeof(r), "%d dBm", e->raw.rssi);
            lv_label_set_text(rssi, r);
            lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    /* Newest first, which is what a sniffer is read for: the latch is built newest-to-
     * oldest, so no reversal is needed here. */
    char count[80];
    snprintf(count, sizeof(count), "%lu frames heard, newest %d shown",
             (unsigned long)obs_total, obs_latch_len);
    lv_label_set_text(obs_count_label, count);
}

void BleToolboxApp::refreshAirTag(void)
{
    if (tag_list == nullptr) {
        return;
    }

    lv_obj_clean(tag_list);

    if (tag_latch_len == 0) {
        lv_obj_t *empty = lv_label_create(tag_list);
        lv_label_set_text(empty, "no Find My trackers heard");
    } else {
        for (int i = 0; i < tag_latch_len; i++) {
            const ble_toolbox_adv_t *a = &tag_latch[i].adv;

            lv_obj_t *row = lv_obj_create(tag_list);
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, 40);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *addr = lv_label_create(row);
            lv_obj_set_style_text_font(addr, TOOLBOX_FONT_DETAIL, 0);
            lv_label_set_text(addr, a->addr_str);
            lv_obj_align(addr, LV_ALIGN_LEFT_MID, 0, 0);

            lv_obj_t *rssi = lv_label_create(row);
            lv_obj_set_style_text_font(rssi, TOOLBOX_FONT_DETAIL, 0);
            char r[24];
            snprintf(r, sizeof(r), "%d dBm", a->rssi);
            lv_label_set_text(rssi, r);
            lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    char count[64];
    snprintf(count, sizeof(count), "%lu tracker advert(s)", (unsigned long)tag_total);
    lv_label_set_text(tag_count_label, count);

    char glasses[64];
    snprintf(glasses, sizeof(glasses), "possible smart glasses: %lu",
             (unsigned long)glasses_total);
    lv_label_set_text(tag_glasses_label, glasses);
}

/* Clear everything the three screens count or list, so a run starts from zero.
 *
 * All three, not just the one whose Start was pressed: they are views of one scan, and
 * leaving two of them showing the previous run's totals while the third restarts would be
 * worse than either resetting all of them or none.
 *
 * The counters are written from the service's callbacks, which run on NimBLE's task, so
 * this races them — a frame arriving at this instant can be counted and then have the
 * count cleared. That direction is the harmless one: the effect is one frame missing from
 * a total that is about to be re-earned, not a corrupt latch. A critical section would
 * close it and also stall the radio for the duration, which is the worse trade here.
 *
 * The visible labels are updated by the next tick, so nothing needs doing to them beyond
 * marking the list dirty. */
void BleToolboxApp::resetCounters(void)
{
    scan_latch_len = 0;
    scan_total = 0;

    obs_latch_len = 0;
    obs_total = 0;

    tag_latch_len = 0;
    tag_total = 0;
    glasses_total = 0;

    list_dirty = true;
}

void BleToolboxApp::onTick(lv_timer_t *timer)
{
    BleToolboxApp *app = (BleToolboxApp *)timer->user_data;
    if (app == nullptr) {
        return;
    }

    app->updateStatus();

    if (g_show_connect) {
        g_show_connect = false;
        app->showScreen(BleToolboxApp::SCREEN_CONNECT);
        app->refreshConnect();
    }

    /* Auto-subscribe runs here, on the app's task, so the GATT client operations do not
     * nest inside NimBLE's own discovery callbacks. */
    if (app->auto_discover_chars) {
        app->auto_discover_chars = false;
        if (app->conn_state == BLE_TOOLBOX_CONN_CONNECTED) {
            (void)ble_toolbox_host_discover_chars(app->conn_svc_start, app->conn_svc_end);
        }
    }
    if (app->auto_subscribe) {
        app->auto_subscribe = false;
        if (app->conn_state == BLE_TOOLBOX_CONN_CONNECTED && app->conn_read_handle != 0) {
            (void)ble_toolbox_host_subscribe(app->conn_read_handle, false);
        }
    }

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    /* The transmitting clock. Driven from here rather than a timer of its own so there
     * is one cadence to reason about, and it runs whether or not the Spam screen is
     * showing — the tick enforces its own expiry, so a user who navigates away while
     * armed still gets stopped on time. */
    if (ble_toolbox_spam_is_armed()) {
        (void)ble_toolbox_spam_tick();

        if (app->active_screen == SCREEN_SPAM) {
            app->refreshSpam();
        }
    }
#endif

    /* Rebuild only the visible screen, and only when something arrived. Rebuilding all
     * of them every 500 ms would churn LVGL objects for screens nobody is looking at. */
    if (!app->list_dirty) {
        return;
    }
    app->list_dirty = false;

    switch (app->active_screen) {
    case SCREEN_SCAN:
        app->refreshScan();
        break;
    case SCREEN_OBSERVER:
        app->refreshObserver();
        break;
    case SCREEN_AIRTAG:
        app->refreshAirTag();
        break;
    case SCREEN_CONNECT:
        app->refreshConnect();
        break;
    default:
        break;
    }
}

/* ---- Screens ------------------------------------------------------------- */

void BleToolboxApp::showScreen(Screen screen)
{
    active_screen = screen;

    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (screens[i] == nullptr) {
            continue;
        }
        if (i == (int)screen) {
            lv_obj_clear_flag(screens[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(screens[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Populate the screen that just appeared, so it is never blank while waiting for
     * the next tick. */
    list_dirty = true;
    switch (screen) {
    case SCREEN_SCAN:     refreshScan();     break;
    case SCREEN_OBSERVER: refreshObserver(); break;
    case SCREEN_AIRTAG:   refreshAirTag();   break;
    case SCREEN_CONNECT:  refreshConnect();  break;
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    case SCREEN_SPAM:     refreshSpam();     break;
#endif
    default: break;
    }
}

void BleToolboxApp::buildMenu(void)
{
    lv_area_t area = getVisualArea();

    /* The menu draws straight into its container, like the Wi-Fi app's, and is the one
     * screen not nesting a panel. That is a compromise with a cost worth naming: it
     * means this container must not also be measured as if it held a panel. */
    lv_obj_t *panel = screens[SCREEN_MENU];
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "BLE Toolbox");
    /* The app's own title, one step above its menu entries so the hierarchy still reads:
     * entries are 28, screen titles 26. It was 24, which the enlarged entries overtook. */
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);

    status_label = lv_label_create(panel);
    lv_obj_set_style_text_font(status_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(status_label, "BLE not started");

    /* Only three entries without the transmit path, four with it. The entry appears
     * because the capability was compiled in, not because a flag hides a button — which
     * is the distinction spec section 10.5 draws for the Wi-Fi toolbox's injection. */
    static const char *entries[4] = {
        "BLE Scan",
        "BLE Observer",
        "AirTag Monitor",
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
        "BLE Spam",
#else
        "",
#endif
    };
    static const int codes[4] = { ACT_MENU_SCAN, ACT_MENU_OBSERVER, ACT_MENU_AIRTAG,
                                  ACT_MENU_SPAM };

    const int entry_count =
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
        4;
#else
        3;
#endif

    /* Menu entries are the app's navigation and carry a font of their own rather than the
     * theme's, so they read at a glance. 28 pt is chosen from the measurement printed
     * below, not by eye: the panel offers 660 px per entry and the longest entry here
     * needs a fraction of that, so the ceiling is the column's height rather than its
     * width. The buttons grew with the text — at 72 px they suit a 28 px line, and the
     * panel scrolls if the column ever outgrows the screen. */
    for (int i = 0; i < entry_count; i++) {
        lv_obj_t *btn = toolbox_make_button(panel, entries[i], onEvent,
                                            (void *)(intptr_t)codes[i], 72,
                                            &lv_font_montserrat_28);
        tb_group_add(btn);
    }

    if (toolbox_measure_menu(panel, TAG) > 0) {
        ESP_LOGW(TAG, "  some menu entries do not fit their button at this font size");
    }

    lv_obj_t *note = lv_label_create(panel);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(note, TOOLBOX_FONT_DETAIL, 0);
    /* ASCII only: LVGL's Montserrat fonts carry U+0020..U+007E and nothing until
     * U+00B0, so a typographic dash draws as a placeholder box. */
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    lv_label_set_text(note,
                      "Scans and decodes BLE advertisements. BLE Spam transmits and is "
                      "gated behind an authorisation dialog.");
#else
    lv_label_set_text(note,
                      "Scans and decodes BLE advertisements. This build has no "
                      "transmit path.");
#endif
}

void BleToolboxApp::buildScan(void)
{
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = toolbox_make_screen(screens[SCREEN_SCAN], area, "BLE Scan",
                                          LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)ACT_BACK);

    scan_state_label = lv_label_create(panel);
    lv_obj_set_style_text_font(scan_state_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(scan_state_label, "idle");

    scan_count_label = lv_label_create(panel);
    lv_obj_set_style_text_font(scan_count_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(scan_count_label, "0 devices");

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    scan_start_btn = toolbox_make_button(buttons, "Start", onEvent,
                                         (void *)(intptr_t)ACT_SCAN_START, 44);
    lv_obj_set_flex_grow(scan_start_btn, 1);
    scan_stop_btn = toolbox_make_button(buttons, "Stop", onEvent,
                                        (void *)(intptr_t)ACT_SCAN_STOP, 44);
    lv_obj_set_flex_grow(scan_stop_btn, 1);
    tb_group_add(scan_start_btn);
    tb_group_add(scan_stop_btn);

    /* The list scrolls inside the panel, which already scrolls. Nesting two scrollers
     * is deliberate here: the buttons and the count stay put while the list moves. */
    scan_list = lv_obj_create(panel);
    lv_obj_set_width(scan_list, LV_PCT(100));
    lv_obj_set_flex_grow(scan_list, 1);
    lv_obj_set_flex_flow(scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scan_list, 0, 0);
    lv_obj_set_style_pad_row(scan_list, 4, 0);
    lv_obj_set_scroll_dir(scan_list, LV_DIR_VER);
}

void BleToolboxApp::buildObserver(void)
{
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = toolbox_make_screen(screens[SCREEN_OBSERVER], area, "BLE Observer",
                                          LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)ACT_BACK);

    obs_count_label = lv_label_create(panel);
    lv_obj_set_style_text_font(obs_count_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(obs_count_label, "0 frames");

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    obs_start_btn = toolbox_make_button(buttons, "Start", onEvent,
                                        (void *)(intptr_t)ACT_OBS_START, 44);
    lv_obj_set_flex_grow(obs_start_btn, 1);
    obs_stop_btn = toolbox_make_button(buttons, "Stop", onEvent,
                                       (void *)(intptr_t)ACT_OBS_STOP, 44);
    lv_obj_set_flex_grow(obs_stop_btn, 1);

    /* A scrolling list, not a single label. toolbox_make_log was a label, and a label
     * clips what it cannot show with no way to reach it: the Observer was never
     * scrollable, and raising the font size only increased how much was cut off. See the
     * helper for the detail. */
    obs_log = toolbox_make_log(panel);
}

void BleToolboxApp::buildConnect(void)
{
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = toolbox_make_screen(screens[SCREEN_CONNECT], area, "BLE Connect",
                                          LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)ACT_BACK);

    conn_state_label = lv_label_create(panel);
    lv_obj_set_style_text_font(conn_state_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(conn_state_label, "disconnected");

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    conn_disconnect_btn = toolbox_make_button(buttons, "Disconnect", onEvent,
                                              (void *)(intptr_t)ACT_CONN_DISCONNECT, 44);
    lv_obj_set_flex_grow(conn_disconnect_btn, 1);
    conn_back_btn = toolbox_make_button(buttons, "Services", onEvent,
                                        (void *)(intptr_t)ACT_CONN_SERVICES, 44);
    lv_obj_set_flex_grow(conn_back_btn, 1);
    {
        lv_obj_t *write_btn = toolbox_make_button(buttons, "Write", onEvent,
                                                  (void *)(intptr_t)ACT_CONN_WRITE, 44);
        lv_obj_set_flex_grow(write_btn, 1);
    }
    {
        lv_obj_t *sub_btn = toolbox_make_button(buttons, "Subscribe", onEvent,
                                                (void *)(intptr_t)ACT_CONN_SUBSCRIBE, 44);
        lv_obj_set_flex_grow(sub_btn, 1);
    }

    /* The services / characteristics list, scrollable. */
    conn_list = lv_obj_create(panel);
    lv_obj_set_width(conn_list, LV_PCT(100));
    lv_obj_set_flex_grow(conn_list, 1);
    lv_obj_set_flex_flow(conn_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(conn_list, 0, 0);
    lv_obj_set_style_pad_row(conn_list, 4, 0);
    lv_obj_set_scroll_dir(conn_list, LV_DIR_VER);

    /* The read/notification value, pinned under the list. A fixed height and a solid
     * background keep it from being collapsed or hidden behind the scrollbar. */
    conn_value_label = lv_label_create(panel);
    lv_obj_set_style_text_font(conn_value_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_long_mode(conn_value_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(conn_value_label, LV_PCT(100));
    lv_obj_set_height(conn_value_label, 44);
    lv_obj_set_style_bg_opa(conn_value_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(conn_value_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_pad_all(conn_value_label, 4, 0);
    lv_label_set_text(conn_value_label, "");
}

/* ---- Spam screen (compiled out without the transmit path) ---------------- */

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM

void BleToolboxApp::buildSpam(void)
{
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = toolbox_make_screen(screens[SCREEN_SPAM], area, "BLE Spam",
                                          LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)ACT_BACK);

    /* The authorisation notice, on the screen itself rather than only in the dialog: the
     * dialog is dismissed once, this is what someone sees every time they come back. */
    lv_obj_t *notice = lv_label_create(panel);
    lv_obj_set_width(notice, LV_PCT(100));
    lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(notice, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(notice,
                      "Transmits imitation pairing advertisements for devices you do "
                      "not own. They can prompt pairing dialogs on other people's "
                      "phones. Use only where you are authorised to.");

    /* Family, then what will actually go out. Two rows because the payload cycles: the
     * family is the choice, the payload is what is on air right now. */
    lv_obj_t *frow = lv_obj_create(panel);
    lv_obj_set_width(frow, LV_PCT(100));
    lv_obj_set_height(frow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(frow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(frow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(frow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *flabel = lv_label_create(frow);
    lv_label_set_text(flabel, "Family");

    spam_family_dd = lv_dropdown_create(frow);
    lv_obj_set_flex_grow(spam_family_dd, 1);
    {
        /* Built from the family table so the two cannot drift apart. */
        char options[128] = { 0 };
        int n = 0;
        for (int i = 0; i < 4; i++) {
            n += snprintf(options + n, sizeof(options) - n, "%s%s",
                          (i == 0) ? "" : "\n", ble_toolbox_spam_family_names[i]);
        }
        lv_dropdown_set_options(spam_family_dd, options);
    }

    spam_payload_label = lv_label_create(panel);
    lv_obj_set_width(spam_payload_label, LV_PCT(100));
    lv_obj_set_style_text_font(spam_payload_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(spam_payload_label, "idle");

    spam_state_label = lv_label_create(panel);
    lv_obj_set_style_text_font(spam_state_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(spam_state_label, "not armed");

    spam_count_label = lv_label_create(panel);
    lv_obj_set_style_text_font(spam_count_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(spam_count_label, "0 advertisements sent");

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    spam_start_btn = toolbox_make_button(buttons, "Arm and start", onEvent,
                                         (void *)(intptr_t)ACT_SPAM_START, 44);
    lv_obj_set_flex_grow(spam_start_btn, 1);
    spam_stop_btn = toolbox_make_button(buttons, "Stop", onEvent,
                                        (void *)(intptr_t)ACT_SPAM_STOP, 44);
    lv_obj_set_flex_grow(spam_stop_btn, 1);
}

void BleToolboxApp::refreshSpam(void)
{
    if (spam_state_label == nullptr) {
        return;
    }

    if (ble_toolbox_spam_is_armed()) {
        const uint32_t left = ble_toolbox_spam_seconds_left();

        char state[64];
        /* The countdown is not decoration: it is the visible form of the cap, so someone
         * using this can see it will stop on its own rather than having to trust that it
         * will. */
        snprintf(state, sizeof(state), "TRANSMITTING - stops in %lu:%02lu",
                 (unsigned long)(left / 60), (unsigned long)(left % 60));
        lv_label_set_text(spam_state_label, state);

        const ble_toolbox_spam_payload_t *p =
            ble_toolbox_spam_payload(ble_toolbox_spam_current_index());
        if (p != nullptr) {
            char pay[96];
            snprintf(pay, sizeof(pay), "sending: %s (%s)", p->label,
                     ble_toolbox_spam_family_names[p->family]);
            lv_label_set_text(spam_payload_label, pay);
        }
    } else {
        lv_label_set_text(spam_state_label, "not armed");
        lv_label_set_text(spam_payload_label, "idle");
    }

    char count[64];
    snprintf(count, sizeof(count), "%lu advertisements sent",
             (unsigned long)ble_toolbox_spam_sent());
    lv_label_set_text(spam_count_label, count);
}

/* The arming dialog. Its own mask rather than the Wi-Fi app's, because the two apps
 * must not depend on each other, but the same shape and the same lesson: the mask is
 * created on lv_layer_top() so nothing else can be tapped, and its pointer is held so
 * close() can delete it. */
void BleToolboxApp::askSpamArm(void)
{
    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    spam_modal = mask;
    lv_obj_set_size(mask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(mask);
    lv_obj_set_width(box, LV_PCT(86));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_style_pad_row(box, 12, 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Confirm transmitting");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    /* The family chosen, named, rather than a generic warning: the decision being made
     * is which devices to imitate. */
    const uint16_t fam = lv_dropdown_get_selected(spam_family_dd);
    char body_text[320];
    snprintf(body_text, sizeof(body_text),
             "This will transmit imitation %s pairing advertisements from this "
             "device's radio.\n\n"
             "They can prompt pairing dialogs on phones nearby, including phones "
             "belonging to people who have not agreed to this.\n\n"
             "It stops on its own after %u minutes, and Stop ends it sooner.",
             ble_toolbox_spam_family_names[fam],
             (unsigned)(BLE_TOOLBOX_SPAM_MAX_HOLD_US / 60000000u));

    lv_obj_t *body = lv_label_create(box);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, body_text);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_btn_create(row);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, onSpamArmCancelled, LV_EVENT_CLICKED, mask);
    lv_obj_t *cancel_txt = lv_label_create(cancel);
    lv_label_set_text(cancel_txt, "Cancel");
    lv_obj_center(cancel_txt);

    lv_obj_t *ok = lv_btn_create(row);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_style_bg_color(ok, lv_color_make(200, 40, 40), 0);
    lv_obj_add_event_cb(ok, onSpamArmConfirmed, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_txt = lv_label_create(ok);
    lv_label_set_text(ok_txt, "I am authorised");
    lv_obj_center(ok_txt);
}

void BleToolboxApp::onSpamArmCancelled(lv_event_t *e)
{
    /* The mask is the user_data, so there is no walking up from the button. */
    lv_obj_t *mask = (lv_obj_t *)lv_event_get_user_data(e);

    if (g_app != nullptr) {
        g_app->spam_modal = nullptr;
    }

    if (mask != nullptr) {
        lv_obj_del(mask);
    }

    ESP_LOGI(TAG, "spam arming cancelled");
}

void BleToolboxApp::onSpamArmConfirmed(lv_event_t *e)
{
    (void)e;

    BleToolboxApp *app = g_app;
    if (app == nullptr) {
        return;
    }

    lv_obj_t *mask = app->spam_modal;
    app->spam_modal = nullptr;
    if (mask != nullptr) {
        lv_obj_del(mask);
    }

    if (!app->ensureService()) {
        lv_label_set_text(app->spam_state_label, "BLE service unavailable");
        return;
    }

    ble_toolbox_spam_params_t p = {};
    p.max_hold_us = BLE_TOOLBOX_SPAM_MAX_HOLD_US;
    p.randomize_address = true;
    /* The button says the acknowledgement in words, and the backend refuses without it.
     * Same posture as the Wi-Fi dialog: the UI is not what enforces the gate. */
    p.acknowledged = true;

    const esp_err_t err = ble_toolbox_spam_arm(&p);
    if (err != ESP_OK) {
        lv_label_set_text(app->spam_state_label, esp_err_to_name(err));
        return;
    }

    ESP_LOGW(TAG, "spam armed by explicit confirmation");
    app->refreshSpam();
}

#endif  /* CONFIG_BLE_TOOLBOX_ALLOW_SPAM */

void BleToolboxApp::buildAirTag(void){
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = toolbox_make_screen(screens[SCREEN_AIRTAG], area, "AirTag Monitor",
                                          LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)ACT_BACK);

    tag_count_label = lv_label_create(panel);
    lv_obj_set_style_text_font(tag_count_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(tag_count_label, "0 tracker advert(s)");

    tag_glasses_label = lv_label_create(panel);
    lv_obj_set_style_text_font(tag_glasses_label, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(tag_glasses_label, "possible smart glasses: 0");

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    tag_start_btn = toolbox_make_button(buttons, "Start", onEvent,
                                        (void *)(intptr_t)ACT_TAG_START, 44);
    lv_obj_set_flex_grow(tag_start_btn, 1);
    tag_stop_btn = toolbox_make_button(buttons, "Stop", onEvent,
                                       (void *)(intptr_t)ACT_TAG_STOP, 44);
    lv_obj_set_flex_grow(tag_stop_btn, 1);

    tag_list = lv_obj_create(panel);
    lv_obj_set_width(tag_list, LV_PCT(100));
    lv_obj_set_flex_grow(tag_list, 1);
    lv_obj_set_flex_flow(tag_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tag_list, 0, 0);
    lv_obj_set_style_pad_row(tag_list, 4, 0);
    lv_obj_set_scroll_dir(tag_list, LV_DIR_VER);
}

/* ---- Self-test ----------------------------------------------------------- */

void BleToolboxApp::selfTest(void)
{
    static const char *names[SCREEN_COUNT] = {
        "menu", "scan", "observer", "airtag",
#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
        "spam",
#endif
    };

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    const int menu_entries = 4;
#else
    const int menu_entries = 3;
#endif

    ESP_LOGW(TAG, "=== BLE TOOLBOX APP SELF-TEST ===");

    if (!run()) {
        ESP_LOGE(TAG, "  run() failed");
        return;
    }
    ESP_LOGW(TAG, "  run() ok, on screen \"%s\"", names[active_screen]);

    /* Walk the menu the way a tap would, checking that each switch lands. The count is
     * taken from the build rather than hard-coded, so adding a screen without extending
     * this test is caught rather than silently skipped. */
    for (int i = 0; i < menu_entries; i++) {
        lv_obj_t *btn = nullptr;

        /* The menu draws straight into its container, so the entries are its clickable
         * button children. Sending the event exercises onEvent, not showScreen(). */
        const uint32_t n = lv_obj_get_child_cnt(screens[SCREEN_MENU]);
        int seen = 0;
        for (uint32_t c = 0; c < n; c++) {
            lv_obj_t *child = lv_obj_get_child(screens[SCREEN_MENU], c);
            if (child != nullptr && lv_obj_check_type(child, &lv_button_class)) {
                if (seen++ == i) {
                    btn = child;
                    break;
                }
            }
        }

        if (btn == nullptr) {
            ESP_LOGE(TAG, "  menu entry %d not found", i);
            continue;
        }

        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        ESP_LOGW(TAG, "  menu entry %d -> \"%s\"", i, names[active_screen]);
    }

    /* Back to the menu, then start a scan through the same handler the Start button
     * uses so the service and the UI state are both exercised. */
    showScreen(SCREEN_MENU);
    ESP_LOGW(TAG, "  back to \"%s\"", names[active_screen]);

    /* Every screen's content height against the space it has. Enlarging the text is the
     * kind of change that pushes content past the bottom of a panel, and the panels scroll
     * rather than clip, so it would go unnoticed without asking. */
    ESP_LOGW(TAG, "  --- screen fit at this font size ---");
    /* The base font the shell and every unstyled widget inherit. Reported because the
     * default is set in sdkconfig, where a change takes effect silently and the only way
     * to know it applied is to ask the compiled-in font. */
    ESP_LOGW(TAG, "  default font line height: %d px (montserrat_14 = 16, montserrat_18 = 21)",
             (int)lv_font_get_line_height(LV_FONT_DEFAULT));
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (screens[i] == nullptr) {
            continue;
        }
        showScreen((Screen)i);
        lv_obj_update_layout(screens[i]);

        /* The screens hold one panel each; the menu holds its content directly. */
        lv_obj_t *inner = (screens[i] == screens[SCREEN_MENU])
                          ? screens[i] : lv_obj_get_child(screens[i], 0);
        if (inner == nullptr) {
            continue;
        }

        /* Content extent, NOT lv_obj_get_height(). A scrollable container reports its
         * visible height there, so every screen comes back as exactly the panel size and
         * the check says nothing - which is what a first version of this printed, as five
         * identical "670 px in 670 px" lines. The bottom of the lowest child is what
         * actually moves when the text grows. */
        lv_obj_update_layout(inner);
        int deepest = 0;
        const uint32_t kids = lv_obj_get_child_cnt(inner);
        for (uint32_t k = 0; k < kids; k++) {
            lv_obj_t *child = lv_obj_get_child(inner, k);
            if (child == nullptr) {
                continue;
            }
            const int bottom = lv_obj_get_y(child) + lv_obj_get_height(child);
            if (bottom > deepest) {
                deepest = bottom;
            }
        }
        deepest += lv_obj_get_style_pad_bottom(inner, LV_PART_MAIN);

        const int room = lv_obj_get_height(inner);
        ESP_LOGW(TAG, "  %-9s content %4d px in %4d px  %s", names[i], deepest, room,
                 deepest > room ? "SCROLLS" : "fits");
    }

    /* The Observer's list specifically: a container holding more than it can show must
     * report a scroll range. That is the property which was missing when it was a label,
     * and it is not visible from the content-height check above.
     *
     * The list has to be FILLED first. Measuring it while it holds only the "nothing heard
     * yet" placeholder reports a negative range and "NOT scrollable", which is correct for
     * an empty list and says nothing about a full one - a first version of this check did
     * exactly that and proved nothing. */
    showScreen(SCREEN_OBSERVER);
    if (obs_log != nullptr) {
        lv_obj_clean(obs_log);
        for (int i = 0; i < kObsCap; i++) {
            toolbox_make_log_line(obs_log, "Apple FindMy  fc:1b:5d:dd:54:80  -64 dBm");
        }
        lv_obj_update_layout(screens[SCREEN_OBSERVER]);

        const int range = lv_obj_get_scroll_bottom(obs_log) + lv_obj_get_scroll_top(obs_log);
        ESP_LOGW(TAG, "  observer list: %d lines filled, scroll range %d px  %s",
                 (int)lv_obj_get_child_cnt(obs_log), range,
                 range > 0 ? "SCROLLABLE" : "NOT scrollable");

        /* Leave it as the app would: the next tick redraws from the latch. */
        obs_latch_len = 0;
        list_dirty = true;
        refreshObserver();
    }
    showScreen(SCREEN_MENU);

    showScreen(SCREEN_SCAN);
    if (!ensureService()) {
        ESP_LOGE(TAG, "  service unavailable");
        return;
    }

    ble_toolbox_scan_params_t p = {};
    p.interval_ms = 100;
    p.window_ms = 100;
    p.passive = true;
    p.filter_duplicates = true;
    scanning = (ble_toolbox_host_scan_start(&p) == ESP_OK);
    ESP_LOGW(TAG, "  scan started: %d", (int)scanning);

    for (int i = 0; i < 60; i++) {          /* up to 6 s, or until devices appear */
        vTaskDelay(pdMS_TO_TICKS(100));
        updateStatus();
        refreshScan();
        if (scan_latch_len > 0) {
            break;
        }
    }

    /* Let the observer fill too, so its cap is exercised rather than assumed. It holds
     * one entry per advertisement, so this is where a busy room actually shows. */
    for (int i = 0; i < 80 && obs_latch_len < (int)kObsCap; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "  heard %lu advertisement(s), %d device(s) listed",
             (unsigned long)scan_total, scan_latch_len);
    /* Reported because the observer's list is capped and the cap is worth seeing: this is
     * the count the Observer screen actually renders. */
    ESP_LOGW(TAG, "  observer: %lu frames heard, %d held (cap %d)",
             (unsigned long)obs_total, obs_latch_len, (int)kObsCap);
    ESP_LOGW(TAG, "  trackers: %lu   smart-glasses hints: %lu",
             (unsigned long)tag_total, (unsigned long)glasses_total);

    /* A second Start must begin from zero rather than adding to the first run. Worth
     * testing because it is the kind of thing that looks right in a screenshot of a fresh
     * boot and is wrong the second time anyone uses it. */
    ESP_LOGW(TAG, "  --- restart check ---");
    const uint32_t before = scan_total;
    resetCounters();

    /* Every field the three screens render is reported, not just the totals. The totals
     * alone were not enough: tag_latch_len and glasses_total were not printed, so the
     * AirTag screen's reset went unverified even though resetCounters() covered it. */
    ESP_LOGW(TAG, "  after reset: scan=%lu/%d obs=%lu/%d tag=%lu/%d glasses=%lu (scan was %lu)",
             (unsigned long)scan_total, scan_latch_len,
             (unsigned long)obs_total, obs_latch_len,
             (unsigned long)tag_total, tag_latch_len,
             (unsigned long)glasses_total, (unsigned long)before);

    const bool cleared = (scan_total == 0 && obs_total == 0 && tag_total == 0 &&
                          glasses_total == 0 && scan_latch_len == 0 &&
                          obs_latch_len == 0 && tag_latch_len == 0);
    ESP_LOGW(TAG, "  all counters and lists clear: %d (want 1)", (int)cleared);

    for (int i = 0; i < 30 && scan_total == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "  counting again after reset: scan=%lu tag=%lu glasses=%lu",
             (unsigned long)scan_total, (unsigned long)tag_total,
             (unsigned long)glasses_total);

    (void)ble_toolbox_host_scan_stop();
    showScreen(SCREEN_MENU);
    ESP_LOGW(TAG, "=== BLE TOOLBOX APP SELF-TEST done ===");
}

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
void BleToolboxApp::spamSelfTest(void)
{
    ESP_LOGW(TAG, "=== BLE SPAM GATE SELF-TEST ===");

    /* 1. The gate must refuse an arm that was not acknowledged. This is the property
     * the feature's safety rests on, and it is the one a UI test cannot show: a button
     * being present says nothing about whether the backend would have accepted a call
     * that skipped it. */
    ble_toolbox_spam_params_t no_ack = {};
    no_ack.acknowledged = false;
    no_ack.max_hold_us = BLE_TOOLBOX_SPAM_MAX_HOLD_US;
    const esp_err_t refused = ble_toolbox_spam_arm(&no_ack);
    ESP_LOGW(TAG, "  arm without acknowledgement: %s (want ESP_ERR_INVALID_ARG)",
             esp_err_to_name(refused));
    if (refused != ESP_ERR_INVALID_ARG) {
        ESP_LOGE(TAG, "  GATE FAILED: an unacknowledged arm was accepted");
        return;
    }
    ESP_LOGW(TAG, "  armed after refusal: %d (want 0)", (int)ble_toolbox_spam_is_armed());

    /* 2. A null params is refused too, so there is no default that arms. */
    const esp_err_t null_refused = ble_toolbox_spam_arm(NULL);
    ESP_LOGW(TAG, "  arm with NULL params: %s (want ESP_ERR_INVALID_ARG)",
             esp_err_to_name(null_refused));

    /* 3. An acknowledged arm with a short hold: it must transmit, then stop itself. A
     * two-second cap is used rather than the ten-minute default because the expiry is
     * the thing under test and waiting ten minutes for it is not a test. */
    if (!ensureService()) {
        ESP_LOGE(TAG, "  service unavailable");
        return;
    }

    ble_toolbox_spam_params_t armed = {};
    armed.acknowledged = true;
    armed.randomize_address = true;
    armed.max_hold_us = 2u * 1000000u;

    const esp_err_t err = ble_toolbox_spam_arm(&armed);
    ESP_LOGW(TAG, "  acknowledged arm: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  could not arm");
        return;
    }

    ESP_LOGW(TAG, "  armed: %d, seconds left: %lu", (int)ble_toolbox_spam_is_armed(),
             (unsigned long)ble_toolbox_spam_seconds_left());

    /* Drive the tick the way the UI timer does, and watch it stop on its own. */
    for (int i = 0; i < 12 && ble_toolbox_spam_is_armed(); i++) {
        (void)ble_toolbox_spam_tick();
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    ESP_LOGW(TAG, "  after the hold: armed=%d, sent=%lu",
             (int)ble_toolbox_spam_is_armed(), (unsigned long)ble_toolbox_spam_sent());

    if (ble_toolbox_spam_is_armed()) {
        ESP_LOGE(TAG, "  GATE FAILED: still transmitting past the hold time");
        (void)ble_toolbox_spam_disarm();
    }

    /* 4. Disarm is idempotent, because close() calls it without knowing the state. */
    ESP_LOGW(TAG, "  disarm when already stopped: %s",
             esp_err_to_name(ble_toolbox_spam_disarm()));

    ESP_LOGW(TAG, "  payloads available: %u", (unsigned)ble_toolbox_spam_payload_count());
    ESP_LOGW(TAG, "=== BLE SPAM GATE SELF-TEST done ===");
}
#endif

/* ---- Events -------------------------------------------------------------- */

void BleToolboxApp::onEvent(lv_event_t *e)
{
    /* The action code travels in the event's user_data. Every registration of this
     * handler passes one, and nothing else does. */
    const int action = (int)(intptr_t)lv_event_get_user_data(e);

    /* The app instance comes from the static, NOT from user_data: that field holds the
     * action code, so casting it yielded a small integer used as a pointer. The Wi-Fi
     * toolbox made exactly that mistake and it cost a long debugging round; the
     * comment is here so the next reader does not repeat it. */
    BleToolboxApp *app = g_app;
    if (app == nullptr) {
        return;
    }

    switch (action) {
    case ACT_MENU_SCAN:
        app->showScreen(SCREEN_SCAN);
        break;
    case ACT_MENU_OBSERVER:
        app->showScreen(SCREEN_OBSERVER);
        break;
    case ACT_MENU_AIRTAG:
        app->showScreen(SCREEN_AIRTAG);
        break;

    case ACT_BACK:
        app->showScreen(SCREEN_MENU);
        break;

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM
    case ACT_MENU_SPAM:
        app->showScreen(SCREEN_SPAM);
        break;

    case ACT_SPAM_START:
        /* Nothing transmits from this button. It opens the dialog, and the dialog's
         * confirm is what arms — an arm reachable in one tap is not a gate. */
        if (!app->ensureService()) {
            lv_label_set_text(app->spam_state_label, "BLE service unavailable");
            break;
        }
        app->askSpamArm();
        break;

    case ACT_SPAM_STOP:
        (void)ble_toolbox_spam_disarm();
        app->refreshSpam();
        break;
#endif

    case ACT_SCAN_START:
    case ACT_OBS_START:
    case ACT_TAG_START: {
        /* One radio, one scan: the three screens are views of the same advertisements,
         * so a Start on any of them starts the same scan rather than competing for it. */
        if (!app->ensureService()) {
            lv_label_set_text(app->scan_state_label, "BLE service unavailable");
            return;
        }

        ble_toolbox_scan_params_t p = {};
        p.interval_ms = 100;
        p.window_ms = 100;
        p.passive = true;
        p.filter_duplicates = true;

        /* Cleared BEFORE the start, so the first frame of the new run is counted. Clearing
         * after would drop however many arrived in between.
         *
         * Done even if the start then fails: a scan is already running in that case, and
         * its counts are still the previous run's, so leaving them would be the worse of
         * the two outcomes either way. */
        app->resetCounters();

        const esp_err_t err = ble_toolbox_host_scan_start(&p);
        if (err == ESP_OK) {
            app->scanning = true;
            lv_label_set_text(app->scan_state_label, "scanning");
        } else if (err == ESP_ERR_INVALID_STATE) {
            lv_label_set_text(app->scan_state_label, "already scanning");
        } else {
            lv_label_set_text(app->scan_state_label, esp_err_to_name(err));
        }
        break;
    }

    case ACT_SCAN_STOP:
    case ACT_OBS_STOP:
    case ACT_TAG_STOP:
        (void)ble_toolbox_host_scan_stop();
        app->scanning = false;
        if (app->scan_state_label != nullptr) {
            lv_label_set_text(app->scan_state_label, "stopped");
        }
        break;

    case ACT_CONN_DISCONNECT:
        (void)ble_toolbox_host_disconnect();
        app->conn_nav = 0;
        app->svc_latch_len = 0;
        app->chr_latch_len = 0;
        app->refreshConnect();
        break;

    case ACT_CONN_SERVICES:
        /* Back to the service list from the characteristics. */
        app->conn_nav = 0;
        app->refreshConnect();
        break;

    case ACT_CONN_WRITE:
        /* Write a small fixed value to the last-tapped characteristic, to exercise the
         * write path. Read it again afterwards to see the change. */
        if (app->conn_state == BLE_TOOLBOX_CONN_CONNECTED && app->conn_read_handle != 0) {
            const uint8_t v = 0x01;
            (void)ble_toolbox_host_write(app->conn_read_handle, &v, 1);
        }
        break;

    case ACT_CONN_SUBSCRIBE:
        /* Subscribe to the last-tapped characteristic so its notifications/indications
         * stream in — this is what a BLE HID keyboard needs before it sends key reports. */
        if (app->conn_state == BLE_TOOLBOX_CONN_CONNECTED && app->conn_read_handle != 0) {
            (void)ble_toolbox_host_subscribe(app->conn_read_handle, false);
        }
        break;

    default:
        break;
    }
}


