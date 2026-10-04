/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen Capture phone app.
 */
#include "ScreenCaptureApp.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "screen_capture.h"
#include "toolbox_icons.hpp"
#include "FilePicker.hpp"

static const char *TAG = "ScreenCapture";

enum { ACT_SHOT = 1, ACT_REC = 2, ACT_VIEW = 3, ACT_BACK = 4 };

/* ---- Launcher icon: a display with capture brackets --------------------------
 *
 * A "screenshot" mark: a monitor outline plus the four corner brackets that mean
 * "this region is what gets captured". Derived from the screenshot_monitor mark in
 * Material Symbols (Apache-2.0), redrawn as strokes so it matches the toolbox icons.
 *
 * In the 24x24 space used below the display is (2.5,4.5)-(21.5,16.5) with r=2 corners,
 * the stand hangs under it, and the brackets frame (6,7.5)-(18,13.5) with a gap at the
 * middle of each side. The stroke-inclusive bounding box is (1.5,3.5)-(22.5,20.5),
 * which the transform centres in the 112 px icon as  icon = 8 + svg * 4. */

typedef struct {
    float scale;    /* icon px per SVG unit */
    float off;      /* icon px offset of SVG origin (0,0) */
} screen_capture_glyph_t;

static const screen_capture_glyph_t k_capture_glyph = {
    .scale = 4.0f,
    .off = 8.0f,
};

/* The painter supersamples 16x per pixel, so a glyph with this many primitives is
 * evaluated about 3.6 million times. Each primitive therefore gets a bounding-box
 * reject first: a handful of compares, against the sqrt/atan2f the full test costs.
 * Without it this icon took seconds to paint and tripped the task watchdog at boot. */

/* Distance to a circular arc, clamped at its two endpoints. Angles are SVG's: degrees,
 * 0 = +x, increasing clockwise because the y axis points down. */
static bool arc_hit(float x, float y, float cx, float cy, float r,
                    float a1, float a2, float half)
{
    const float reach = r + half;
    if (x < cx - reach || x > cx + reach || y < cy - reach || y > cy + reach) {
        return false;
    }

    const float dx = x - cx;
    const float dy = y - cy;
    const float dist = sqrtf((dx * dx) + (dy * dy));

    float ang = atan2f(dy, dx) * (180.0f / (float)M_PI);
    if (ang < 0.0f) {
        ang += 360.0f;
    }

    if (ang >= a1 && ang <= a2) {
        return fabsf(dist - r) <= half;
    }

    /* Outside the sweep the nearest point is one of the arc's ends. */
    float best = 1e9f;
    const float ends[2] = { a1, a2 };
    for (int i = 0; i < 2; i++) {
        const float ra = ends[i] * ((float)M_PI / 180.0f);
        const float ex = cx + (r * cosf(ra));
        const float ey = cy + (r * sinf(ra));
        const float d = sqrtf(((x - ex) * (x - ex)) + ((y - ey) * (y - ey)));
        if (d < best) {
            best = d;
        }
    }
    return best <= half;
}

static bool seg_hit(float x, float y, float ax, float ay, float bx, float by, float half)
{
    if (x < fminf(ax, bx) - half || x > fmaxf(ax, bx) + half ||
        y < fminf(ay, by) - half || y > fmaxf(ay, by) + half) {
        return false;
    }
    return toolbox_icon_dist_to_segment(x, y, ax, ay, bx, by) <= half;
}

