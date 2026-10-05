/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — phone app. See ToolboxApp.hpp for the scope note.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_heap_caps.h"
#include "lwip/inet.h"
#include "esp_log.h"

#include "wifi_toolbox_host.h"
#include "wifi_toolbox_net.h"
#include "wifi_toolbox_pcap.h"
#include "wifi_toolbox_rpc.h"
#include "vendor_lookup.h"

#include "ToolboxApp.hpp"
#include "toolbox_ui.hpp"
#include "toolbox_icons.hpp"

static const char *TAG = "ToolboxApp";

/* Launcher icon: Wi-Fi arcs on the toolbox blue. The painter is shared with the BLE
 * Toolbox — see toolbox_icons.hpp for why it is painted rather than a converted PNG,
 * and for the pixel-layout trap that cost this icon a round. The geometry is the one
 * that was prototyped and looked at before becoming C. */
static toolbox_icon_state_t s_icon_state;
static const float k_icon_radii[3] = { 19.0f, 39.0f, 59.0f };
static const toolbox_wifi_glyph_t k_wifi_glyph = {
    .cx = 56.0f, .apex_y = 68.0f, .thick = 7.0f, .span = 48.0f,
    .dot_x = 56.0f, .dot_y = 88.0f, .dot_r = 6.5f,
    .radii = k_icon_radii,
};

static const lv_image_dsc_t *toolbox_launcher_icon(void)
{
    return toolbox_icon_paint(&s_icon_state, 0x49, 0x8B, 0xE8,
                              toolbox_icon_is_wifi, (void *)&k_wifi_glyph, TAG);
}

/* The arming notice (spec section 10.1). Shown every time the gate is opened,
 * because the point of a notice is that the person about to transmit reads it. */
static const char *k_auth_notice =
    "Capture and injection are for networks you own or are\n"
    "explicitly authorised to test. Deauth and disassoc frames\n"
    "disrupt other people's connections.";

/* ---- Small helpers ------------------------------------------------------- */

/* Dropdown helper: index in, option text out. */
static const char *dd_text(lv_obj_t *dd)
{
    static char buf[64];
    lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
    return buf;
}

static uint16_t dd_selected(lv_obj_t *dd)
{
    return lv_dropdown_get_selected(dd);
}

/* ---- Toolbox callbacks (these run on esp-hosted's RPC task) -------------
 *
 * They must not touch LVGL: the RPC task is not the display task, and LVGL is not
 * thread-safe here. Each one latches into the app's state and the app's LVGL timer
 * renders it, which is also what makes the ~1 Hz status push cheap to consume. */

namespace {

struct Latch {
    volatile bool     status_dirty;
    volatile bool     link_up;
    volatile bool     have_status;
    volatile bool     link_changed;
    wifi_toolbox_status_t status;

    /* Net-scan results, appended by the scan task and drained by the UI timer.
     * A scan line is short and the count is small, so this is a fixed ring rather
     * than an allocation on a task that must not block. */
    volatile uint32_t scan_pending;
    volatile bool     scan_done;
    volatile esp_err_t scan_err;
    char              scan_lines[16][64];
    char              scan_stats[48];
    uint32_t          scan_dropped;
};

Latch g_latch;

/* The app instance the UI timer drives. Set in init(): there is exactly one. */
ToolboxApp *g_app = nullptr;

}  /* namespace */

/* The app needs to read the latch from its timer, and the callbacks need to write
 * it from the RPC task. A spinlock would be overkill: both sides touch single
 * words except for `status`, which is only ever *replaced* under this critical
 * section, so a torn read cannot happen. */
static portMUX_TYPE s_latch_mux = portMUX_INITIALIZER_UNLOCKED;

extern "C" void toolbox_ui_on_status(const wifi_toolbox_status_t *st, void *ctx);
extern "C" void toolbox_ui_on_link(bool up, void *ctx);
extern "C" void toolbox_ui_on_frame(const wifi_toolbox_frame_evt_t *hdr, const uint8_t *payload, void *ctx);
extern "C" void toolbox_ui_on_error(wifi_toolbox_result_t code, const char *message, void *ctx);

namespace {

volatile uint32_t g_frames_seen;
char              g_last_error[96];

}  /* namespace */

extern "C" void toolbox_ui_on_status(const wifi_toolbox_status_t *st, void *ctx)
{
    (void)ctx;

    portENTER_CRITICAL(&s_latch_mux);
    g_latch.status = *st;
    g_latch.have_status = true;
    g_latch.status_dirty = true;
    portEXIT_CRITICAL(&s_latch_mux);
}

extern "C" void toolbox_ui_on_link(bool up, void *ctx)
{
    (void)ctx;

    g_latch.link_up = up;
    g_latch.link_changed = true;
}

extern "C" void toolbox_ui_on_frame(const wifi_toolbox_frame_evt_t *hdr, const uint8_t *payload, void *ctx)
{
    (void)hdr;
    (void)payload;
    (void)ctx;

    /* Writing the capture file happens here, on the RPC task, so the frames are
     * written before the next one arrives. Only when a capture screen asked for a
     * file: a capture with no file is still a valid session (the counters are the
     * point on a screen), and an implicit file would fill the card. */
    if (wifi_toolbox_pcap_is_open()) {
        (void)wifi_toolbox_pcap_write(hdr, payload);
    }

    /* Counted under the same lock the status latch uses, so the read-modify-write
     * is one operation rather than a `volatile ++` that only looks atomic. */
    portENTER_CRITICAL(&s_latch_mux);
    g_frames_seen++;
    portEXIT_CRITICAL(&s_latch_mux);
}

extern "C" void toolbox_ui_on_error(wifi_toolbox_result_t code, const char *message, void *ctx)
{
    (void)ctx;

    snprintf(g_last_error, sizeof(g_last_error), "%s", (message != nullptr) ? message : "error");
    ESP_LOGW(TAG, "toolbox error %u: %s", (unsigned)code, g_last_error);
}

/* ---- Net-scan callbacks (also on the scan task) -------------------------- */

static void push_scan_line(const char *text)
{
    const uint32_t slot = g_latch.scan_pending;

    if (slot >= 16) {
        g_latch.scan_dropped++;
        return;
    }

    snprintf(g_latch.scan_lines[slot], sizeof(g_latch.scan_lines[0]), "%s", text);
    g_latch.scan_pending = slot + 1;
}

extern "C" void toolbox_ui_on_scan_host(uint32_t ip, const char *ip_text, void *ctx)
{
    (void)ip;
    (void)ctx;

    char line[64];
    snprintf(line, sizeof(line), "host up: %s", (ip_text != nullptr) ? ip_text : "?");
    push_scan_line(line);
}

extern "C" void toolbox_ui_on_scan_port(uint32_t ip, uint16_t port, const char *service, void *ctx)
{
    (void)ctx;

    struct in_addr a;
    a.s_addr = ip;

    char line[64];
    snprintf(line, sizeof(line), "%s:%u open (%s)", inet_ntoa(a), (unsigned)port,
             (service != nullptr) ? service : "-");
    push_scan_line(line);
}