static bool screen_capture_is_glyph(float x, float y, void *ctx)
{
    const screen_capture_glyph_t *g = (const screen_capture_glyph_t *)ctx;

    /* The icon is supersampled by the painter; reject the cheap half first. */
    if (x < g->off || y < g->off) {
        return false;
    }

    const float sx = (x - g->off) / g->scale;
    const float sy = (y - g->off) / g->scale;
    const float half = 1.0f;    /* the drawn stroke-width 2, halved, in SVG units */

    /* Display: the four rounded corners. */
    if (arc_hit(sx, sy, 4.5f, 6.5f, 2.0f, 180.0f, 270.0f, half)) return true;
    if (arc_hit(sx, sy, 19.5f, 6.5f, 2.0f, 270.0f, 360.0f, half)) return true;
    if (arc_hit(sx, sy, 19.5f, 14.5f, 2.0f, 0.0f, 90.0f, half)) return true;
    if (arc_hit(sx, sy, 4.5f, 14.5f, 2.0f, 90.0f, 180.0f, half)) return true;

    /* Display: the edges. */
    if (seg_hit(sx, sy, 4.5f, 4.5f, 19.5f, 4.5f, half)) return true;
    if (seg_hit(sx, sy, 4.5f, 16.5f, 19.5f, 16.5f, half)) return true;
    if (seg_hit(sx, sy, 2.5f, 6.5f, 2.5f, 14.5f, half)) return true;
    if (seg_hit(sx, sy, 21.5f, 6.5f, 21.5f, 14.5f, half)) return true;

    /* The stand. */
    if (seg_hit(sx, sy, 12.0f, 16.5f, 12.0f, 19.5f, half)) return true;
    if (seg_hit(sx, sy, 8.5f, 19.5f, 15.5f, 19.5f, half)) return true;

    /* The capture brackets, gapped at the middle of each side so they read as a frame
     * rather than as a second rectangle. */
    if (seg_hit(sx, sy, 6.0f, 7.5f, 9.0f, 7.5f, half)) return true;
    if (seg_hit(sx, sy, 6.0f, 7.5f, 6.0f, 10.5f, half)) return true;
    if (seg_hit(sx, sy, 15.0f, 7.5f, 18.0f, 7.5f, half)) return true;
    if (seg_hit(sx, sy, 18.0f, 7.5f, 18.0f, 10.5f, half)) return true;
    if (seg_hit(sx, sy, 6.0f, 10.5f, 6.0f, 13.5f, half)) return true;
    if (seg_hit(sx, sy, 6.0f, 13.5f, 9.0f, 13.5f, half)) return true;
    if (seg_hit(sx, sy, 18.0f, 10.5f, 18.0f, 13.5f, half)) return true;
    if (seg_hit(sx, sy, 15.0f, 13.5f, 18.0f, 13.5f, half)) return true;

    return false;
}

static toolbox_icon_state_t s_icon_state;

static const lv_image_dsc_t *screen_capture_launcher_icon(void)
{
    /* Teal, so it reads apart from the two blue toolbox apps. */
    return toolbox_icon_paint(&s_icon_state, 0x14, 0x9E, 0x8C,
                              screen_capture_is_glyph, (void *)&k_capture_glyph, TAG);
}

static ScreenCaptureApp *g_app = nullptr;

ScreenCaptureApp::ScreenCaptureApp()
    : ESP_Brookesia_PhoneApp(
          esp_brookesia::systems::base::App::Config::SIMPLE_CONSTRUCTOR("Screen Capture", nullptr, true),
          esp_brookesia::systems::phone::App::Config::SIMPLE_CONSTRUCTOR(nullptr, true, false)),
      status_label_(nullptr),
      rec_label_(nullptr),
      ui_timer_(nullptr),
      main_panel_(nullptr),
      viewer_panel_(nullptr),
      viewer_img_(nullptr),
      viewer_data_(nullptr),
      viewer_dsc_{},
      active_screen_(SCREEN_MAIN)
{
}

ScreenCaptureApp::~ScreenCaptureApp()
{
}

bool ScreenCaptureApp::init(void)
{
    g_app = this;

    /* The launcher icon has to be set here, not in run(): the core builds the home
     * screen during start(), which is after every app's init() but before any app's
     * run(), so an icon assigned in run() arrives after the widget that shows it. */
    const lv_image_dsc_t *icon = screen_capture_launcher_icon();
    if (icon != nullptr) {
        setLauncherIconImage(esp_brookesia::gui::StyleImage::IMAGE(icon));
    }

    return true;
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    return title;
}

static lv_obj_t *make_back_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, ScreenCaptureApp::onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)ACT_BACK);
    return btn;
}