extern "C" void toolbox_ui_on_scan_progress(const wifi_toolbox_scan_stats_t *stats, void *ctx)
{
    (void)ctx;

    if (stats == nullptr) {
        return;
    }

    snprintf(g_latch.scan_stats, sizeof(g_latch.scan_stats), "scanned %lu  found %lu",
             (unsigned long)stats->scanned, (unsigned long)stats->found);
}

extern "C" void toolbox_ui_on_scan_done(esp_err_t err, const wifi_toolbox_scan_stats_t *stats, void *ctx)
{
    (void)ctx;

    if (stats != nullptr) {
        snprintf(g_latch.scan_stats, sizeof(g_latch.scan_stats), "done: scanned %lu  found %lu",
                 (unsigned long)stats->scanned, (unsigned long)stats->found);
    }

    g_latch.scan_err = err;
    g_latch.scan_done = true;
}

/* Start a scan with the UI's callbacks wired in. A thin wrapper so the event
 * handler does not have to build the callback table every time. */
static esp_err_t scan_start_ui(const wifi_toolbox_scan_request_t *req)
{
    wifi_toolbox_scan_callbacks_t cb = {};
    cb.on_host = toolbox_ui_on_scan_host;
    cb.on_port = toolbox_ui_on_scan_port;
    cb.on_progress = toolbox_ui_on_scan_progress;
    cb.on_done = toolbox_ui_on_scan_done;
    cb.ctx = nullptr;

    g_latch.scan_pending = 0;
    g_latch.scan_done = false;
    g_latch.scan_err = ESP_OK;
    g_latch.scan_dropped = 0;
    g_latch.scan_stats[0] = '\0';

    return wifi_toolbox_scan_start(req, &cb);
}

/* ---- Construction -------------------------------------------------------- */

ToolboxApp::ToolboxApp():
    ESP_Brookesia_PhoneApp("Wi-Fi Toolbox", nullptr, true),
    arm_modal(nullptr),
    active_screen(SCREEN_MENU),
    banner(nullptr),
    banner_label(nullptr),
    cap_state_label(nullptr),
    cap_counters_label(nullptr),
    cap_path_label(nullptr),
    cap_channel_dd(nullptr),
    cap_hop_switch(nullptr),
    cap_elicit_switch(nullptr),
    cap_log(nullptr),
    inj_mode_dd(nullptr),
    inj_ssid_ta(nullptr),
    inj_bssid_ta(nullptr),
    inj_bssid_vendor(nullptr),
    inj_client_ta(nullptr),
    inj_channel_dd(nullptr),
    inj_duration_slider(nullptr),
    inj_duration_label(nullptr),
    inj_counters_label(nullptr),
    inj_log(nullptr),
    net_mode_dd(nullptr),
    net_preset_dd(nullptr),
    net_target_ta(nullptr),
    net_results(nullptr),
    net_stats_label(nullptr),
    ui_timer(nullptr),
    arm_pending_inject(false),
    arm_pending_channel(6),
    arm_pending_duration_s(30),
    arm_pending_mode{0}
{
    memset(screens, 0, sizeof(screens));
}

ToolboxApp::~ToolboxApp()
{
}

bool ToolboxApp::init(void)
{
    ESP_LOGI(TAG, "init");

    /* Nothing is registered with esp-hosted here, and that is deliberate.
     *
     * Observed on this board: subscribing to the toolbox's custom-RPC messages
     * from app-install time — which is ~3 s in, *before* esp-hosted has reset and
     * enumerated the co-processor — leaves the Wi-Fi service's own
     * esp_wifi_init() returning ESP_FAIL, and this app aborts on it. With those
     * same subscriptions made later, Wi-Fi initialises normally. The heap was not
     * the cause (137 KB free at the time), so it is the ordering.
     *
     * Deferring to the first run() costs nothing that the spec asks for: a session
     * can only be started from these screens in the first place, and once they are
     * open the module keeps reporting whether or not the user navigates away. */

    /* The launcher icon, on the other hand, has to be set here and not in run().
     * The core builds the home screen during start(), which happens after every app's
     * init() but before any app's run(), and the launcher's icon widget takes the
     * image resource when it is created — so an icon assigned in run() would arrive
     * after the widget that shows it already exists.
     *
     * Painting it here is safe: it touches no radio and registers nothing, so it is
     * not the app-install ordering problem described above. */
    const lv_image_dsc_t *icon = toolbox_launcher_icon();
    if (icon != nullptr) {
        const ESP_Brookesia_StyleImage_t image = ESP_BROOKESIA_STYLE_IMAGE(icon);
        setLauncherIconImage(image);
    }

    return true;
}

/* Called from run(): by then esp-hosted is up and the Wi-Fi service has had its
 * turn at the co-processor. Idempotent, so reopening the app is cheap. */
bool ToolboxApp::ensureHost(void)
{
    if (g_app == nullptr) {
        wifi_toolbox_host_callbacks_t cb = {};
        cb.on_frame = toolbox_ui_on_frame;
        cb.on_status = toolbox_ui_on_status;
        cb.on_link = toolbox_ui_on_link;
        cb.on_error = toolbox_ui_on_error;

        const esp_err_t err = wifi_toolbox_host_init(&cb, this);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "toolbox host init failed: %s", esp_err_to_name(err));
            return false;
        }

        g_app = this;
        g_latch.link_up = true;

        ESP_LOGI(TAG, "toolbox host brought up on first open");
    }

    (void)wifi_toolbox_host_query_caps(nullptr);

    return true;
}

/* ---- Screen scaffolding -------------------------------------------------- */

void ToolboxApp::logScreenStructure(void)
{
    static const char *names[SCREEN_COUNT] = { "menu", "capture", "inject", "network" };

    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (screens[i] == nullptr) {
            ESP_LOGW(TAG, "  screen %-8s: container MISSING", names[i]);
            continue;
        }

        const uint32_t children = lv_obj_get_child_cnt(screens[i]);
        const bool hidden = lv_obj_has_flag(screens[i], LV_OBJ_FLAG_HIDDEN);
        const bool active = (i == (int)active_screen);

        ESP_LOGW(TAG, "  screen %-8s: children=%u hidden=%d active=%d %s",
                 names[i], (unsigned)children, (int)hidden, (int)active,
                 (children == 0) ? "<-- EMPTY, its panel is parented elsewhere" : "");
    }
}

void ToolboxApp::logScrollState(void)
{
    static const char *names[SCREEN_COUNT] = { "menu", "capture", "inject", "network" };

    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (screens[i] == nullptr) {
            continue;
        }

        /* Each screen container holds exactly one panel, which in turn holds the
         * content. Measure the panel: that is what would scroll. */
        lv_obj_t *panel = lv_obj_get_child(screens[i], 0);
        if (panel == nullptr) {
            ESP_LOGW(TAG, "  %-8s: no panel", names[i]);
            continue;
        }

        const lv_coord_t panel_h = lv_obj_get_height(panel);
        const lv_coord_t overflow = lv_obj_get_scroll_bottom(panel) + lv_obj_get_scroll_top(panel);
        const lv_coord_t content_h = panel_h + overflow;
        const uint32_t kids = lv_obj_get_child_cnt(panel);

        ESP_LOGW(TAG, "  %-8s: panel_h=%d children=%u content_h=%d overflow=%d scrollable=%d %s",
                 names[i], (int)panel_h, (unsigned)kids, (int)content_h, (int)overflow,
                 (int)lv_obj_has_flag(panel, LV_OBJ_FLAG_SCROLLABLE),
                 (kids < 2) ? "<-- NOT A PANEL, measurement is meaningless" : "");
    }
}



void ToolboxApp::showScreenForTest(int index)
{
    if (index < 0 || index >= SCREEN_COUNT) {
        return;
    }

    showScreen((Screen)index);
}

/* Send a real click to the menu's buttons, one at a time, and report which screen
 * each one lands on. This is the path a finger takes: LVGL dispatches the event to
 * the registered callback, so it exercises onEvent rather than calling showScreen()
 * directly and hoping. */
void ToolboxApp::runNavigationSelfTest(void)
{
    static const char *names[SCREEN_COUNT] = { "menu", "capture", "inject", "network" };

    if (screens[SCREEN_MENU] == nullptr) {
        ESP_LOGW(TAG, "self-test: menu container missing");
        return;
    }

    /* The entries live inside the menu's panel, which is the container's only child.
     * Clicking the container's children would click the panel itself. */
    lv_obj_t *panel = lv_obj_get_child(screens[SCREEN_MENU], 0);
    if (panel == nullptr) {
        ESP_LOGW(TAG, "self-test: menu panel missing");
        return;
    }

    const uint32_t total = lv_obj_get_child_cnt(panel);
    int clicked = 0;
    int skipped = 0;

    for (uint32_t i = 0; i < total; i++) {
        lv_obj_t *child = lv_obj_get_child(panel, i);

        /* Select by class, not by the clickable flag. The header is a generic
         * container and generic containers are clickable in LVGL, so a flag test
         * counts the header as a menu entry and clicks it — which is what the first
         * attempt did. The entries are buttons. */
        if (child == nullptr || lv_obj_get_class(child) != &lv_button_class) {
            continue;
        }

        /* The header's back button is a button too, and clicking it closes the app,
         * not something to do mid-check. It is told apart by its parent: the entries
         * are direct children of the panel, the back button sits in the header. */
        if (lv_obj_get_parent(child) != panel) {
            skipped++;
            continue;
        }

        const Screen before = active_screen;
        lv_obj_send_event(child, LV_EVENT_CLICKED, nullptr);
        clicked++;

        ESP_LOGW(TAG, "  clicked menu entry %d: %s -> %s", clicked,
                 names[before], names[active_screen]);

        /* Back to the menu for the next one, the way a person would. */
        showScreen(SCREEN_MENU);
    }

    ESP_LOGW(TAG, "  %d menu entries clicked (%d header button(s) skipped)", clicked, skipped);
}

void ToolboxApp::showScreen(Screen screen)
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

    if (screen != SCREEN_MENU) {
        (void)wifi_toolbox_host_refresh_status();
    }
}

/* A titled panel with a back button, built INSIDE the container it belongs to.
 *
 * The parent matters and is not cosmetic. showScreen() switches screens by
 * hiding and showing screens[i], so a panel created anywhere else is not switched
 * at all — it just stays on top of everything. The first version of this built the
 * panel on lv_screen_active(), which made it a sibling of the containers rather than a
 * child: opening a sub-screen showed a second panel over the menu, and tapping its
 * back button hid a container that was never visible, leaving the menu's panel
 * underneath. That is the blank screen. Nesting the panel fixes the switch, and
 * also makes the panel inherit the cleanup when the core deletes the screen. */
static lv_obj_t *make_screen(lv_obj_t *parent, lv_area_t area, const char *title, const char *back_label,
                             lv_event_cb_t back_cb, void *user)
{
    const int w = area.x2 - area.x1 + 1;
    const int h = area.y2 - area.y1 + 1;

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);

    /* Scroll when the content does not fit, and keep the bar on screen rather than
     * showing it only while scrolling. Measured on this board, one screen does not
     * fit: the injection controls need 788 px in a 670 px panel, so 118 px of them
     * were simply inaccessible with scrolling off — including the Start button at the
     * bottom. The capture and network screens fit today, but their content is also
     * variable (the log grows), so they get the same treatment rather than being
     * special-cased into a latent clip.
     *
     * LV_SCROLLBAR_MODE_ON rather than AUTO because a bar that appears only mid-drag
     * is easy to miss; the point is to show at a glance that there is more below. The
     * bar overlays the panel's right edge without a reserved strip (the default for
     * this mode), so nothing shifts sideways when it appears. */
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_width(panel, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(panel, lv_color_make(190, 60, 60), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(panel, LV_OPA_70, LV_PART_SCROLLBAR);

    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* No callback means no back button: the menu is the top of this app and has
     * nowhere to go back to, so the framework's own bar handles closing it. */
    if (back_cb != nullptr) {
        lv_obj_t *back = lv_btn_create(header);
        lv_obj_t *back_txt = lv_label_create(back);
        lv_label_set_text(back_txt, back_label);
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, user);
    }

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, TOOLBOX_FONT_TITLE, 0);

    return panel;
}

/* A labelled text area for a MAC or address field. */
static lv_obj_t *make_field(lv_obj_t *parent, const char *label_text, const char *placeholder, lv_obj_t **out_ta)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_width(label, 118);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_flex_grow(ta, 1);

    if (out_ta != nullptr) {
        *out_ta = ta;
    }

    return row;
}

static lv_obj_t *make_label_row(lv_obj_t *parent, const char *label_text, lv_obj_t **out_value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, "-");

    if (out_value != nullptr) {
        *out_value = value;
    }

    return row;
}

/* A single-line status field, NOT a scrolling list — deliberately still a label.
 *
 * The BLE Toolbox's equivalent was a label too and was wrong, because it accumulated one
 * line per captured frame and a label clips what it cannot show. This one is assigned
 * rather than appended ("capture started", "stopped", "PCAP opened"), so it only ever holds
 * one line and there is nothing to scroll to. Use the shared toolbox_make_log() where lines
 * accumulate. */