bool ScreenCaptureApp::run(void)
{
    lv_area_t area = getVisualArea();
    const int w = area.x2 - area.x1 + 1;
    const int h = area.y2 - area.y1 + 1;

    /* ---- main panel: capture controls ---- */
    main_panel_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(main_panel_, w, h);
    lv_obj_align(main_panel_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(main_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(main_panel_, 20, 0);
    lv_obj_set_style_pad_row(main_panel_, 16, 0);
    lv_obj_set_style_border_width(main_panel_, 0, 0);
    lv_obj_clear_flag(main_panel_, LV_OBJ_FLAG_SCROLLABLE);

    make_title(main_panel_, "Screen Capture");

    status_label_ = lv_label_create(main_panel_);
    lv_obj_set_width(status_label_, LV_PCT(100));
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(status_label_,
                      "Screenshot: tap, then switch to the app you want to capture "
                      "(5 s delay). Recording keeps running while you use other apps.");

    lv_obj_t *shot = lv_btn_create(main_panel_);
    lv_obj_set_width(shot, LV_PCT(100));
    lv_obj_set_height(shot, 60);
    lv_obj_t *shot_lbl = lv_label_create(shot);
    lv_label_set_text(shot_lbl, "Screenshot (5 s)");
    lv_obj_center(shot_lbl);
    lv_obj_add_event_cb(shot, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)ACT_SHOT);

    lv_obj_t *rec = lv_btn_create(main_panel_);
    lv_obj_set_width(rec, LV_PCT(100));
    lv_obj_set_height(rec, 60);
    rec_label_ = lv_label_create(rec);
    lv_label_set_text(rec_label_, "Start Recording");
    lv_obj_center(rec_label_);
    lv_obj_add_event_cb(rec, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)ACT_REC);

    lv_obj_t *view = lv_btn_create(main_panel_);
    lv_obj_set_width(view, LV_PCT(100));
    lv_obj_set_height(view, 60);
    lv_obj_t *view_lbl = lv_label_create(view);
    lv_label_set_text(view_lbl, "View Captures");
    lv_obj_center(view_lbl);
    lv_obj_add_event_cb(view, onEvent, LV_EVENT_CLICKED, (void *)(intptr_t)ACT_VIEW);

    /* Browsing the captures is the shared file picker's job; this app only needs the
     * capture controls and an image viewer. */

    /* ---- viewer panel: full-screen screenshot ---- */
    viewer_panel_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(viewer_panel_, w, h);
    lv_obj_align(viewer_panel_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(viewer_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(viewer_panel_, 20, 0);
    lv_obj_set_style_pad_row(viewer_panel_, 12, 0);
    lv_obj_set_style_border_width(viewer_panel_, 0, 0);

    lv_obj_t *v_header = lv_obj_create(viewer_panel_);
    lv_obj_set_width(v_header, LV_PCT(100));
    lv_obj_set_height(v_header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(v_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(v_header, LV_OBJ_FLAG_SCROLLABLE);
    make_back_button(v_header);
    make_title(v_header, "Capture");

    viewer_img_ = lv_image_create(viewer_panel_);
    lv_obj_align(viewer_img_, LV_ALIGN_CENTER, 0, 0);

    /* Bring the SD card and encoder up; show the outcome in the status line. */
    esp_err_t err = screen_capture_init();
    if (err != ESP_OK) {
        lv_label_set_text(status_label_, "SD card unavailable — capture will fail.");
        ESP_LOGW(TAG, "screen_capture_init: %s", esp_err_to_name(err));
    }

    if (ui_timer_ == nullptr) {
        ui_timer_ = lv_timer_create(onTick, 200, this);
    }

    showScreen(SCREEN_MAIN);
    refresh();
    return true;
}

bool ScreenCaptureApp::back(void)
{
    /* The picker is an overlay on the system layer, so it takes the back gesture first. */
    if (file_picker::is_open()) {
        file_picker::close();
        return true;
    }
    if (active_screen_ == SCREEN_VIEWER) {
        closeViewer();
        return true;
    }
    notifyCoreClosed();
    return true;
}

bool ScreenCaptureApp::close(void)
{
    file_picker::close();

    if (ui_timer_ != nullptr) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
    free(viewer_data_);
    viewer_data_ = nullptr;
    viewer_dsc_.data = nullptr;

    status_label_ = nullptr;
    rec_label_ = nullptr;
    main_panel_ = nullptr;
    viewer_panel_ = nullptr;
    viewer_img_ = nullptr;
    return true;
}

void ScreenCaptureApp::showScreen(Screen s)
{
    active_screen_ = s;

    lv_obj_add_flag(main_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(viewer_panel_, LV_OBJ_FLAG_HIDDEN);

    if (s == SCREEN_MAIN) {
        lv_obj_clear_flag(main_panel_, LV_OBJ_FLAG_HIDDEN);
    } else if (s == SCREEN_VIEWER) {
        lv_obj_clear_flag(viewer_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ScreenCaptureApp::openViewer(const char *path)
{
    if (viewer_img_ == nullptr || path == nullptr) {
        return;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        ESP_LOGW(TAG, "bad size for %s", path);
        return;
    }
    uint8_t *png = (uint8_t *)malloc((size_t)sz);
    if (png == nullptr) {
        fclose(f);
        return;
    }
    if (fread(png, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(png);
        return;
    }
    fclose(f);

    int w = 0, h = 0;
    uint8_t *rgb565 = nullptr;
    bool ok = screen_capture_png_to_rgb565(png, (size_t)sz, &w, &h, &rgb565);
    free(png);

    if (!ok || rgb565 == nullptr) {
        ESP_LOGW(TAG, "decode failed for %s", path);
        return;
    }

    free(viewer_data_);
    viewer_data_ = rgb565;

    viewer_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    viewer_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    viewer_dsc_.header.w = (uint16_t)w;
    viewer_dsc_.header.h = (uint16_t)h;
    viewer_dsc_.header.stride = (uint16_t)(w * 2);
    viewer_dsc_.data_size = (uint32_t)(w * h * 2);
    viewer_dsc_.data = rgb565;

    lv_image_set_src(viewer_img_, &viewer_dsc_);
    lv_obj_set_size(viewer_img_, w, h);
    lv_obj_center(viewer_img_);

    showScreen(SCREEN_VIEWER);
}

void ScreenCaptureApp::closeViewer(void)
{
    free(viewer_data_);
    viewer_data_ = nullptr;
    viewer_dsc_.data = nullptr;
    showScreen(SCREEN_MAIN);
}

void ScreenCaptureApp::refresh(void)
{
    if (status_label_ != nullptr && screen_capture_is_recording()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Recording... %d frames", screen_capture_recorded_frames());
        lv_label_set_text(status_label_, buf);
    }
    if (rec_label_ != nullptr) {
        lv_label_set_text(rec_label_, screen_capture_is_recording() ? "Stop Recording" : "Start Recording");
    }
}

void ScreenCaptureApp::onEvent(lv_event_t *e)
{
    ScreenCaptureApp *app = g_app;
    if (app == nullptr) {
        return;
    }
    const int act = (int)(intptr_t)lv_event_get_user_data(e);

    if (act == ACT_SHOT) {
        if (screen_capture_screenshot_delayed(5000) == ESP_OK) {
            lv_label_set_text(app->status_label_, "Capturing in 5 s — switch to your screen.");
        } else {
            lv_label_set_text(app->status_label_, "Failed to schedule screenshot.");
        }
    } else if (act == ACT_REC) {
        if (screen_capture_is_recording()) {
            screen_capture_record_stop();
            lv_label_set_text(app->status_label_, "Recording stopped.");
        } else {
            esp_err_t err = screen_capture_record_start();
            lv_label_set_text(app->status_label_, (err == ESP_OK)
                ? "Recording — switch to another app."
                : "Failed to start recording (SD card?).");
        }
    } else if (act == ACT_VIEW) {
        /* The shared picker browses the whole card, starting where captures land. */
        if (!file_picker::open("/sdcard/shots", ".png",
                               ScreenCaptureApp::onFilePicked,
                               ScreenCaptureApp::onFilePickCancelled, app)) {
            lv_label_set_text(app->status_label_, "Cannot open the file picker.");
        }
    } else if (act == ACT_BACK) {
        if (app->active_screen_ == SCREEN_VIEWER) {
            app->closeViewer();
        }
    }
    app->refresh();
}

void ScreenCaptureApp::onFilePicked(const char *path, void *user)
{
    ScreenCaptureApp *app = (ScreenCaptureApp *)user;
    if (app != nullptr && path != nullptr) {
        app->openViewer(path);
    }
}

void ScreenCaptureApp::onFilePickCancelled(const char *path, void *user)
{
    (void)path;
    (void)user;
    /* Nothing to undo: the picker covered the app, which is still where it was. */
}

void ScreenCaptureApp::onTick(lv_timer_t *t)
{
    ScreenCaptureApp *app = (ScreenCaptureApp *)lv_timer_get_user_data(t);
    if (app != nullptr) {
        app->refresh();
    }
}