static lv_obj_t *make_log(lv_obj_t *parent)
{
    lv_obj_t *log = lv_label_create(parent);
    lv_obj_set_width(log, LV_PCT(100));
    lv_obj_set_flex_grow(log, 1);
    lv_label_set_long_mode(log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(log, TOOLBOX_FONT_LOG, 0);
    lv_label_set_text(log, "");

    return log;
}

/* ---- Menu ---------------------------------------------------------------- */

void ToolboxApp::buildMenu(void)
{
    lv_area_t area = getVisualArea();

    /* The menu now nests a panel like every other screen, instead of drawing into
     * its container directly. Two reasons, and the first is not cosmetic: measuring
     * overflow needs one place to look, and when the menu drew straight into the
     * container there was none — lv_obj_get_child() returned the title label, so the
     * scroll measurement read a label's height and would have reported a clipped menu
     * as fine. The second is that the menu is then able to scroll like the rest, and
     * can no longer be the one screen that silently clips.
     *
     * No back button: the menu is the top of this app, and the framework's own bar
     * closes the app from here. make_screen leaves the button out when the callback
     * is null, which also keeps the self-test from closing the app mid-check. */
    lv_obj_t *panel = make_screen(screens[SCREEN_MENU], area, "Wi-Fi Toolbox", LV_SYMBOL_LEFT,
                                  nullptr, nullptr);
    lv_obj_set_style_pad_row(panel, 8, 0);

    /* Menu construction is where the injection entry is withheld when the build
     * does not offer it: spec section 10.5 wants the shipped product to be unable
     * to ask for injection at all, not to offer it and then refuse. */
    static const char *entries[3] = {
        "PMKID / EAPOL Capture",
#if CONFIG_TOOLBOX_ALLOW_INJECTION
        "Frame Injection",
#else
        "Frame Injection (not in this build)",
#endif
        "WiFi + Net Utilities",
    };

    /* Menu entries are the app's navigation and carry a font of their own rather than the
     * theme's. Chosen by measurement, not by eye: this menu's longest entry is 35
     * characters, so a size that suits the BLE menu's 15 would overflow here. The
     * measurement is reported at startup - see toolbox_measure_menu - and the entry
     * strings are the thing to shorten if a larger font is ever wanted. */
    for (int i = 0; i < 3; i++) {
        toolbox_make_button(panel, entries[i], onEvent, (void *)(intptr_t)(i + 1), 72,
                            &lv_font_montserrat_28);
    }

    if (toolbox_measure_menu(panel, TAG) > 0) {
        ESP_LOGW(TAG, "  some menu entries do not fit their button at this font size");
    }

    /* The co-processor status line the spec asks for: "this co-processor cannot
     * inject" belongs here, not as a failure at Start. */
    lv_obj_t *info = lv_label_create(panel);
    lv_obj_set_width(info, LV_PCT(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(info, TOOLBOX_FONT_DETAIL, 0);

    wifi_toolbox_caps_t caps;
    char text[192];
    if (wifi_toolbox_host_query_caps(&caps) == ESP_OK) {
        /* Plain ASCII only. LVGL's built-in Montserrat fonts carry one cmap for
         * U+0020..U+007E and nothing between there and U+00B0, so typographic
         * characters are simply absent: an em-dash draws as a placeholder box, which
         * on a one-line 12 px label reads as a stray mark rather than as text. This
         * line used to carry one, and that stray mark is what it looked like. */
        snprintf(text, sizeof(text),
                 "Co-processor: rpc v%u fw v%u - injection %s, capture %s, raw-frame patch %s, %u modes",
                 caps.rpc_version, caps.fw_version,
                 caps.injection_available ? "available" : "NOT AVAILABLE",
                 caps.capture_available ? "available" : "unavailable",
                 caps.raw_frame_patch ? "applied" : "MISSING",
                 caps.mode_count);
    } else {
        snprintf(text, sizeof(text), "Co-processor: not answering yet");
    }
    lv_label_set_text(info, text);

    (void)area;
}

/* ---- Capture screen ------------------------------------------------------ */

void ToolboxApp::buildCapture(void)
{
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = make_screen(screens[SCREEN_CAPTURE], area, "PMKID / EAPOL Capture", LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)100);

    /* Channel, or hop. */
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ch_label = lv_label_create(row);
    lv_label_set_text(ch_label, "Channel");

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13");
    lv_dropdown_set_selected(dd, 5);            /* channel 6 */
    cap_channel_dd = dd;

    lv_obj_t *hop_label = lv_label_create(row);
    lv_label_set_text(hop_label, "Hop");

    lv_obj_t *hop = lv_switch_create(row);
    cap_hop_switch = hop;

    lv_obj_t *elicit_label = lv_label_create(row);
    lv_label_set_text(elicit_label, "Elicit deauth");

    lv_obj_t *elicit = lv_switch_create(row);
    cap_elicit_switch = elicit;

    make_label_row(panel, "State", &cap_state_label);
    make_label_row(panel, "Counters", &cap_counters_label);
    make_label_row(panel, "PCAP file", &cap_path_label);

    /* Target BSSID for elicitation: an empty field means broadcast, which the
     * arming dialog then says out loud. */
    make_field(panel, "Target AP", "aa:bb:cc:dd:ee:ff", &inj_bssid_ta);

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *start = lv_btn_create(buttons);
    lv_obj_t *start_txt = lv_label_create(start);
    lv_label_set_text(start_txt, "Arm & Start");
    lv_obj_add_event_cb(start, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)201);

    lv_obj_t *stop = lv_btn_create(buttons);
    lv_obj_t *stop_txt = lv_label_create(stop);
    lv_label_set_text(stop_txt, "Stop");
    lv_obj_add_event_cb(stop, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)202);

    lv_obj_t *write = lv_btn_create(buttons);
    lv_obj_t *write_txt = lv_label_create(write);
    lv_label_set_text(write_txt, "Write PCAP");
    lv_obj_add_event_cb(write, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)203);

    cap_log = make_log(panel);
}

/* ---- Inject screen ------------------------------------------------------- */

void ToolboxApp::buildInject(void)
{
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = make_screen(screens[SCREEN_INJECT], area, "Frame Injection", LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)100);

    /* Nine modes: Karma is absent because the original's only broadcasts probe
     * responses from the inject timer and real per-station Karma needs the receive
     * path (see the protocol header). */
    static const char *modes =
        "Beacon spam\nProbe flood\nDeauth burst\nAuth flood\nAssoc flood\n"
        "Assoc sleep\nDisassoc\nClient->AP deauth\nNAV / Duration";
    lv_obj_t *mrow = lv_obj_create(panel);
    lv_obj_set_width(mrow, LV_PCT(100));
    lv_obj_set_height(mrow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(mrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mlabel = lv_label_create(mrow);
    lv_label_set_text(mlabel, "Mode");
    lv_obj_set_width(mlabel, 90);

    inj_mode_dd = lv_dropdown_create(mrow);
    lv_dropdown_set_options(inj_mode_dd, modes);

    make_field(panel, "SSID", "beacon SSID", &inj_ssid_ta);
    make_field(panel, "BSSID", "02:11:22:33:44:55", &inj_bssid_ta);
    make_label_row(panel, "Vendor", &inj_bssid_vendor);
    make_field(panel, "Client MAC", "ff:ff:ff:ff:ff:ff = broadcast", &inj_client_ta);

    lv_obj_t *crow = lv_obj_create(panel);
    lv_obj_set_width(crow, LV_PCT(100));
    lv_obj_set_height(crow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(crow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *clabel = lv_label_create(crow);
    lv_label_set_text(clabel, "Channel");
    lv_obj_set_width(clabel, 90);

    inj_channel_dd = lv_dropdown_create(crow);
    lv_dropdown_set_options(inj_channel_dd, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13");
    lv_dropdown_set_selected(inj_channel_dd, 5);

    /* Duration: mandatory and bounded. The slider cannot express 0, which is the
     * point — the spec requires a duration on every session. */
    lv_obj_t *drow = lv_obj_create(panel);
    lv_obj_set_width(drow, LV_PCT(100));
    lv_obj_set_height(drow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(drow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(drow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(drow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dlabel = lv_label_create(drow);
    lv_label_set_text(dlabel, "Duration");
    lv_obj_set_width(dlabel, 90);

    inj_duration_slider = lv_slider_create(drow);
    lv_obj_set_flex_grow(inj_duration_slider, 1);
    lv_slider_set_range(inj_duration_slider, 5, 600);
    lv_slider_set_value(inj_duration_slider, 30, LV_ANIM_OFF);
    lv_obj_add_event_cb(inj_duration_slider, onEvent, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)300);

    inj_duration_label = lv_label_create(drow);
    lv_label_set_text(inj_duration_label, "30 s");

    make_label_row(panel, "Counters", &inj_counters_label);

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *start = lv_btn_create(buttons);
    lv_obj_t *start_txt = lv_label_create(start);
    lv_label_set_text(start_txt, "Arm & Start");
    lv_obj_add_event_cb(start, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)301);

    lv_obj_t *stop = lv_btn_create(buttons);
    lv_obj_t *stop_txt = lv_label_create(stop);
    lv_label_set_text(stop_txt, "Stop");
    lv_obj_add_event_cb(stop, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)302);

    lv_obj_t *disarm = lv_btn_create(buttons);
    lv_obj_t *disarm_txt = lv_label_create(disarm);
    lv_label_set_text(disarm_txt, "Disarm");
    lv_obj_add_event_cb(disarm, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)303);

    inj_log = make_log(panel);
}

/* ---- Network screen ------------------------------------------------------ */

void ToolboxApp::buildNetwork(void)
{
    lv_area_t area = getVisualArea();
    lv_obj_t *panel = make_screen(screens[SCREEN_NETWORK], area, "WiFi + Net Utilities", LV_SYMBOL_LEFT, onEvent, (void *)(intptr_t)100);

    lv_obj_t *mrow = lv_obj_create(panel);
    lv_obj_set_width(mrow, LV_PCT(100));
    lv_obj_set_height(mrow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(mrow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mlabel = lv_label_create(mrow);
    lv_label_set_text(mlabel, "Mode");
    lv_obj_set_width(mlabel, 90);

    net_mode_dd = lv_dropdown_create(mrow);
    lv_dropdown_set_options(net_mode_dd, "Host sweep\nSSH (22)\nTelnet (23)\nPort preset");

    lv_obj_t *prow = lv_obj_create(panel);
    lv_obj_set_width(prow, LV_PCT(100));
    lv_obj_set_height(prow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *plabel = lv_label_create(prow);
    lv_label_set_text(plabel, "Preset");
    lv_obj_set_width(plabel, 90);

    net_preset_dd = lv_dropdown_create(prow);
    lv_dropdown_set_options(net_preset_dd, "Common\nWeb\nIoT/SCADA\nWindows/SMB");

    make_field(panel, "Target", "192.168.1.1 (blank = this device's subnet)", &net_target_ta);

    lv_obj_t *buttons = lv_obj_create(panel);
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 8, 0);
    lv_obj_clear_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *start = lv_btn_create(buttons);
    lv_obj_t *start_txt = lv_label_create(start);
    lv_label_set_text(start_txt, "Start scan");
    lv_obj_add_event_cb(start, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)401);

    lv_obj_t *stop = lv_btn_create(buttons);
    lv_obj_t *stop_txt = lv_label_create(stop);
    lv_label_set_text(stop_txt, "Stop");
    lv_obj_add_event_cb(stop, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)402);

    make_label_row(panel, "Progress", &net_stats_label);

    /* Results are a rolling label rather than a list: a sweep produces a handful
     * of lines, and a list would need scrolling state that the counters do not. */
    net_results = lv_label_create(panel);
    lv_obj_set_width(net_results, LV_PCT(100));
    lv_obj_set_flex_grow(net_results, 1);
    lv_label_set_long_mode(net_results, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(net_results, TOOLBOX_FONT_DETAIL, 0);
    lv_label_set_text(net_results, "");
}

/* ---- run() --------------------------------------------------------------- */

bool ToolboxApp::run(void)
{
    lv_area_t area = getVisualArea();

    /* See init(): the module is brought up here, not at install time. */
    (void)ensureHost();

    /* One container per screen, created up front so switching is a visibility
     * change rather than a rebuild — the counters update at 1 Hz and rebuilding
     * LVGL objects on every tick would be wasteful and would fight the user's
     * scrolling.
     *
     * The containers are plain viewports, each holding exactly one child panel of
     * its own size and nothing else. Deliberately no layout is set on them: in LVGL
     * 8.4 an object with no flex or grid flow leaves its children where they are
     * placed, which is what lv_obj_align() in make_screen() needs. The menu is the
     * exception — buildMenu() draws into its container directly and gives it a flex
     * column of its own. */
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

    /* The banner sits above the screens so it is visible on every one of them. */
    if (banner == nullptr) {
        banner = lv_obj_create(lv_screen_active());
        lv_obj_set_size(banner, area.x2 - area.x1 + 1, 34);
        lv_obj_align(banner, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_color(banner, lv_color_make(200, 40, 40), 0);
        lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(banner, 0, 0);
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);

        banner_label = lv_label_create(banner);
        lv_label_set_text(banner_label, "");
        lv_obj_center(banner_label);
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    }

    buildMenu();
    buildCapture();
    buildInject();
    buildNetwork();

    showScreen(SCREEN_MENU);

    if (ui_timer == nullptr) {
        /* 500 ms: fast enough that a 1 Hz status push is rendered promptly, slow
         * enough to be free. This is the only place LVGL is touched from the
         * toolbox's state. */
        ui_timer = lv_timer_create(statusTick, 500, this);
    }

    lv_obj_clear_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

    return true;
}

bool ToolboxApp::back(void)
{
    if (active_screen != SCREEN_MENU) {
        showScreen(SCREEN_MENU);
        return true;
    }

    notifyCoreClosed();
    return true;
}

bool ToolboxApp::close(void)
{
    /* A session outliving the screen is exactly the leftover the spec rules out
     * (section 9), so closing the app stops it. The armed flag goes with it. */
    if (wifi_toolbox_host_is_armed()) {
        (void)wifi_toolbox_host_disarm();
    }

    (void)wifi_toolbox_host_stop();
    wifi_toolbox_pcap_stop();
    wifi_toolbox_scan_stop();

    if (ui_timer != nullptr) {
        lv_timer_del(ui_timer);
        ui_timer = nullptr;
    }

    /* The arming dialog lives on lv_layer_top(), not on this app's default screen,
     * so neither the core's cleanDefaultScreen() nor anything else will remove it.
     * Closing the app with the dialog open would otherwise leave the mask covering
     * the launcher permanently. */
    if (arm_modal != nullptr) {
        lv_obj_del(arm_modal);
        arm_modal = nullptr;
    }

    /* Every LVGL object this app created lives on the core's default screen, which
     * the framework deletes when the app closes. The pointers to them must be
     * cleared in the same breath, or the next run() sees a non-null pointer, skips
     * rebuilding, and calls lv_obj_add_event_cb on freed memory — a use-after-free
     * that presents as the screen going blank and the device needing a reset.
     *
     * Nulling them is what makes run() rebuild from scratch. It is also why run()
     * tests each pointer instead of assuming this is the first launch. */
    for (int i = 0; i < SCREEN_COUNT; i++) {
        screens[i] = nullptr;
    }

    banner = nullptr;
    banner_label = nullptr;

    cap_state_label = nullptr;
    cap_counters_label = nullptr;
    cap_path_label = nullptr;
    cap_channel_dd = nullptr;
    cap_hop_switch = nullptr;
    cap_elicit_switch = nullptr;
    cap_log = nullptr;

    inj_mode_dd = nullptr;
    inj_ssid_ta = nullptr;
    inj_bssid_ta = nullptr;
    inj_client_ta = nullptr;
    inj_channel_dd = nullptr;
    inj_duration_slider = nullptr;
    inj_duration_label = nullptr;
    inj_counters_label = nullptr;
    inj_log = nullptr;

    net_mode_dd = nullptr;
    net_preset_dd = nullptr;
    net_target_ta = nullptr;
    net_results = nullptr;
    net_stats_label = nullptr;

    active_screen = SCREEN_MENU;

    return true;
}

/* ---- Arming dialog ------------------------------------------------------- */

void ToolboxApp::askArm(bool for_injection, uint8_t channel, uint32_t duration_s, const char *mode_name)
{
    arm_pending_inject = for_injection;
    arm_pending_channel = channel;
    arm_pending_duration_s = duration_s;
    snprintf(arm_pending_mode, sizeof(arm_pending_mode), "%s", (mode_name != nullptr) ? mode_name : "-");

    char text[640];
    snprintf(text, sizeof(text),
             "%s\n\n"
             "About to arm:\n"
             "  what:      %s\n"
             "  channel:   %u\n"
             "  duration:  %lu s\n"
             "  from:      the ESP32-C6 co-processor, not this chip\n\n"
             "Arming opens the co-processor's gate for this one mode.",
             k_auth_notice,
             arm_pending_mode,
             (unsigned)channel,
             (unsigned long)duration_s);

    /* A modal built by hand rather than lv_msgbox: the stock message box's button
     * layout in LVGL 8.4 puts both actions in one row and offers no way to label
     * them with anything but a single string each, and the two labels here are the
     * whole point — "I am authorised" versus "Cancel" is the decision the dialog
     * exists to force. */
    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    arm_modal = mask;
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
    lv_label_set_text(title, for_injection ? "Confirm injection" : "Confirm capture");
    lv_obj_set_style_text_font(title, TOOLBOX_FONT_TITLE, 0);

    lv_obj_t *body = lv_label_create(box);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(body, text);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_btn_create(row);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, onArmCancelled, LV_EVENT_CLICKED, this);
    lv_obj_t *cancel_txt = lv_label_create(cancel);
    lv_label_set_text(cancel_txt, "Cancel");
    lv_obj_center(cancel_txt);

    lv_obj_t *ok = lv_btn_create(row);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_style_bg_color(ok, lv_color_make(200, 40, 40), 0);
    lv_obj_add_event_cb(ok, onArmConfirmed, LV_EVENT_CLICKED, this);
    lv_obj_t *ok_txt = lv_label_create(ok);
    lv_label_set_text(ok_txt, "I am authorised");
    lv_obj_center(ok_txt);
}

/* The modal's mask is two levels up: button -> row -> box -> mask. */
static void close_modal(lv_obj_t *target)
{
    lv_obj_t *row = lv_obj_get_parent(target);
    lv_obj_t *box = lv_obj_get_parent(row);
    lv_obj_t *mask = lv_obj_get_parent(box);

    if (mask != nullptr) {
        lv_obj_del(mask);
    }
}

void ToolboxApp::onArmCancelled(lv_event_t *e)
{
    ToolboxApp *app = (ToolboxApp *)lv_event_get_user_data(e);

    close_modal((lv_obj_t *)lv_event_get_target(e));

    if (app != nullptr) {
        /* The dialog is gone; drop the handle so close() does not try to delete it
         * twice. Without this the app holds a dangling pointer to freed memory. */
        app->arm_modal = nullptr;
        ESP_LOGI(TAG, "arming cancelled");
    }
}

/* ---- Arming confirm: this is where a session actually starts ------------- */

void ToolboxApp::onArmConfirmed(lv_event_t *e)
{
    ToolboxApp *app = (ToolboxApp *)lv_event_get_user_data(e);

    close_modal((lv_obj_t *)lv_event_get_target(e));

    if (app == nullptr) {
        return;
    }

    app->arm_modal = nullptr;

    /* The acknowledgement is passed through as true because the button that got
     * here says so in words. The co-processor refuses an arm without it, which is
     * the copy that matters: this end cannot be the one enforcing its own gate. */
    const wifi_toolbox_arm_mode_t mode = app->arm_pending_inject ? WIFI_TOOLBOX_ARM_INJECT
                                                                : WIFI_TOOLBOX_ARM_CAPTURE;

    if (wifi_toolbox_host_arm(mode, true) != ESP_OK) {
        lv_label_set_text(app->arm_pending_inject ? app->inj_log : app->cap_log,
                          "arming failed: the co-processor did not answer");
        return;
    }

    esp_err_t err = ESP_FAIL;

    if (app->arm_pending_inject) {
#if CONFIG_TOOLBOX_ALLOW_INJECTION
        wifi_toolbox_inject_params_t p;
        memset(&p, 0, sizeof(p));

        p.mode = (uint8_t)dd_selected(app->inj_mode_dd);
        p.channel = (uint8_t)(dd_selected(app->inj_channel_dd) + 1);

        const uint32_t duration_s = (uint32_t)lv_slider_get_value(app->inj_duration_slider);
        const uint32_t duration_ms = duration_s * 1000u;
        p.duration_ms_lo = (uint16_t)(duration_ms & 0xFFFFu);
        p.duration_ms_hi = (uint16_t)((duration_ms >> 16) & 0xFFFFu);

        const char *ssid = lv_textarea_get_text(app->inj_ssid_ta);
        const size_t ssid_len = (ssid != nullptr) ? strlen(ssid) : 0;
        p.ssid_len = (uint8_t)((ssid_len > 32) ? 32 : ssid_len);
        if (p.ssid_len > 0) {
            memcpy(p.ssid, ssid, p.ssid_len);
        }

        if (!wifi_toolbox_parse_mac(lv_textarea_get_text(app->inj_bssid_ta), p.bssid)) {
            /* No BSSID given: a locally-administered one, so a beacon cannot be
             * confused with a real AP's and the operator is not spoofing anybody
             * by accident. */
            const uint8_t fallback[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
            memcpy(p.bssid, fallback, sizeof(p.bssid));
        }

        if (!wifi_toolbox_parse_mac(lv_textarea_get_text(app->inj_client_ta), p.client)) {
            const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            memcpy(p.client, bcast, sizeof(p.client));
        }

        err = wifi_toolbox_host_inject_start(&p);
        lv_label_set_text(app->inj_log, (err == ESP_OK) ? "session started" : "start failed");
#else
        /* Refused here, and the co-processor refuses too because its image was
         * built without injection (spec 10.5: the flag has to exist on both). */
        lv_label_set_text(app->inj_log, "this build does not offer frame injection");
        err = ESP_ERR_NOT_SUPPORTED;
#endif
    } else {
        wifi_toolbox_capture_params_t p;
        memset(&p, 0, sizeof(p));

        const bool hop = lv_obj_has_state(app->cap_hop_switch, LV_STATE_CHECKED);
        p.hop = hop ? 1 : 0;
        p.channel = (uint8_t)(dd_selected(app->cap_channel_dd) + 1);
        p.hop_dwell_ms = WIFI_TOOLBOX_HOP_DWELL_MS_DEF;
        p.filter_mask = WIFI_TOOLBOX_CAP_FILTER_DEFAULT;
        (void)wifi_toolbox_parse_mac(lv_textarea_get_text(app->inj_bssid_ta), p.target_bssid);

        /* A file is opened only when the switch asks for one: the counters are the
         * point on this screen, and an implicit file would fill the card. */
        if (lv_obj_has_state(app->cap_elicit_switch, LV_STATE_CHECKED)) {
            p.elicit_ms = 5000;
        }

        err = wifi_toolbox_host_capture_start(&p);
        lv_label_set_text(app->cap_log, (err == ESP_OK) ? "capture started" : "start failed");
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session start failed: %s", esp_err_to_name(err));
    }
}

/* ---- Events -------------------------------------------------------------- */

void ToolboxApp::onEvent(lv_event_t *e)
{
    /* The action code travels in the event's user_data, so it has to be read from
     * there — lv_event_get_param carries the slider's new value for the duration
     * control and is not an action code. */
    const int code = (int)(intptr_t)lv_event_get_user_data(e);

    /* The app instance comes from the static, NOT from the event's user_data.
     *
     * Every registration of this handler passes an action code as its user_data
     * (`(void *)(intptr_t)201` and so on), because that is what the handler needs to
     * know which control was touched. Reading the instance out of the same field
     * therefore yields the code, not a pointer: casting it produced `(ToolboxApp*)1`
     * and the first menu tap dereferenced address 1.
     *
     * It went unnoticed because taps could not reach a menu button while the screen
     * nesting was wrong — see make_screen(). Fixing the nesting is what exposed it.
     * Only the arming dialog's two callbacks pass `this` as user_data, and those are
     * separate handlers. */
    ToolboxApp *app = g_app;

    /* The action code is the user_data, always. Every registration of this handler
     * passes one — 1/2/3 for the menu entries, 100 for a back button, 201-203, 300,
     * 301-303, 401-402 for the controls.
     *
     * There used to be a fallback here that read lv_event_get_param(e) when the code
     * looked out of range, on the theory that the builders passed the action twice.
     * They do not. In LVGL 8.x `param` is "arbitrary data depending on the widget
     * type and the event. (Usually NULL)"; for a real touch it is the input device
     * pointer. The menu codes are 1-3, which are below the range the test allowed,
     * so every menu tap replaced a perfectly good action code with a pointer value,
     * matched no case, and did nothing at all. The bounds test was the bug. */
    const int action = code;

    if (app == nullptr) {
        return;
    }

    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
    wifi_toolbox_status_t st;

    switch (action) {
    case 1: app->showScreen(SCREEN_CAPTURE); break;
    case 2: app->showScreen(SCREEN_INJECT); break;
    case 3: app->showScreen(SCREEN_NETWORK); break;
    case 100: app->showScreen(SCREEN_MENU); break;
    case 300: {
        const int v = (int)lv_slider_get_value(target);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d s", v);
        lv_label_set_text(app->inj_duration_label, buf);
        break;
    }

    case 201: {   /* capture: arm and start */
        const uint8_t channel = (uint8_t)(dd_selected(app->cap_channel_dd) + 1);
        const bool hop = lv_obj_has_state(app->cap_hop_switch, LV_STATE_CHECKED);
        app->askArm(false, hop ? 0 : channel, 0, hop ? "capture, hopping the 2.4 GHz band"
                                                     : "capture on one channel");
        break;
    }

    case 202:     /* capture: stop */
        (void)wifi_toolbox_host_stop();
        wifi_toolbox_pcap_stop();
        lv_label_set_text(app->cap_log, "stopped");
        break;

    case 203:     /* capture: start or stop writing a PCAP */
        if (wifi_toolbox_pcap_is_open()) {
            wifi_toolbox_pcap_stop();
            lv_label_set_text(app->cap_log, "PCAP closed");
        } else {
            const esp_err_t err = wifi_toolbox_pcap_start("capture");
            lv_label_set_text(app->cap_log, (err == ESP_OK) ? "PCAP opened"
                                                            : "cannot write: is the SD card in?");
        }
        break;

    case 301: {   /* inject: arm and start */
        const uint32_t dur = (uint32_t)lv_slider_get_value(app->inj_duration_slider);
        const char *mode = dd_text(app->inj_mode_dd);
        char mode_copy[24];
        snprintf(mode_copy, sizeof(mode_copy), "%s", (mode != nullptr) ? mode : "injection");
        app->askArm(true, (uint8_t)(dd_selected(app->inj_channel_dd) + 1), dur, mode_copy);
        break;
    }

    case 302:     /* inject: stop */
        (void)wifi_toolbox_host_stop();
        lv_label_set_text(app->inj_log, "stopped");
        break;

    case 303:     /* inject: disarm */
        (void)wifi_toolbox_host_disarm();
        lv_label_set_text(app->inj_log, "disarmed - the co-processor's gate is shut");
        break;

    case 401: {   /* network: start */
        wifi_toolbox_scan_request_t req;
        memset(&req, 0, sizeof(req));

        const uint16_t mode = dd_selected(app->net_mode_dd);
        req.mode = (wifi_toolbox_scan_mode_t)mode;
        req.preset = (wifi_toolbox_port_preset_t)dd_selected(app->net_preset_dd);

        const char *target = lv_textarea_get_text(app->net_target_ta);
        if ((target != nullptr) && (target[0] != '\0')) {
            snprintf(req.target, sizeof(req.target), "%s", target);
        }

        /* The scan reports through callbacks that run on its own task, and those
         * must not touch LVGL, so results are appended by the UI timer instead —
         * see onScanProgress/onScanDone. */
        if (scan_start_ui(&req) != ESP_OK) {
            lv_label_set_text(app->net_results, "cannot start: connect to a network first, "
                                                "and give a target unless sweeping the subnet");
        } else {
            lv_label_set_text(app->net_results, "");
        }
        break;
    }

    case 402:     /* network: stop */
        wifi_toolbox_scan_stop();
        lv_label_set_text(app->net_stats_label, "stopped");
        break;

    default:
        break;
    }

    (void)st;
}

/* ---- Rendering ----------------------------------------------------------- */

void ToolboxApp::logLine(const char *text)
{
    ESP_LOGI(TAG, "%s", text);
}

void ToolboxApp::updateBanner(void)
{
    if (banner == nullptr) {
        return;
    }

    wifi_toolbox_status_t st;
    if (!wifi_toolbox_host_status(&st)) {
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const bool injecting = (st.radio.inject_running != 0) ||
                           (st.inject.state == WIFI_TOOLBOX_STATE_INJECTING);
    const bool capturing = (st.radio.capture_running != 0);

    if (!injecting && !capturing) {
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char text[160];
    if (injecting) {
        /* The wording matters: the counters say the driver accepted the frames,
         * and the on-air check has not been passed on this hardware, so claiming
         * "transmitting" would be a claim the evidence does not support. */
        snprintf(text, sizeof(text),
                 LV_SYMBOL_WARNING " INJECTING on ch%u - %lu frames accepted by the driver (%lu failed)",
                 st.inject.channel, (unsigned long)st.inject.frames_sent,
                 (unsigned long)st.inject.frames_failed);
    } else {
        snprintf(text, sizeof(text),
                 LV_SYMBOL_EYE_OPEN " CAPTURING%s - %lu frames, %lu dropped",
                 st.radio.hopping ? " (hopping)" : "",
                 (unsigned long)st.capture.frames_sent, (unsigned long)st.capture.frames_dropped);
    }

    lv_label_set_text(banner_label, text);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

void ToolboxApp::refreshFromStatus(void)
{
    wifi_toolbox_status_t st;
    const bool have = wifi_toolbox_host_status(&st);

    char buf[192];

    /* Menu-independent: the banner is the always-visible part. */
    updateBanner();

    if (!have) {
        if (cap_state_label != nullptr) {
            lv_label_set_text(cap_state_label, "no status yet");
        }
        return;
    }

    if (cap_state_label != nullptr) {
        snprintf(buf, sizeof(buf), "%s%s  (link %s)",
                 wifi_toolbox_state_name(st.capture.state),
                 st.radio.hopping ? ", hopping" : "",
                 g_latch.link_up ? "up" : "DOWN");
        lv_label_set_text(cap_state_label, buf);
    }

    if (cap_counters_label != nullptr) {
        snprintf(buf, sizeof(buf), "seen %lu  matched %lu  sent %lu  drop %lu  elicits %lu",
                 (unsigned long)st.capture.packets_seen, (unsigned long)st.capture.frames_matched,
                 (unsigned long)st.capture.frames_sent, (unsigned long)st.capture.frames_dropped,
                 (unsigned long)st.capture.elicits);
        lv_label_set_text(cap_counters_label, buf);
    }

    if (cap_path_label != nullptr) {
        wifi_toolbox_pcap_stats_t ps;
        wifi_toolbox_pcap_get_stats(&ps);
        if (ps.path[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s  (%lu pkts, %llu B%s)",
                     ps.path, (unsigned long)ps.packets, (unsigned long long)ps.bytes,
                     ps.stopped_for_space ? ", STOPPED: card full" : "");
        } else {
            snprintf(buf, sizeof(buf), "not writing");
        }
        lv_label_set_text(cap_path_label, buf);
    }

    if (inj_counters_label != nullptr) {
        snprintf(buf, sizeof(buf), "%s  sent %lu  failed %lu  %lu ms",
                 wifi_toolbox_state_name(st.inject.state),
                 (unsigned long)st.inject.frames_sent, (unsigned long)st.inject.frames_failed,
                 (unsigned long)st.inject.elapsed_ms);
        lv_label_set_text(inj_counters_label, buf);
    }

    if (net_stats_label != nullptr && wifi_toolbox_scan_is_running()) {
        snprintf(buf, sizeof(buf), "scanning...");
        lv_label_set_text(net_stats_label, buf);
    }
}

void ToolboxApp::statusTick(lv_timer_t *timer)
{
    ToolboxApp *app = (ToolboxApp *)timer->user_data;

    if (app == nullptr) {
        return;
    }

    if (g_latch.link_changed) {
        g_latch.link_changed = false;

        /* A dropped link stops the session on the co-processor's side; the banner
         * has to stop claiming otherwise, and the gate is already cleared. */
        if (!g_latch.link_up && app->inj_log != nullptr) {
            lv_label_set_text(app->inj_log, "co-processor link dropped - session stopped, gate cleared");
        }
    }

    /* Reflect the vendor of the BSSID typed into the inject/capture target field. */
    if (app->inj_bssid_vendor != nullptr && app->inj_bssid_ta != nullptr) {
        uint8_t mac[6];
        const char *vendor = nullptr;
        if (wifi_toolbox_parse_mac(lv_textarea_get_text(app->inj_bssid_ta), mac)) {
            vendor = vendor_lookup_oui(mac);
        }
        lv_label_set_text(app->inj_bssid_vendor, vendor ? vendor : "unknown");
    }

    /* Scan results: appended here rather than in the scan callbacks, which run on
     * the scan task and must not touch LVGL. */
    if (app->net_results != nullptr) {
        if (g_latch.scan_pending > 0) {
            const uint32_t n = g_latch.scan_pending;
            g_latch.scan_pending = 0;

            for (uint32_t i = 0; i < n; i++) {
                lv_label_ins_text(app->net_results, LV_LABEL_POS_LAST, "\n");
                lv_label_ins_text(app->net_results, LV_LABEL_POS_LAST, g_latch.scan_lines[i]);
            }
        }

        if (app->net_stats_label != nullptr && g_latch.scan_stats[0] != '\0') {
            lv_label_set_text(app->net_stats_label, g_latch.scan_stats);
            g_latch.scan_stats[0] = '\0';
        }

        if (g_latch.scan_done) {
            g_latch.scan_done = false;
            if (g_latch.scan_err != ESP_OK) {
                lv_label_ins_text(app->net_results, LV_LABEL_POS_LAST, "\n");
                lv_label_ins_text(app->net_results, LV_LABEL_POS_LAST,
                                  "scan ended early (stopped, or no network)");
            }
        }
    }

    if (g_latch.status_dirty) {
        g_latch.status_dirty = false;
        app->refreshFromStatus();
    }
}



