/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen Capture phone app.
 */
#include "ScreenCaptureApp.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <algorithm>
#include <cctype>

#include "esp_log.h"
#include "screen_capture.h"

static const char *TAG = "ScreenCapture";

enum { ACT_SHOT = 1, ACT_REC = 2, ACT_VIEW = 3, ACT_BACK = 4 };

static ScreenCaptureApp *g_app = nullptr;

ScreenCaptureApp::ScreenCaptureApp()
    : ESP_Brookesia_PhoneApp(
          esp_brookesia::systems::base::App::Config::SIMPLE_CONSTRUCTOR("Screen Capture", nullptr, true),
          esp_brookesia::systems::phone::App::Config::SIMPLE_CONSTRUCTOR(nullptr, true, false)),
      status_label_(nullptr),
      rec_label_(nullptr),
      ui_timer_(nullptr),
      main_panel_(nullptr),
      gallery_panel_(nullptr),
      viewer_panel_(nullptr),
      gallery_list_(nullptr),
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

    /* ---- gallery panel: list of screenshots ---- */
    gallery_panel_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(gallery_panel_, w, h);
    lv_obj_align(gallery_panel_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(gallery_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(gallery_panel_, 20, 0);
    lv_obj_set_style_pad_row(gallery_panel_, 12, 0);
    lv_obj_set_style_border_width(gallery_panel_, 0, 0);

    lv_obj_t *g_header = lv_obj_create(gallery_panel_);
    lv_obj_set_width(g_header, LV_PCT(100));
    lv_obj_set_height(g_header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(g_header, LV_OBJ_FLAG_SCROLLABLE);
    make_back_button(g_header);
    make_title(g_header, "Captures");

    gallery_list_ = lv_obj_create(gallery_panel_);
    lv_obj_set_width(gallery_list_, LV_PCT(100));
    lv_obj_set_flex_grow(gallery_list_, 1);
    lv_obj_set_flex_flow(gallery_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(gallery_list_, 0, 0);
    lv_obj_set_style_pad_row(gallery_list_, 8, 0);
    lv_obj_set_scroll_dir(gallery_list_, LV_DIR_VER);

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
    if (active_screen_ == SCREEN_VIEWER) {
        closeViewer();
        return true;
    }
    if (active_screen_ == SCREEN_GALLERY) {
        showScreen(SCREEN_MAIN);
        return true;
    }
    notifyCoreClosed();
    return true;
}

bool ScreenCaptureApp::close(void)
{
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
    gallery_panel_ = nullptr;
    viewer_panel_ = nullptr;
    gallery_list_ = nullptr;
    viewer_img_ = nullptr;
    gallery_files_.clear();
    return true;
}

void ScreenCaptureApp::showScreen(Screen s)
{
    active_screen_ = s;

    lv_obj_add_flag(main_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gallery_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(viewer_panel_, LV_OBJ_FLAG_HIDDEN);

    if (s == SCREEN_MAIN) {
        lv_obj_clear_flag(main_panel_, LV_OBJ_FLAG_HIDDEN);
    } else if (s == SCREEN_GALLERY) {
        rebuildGallery();
        lv_obj_clear_flag(gallery_panel_, LV_OBJ_FLAG_HIDDEN);
    } else if (s == SCREEN_VIEWER) {
        lv_obj_clear_flag(viewer_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool ends_with_png(const char *name)
{
    size_t n = strlen(name);
    if (n < 4) {
        return false;
    }
    const char *ext = name + n - 4;
    return ext[0] == '.' &&
           (ext[1] == 'p' || ext[1] == 'P') &&
           (ext[2] == 'n' || ext[2] == 'N') &&
           (ext[3] == 'g' || ext[3] == 'G');
}

void ScreenCaptureApp::rebuildGallery(void)
{
    if (gallery_list_ == nullptr) {
        return;
    }

    lv_obj_clean(gallery_list_);
    gallery_files_.clear();

    std::vector<std::string> names;
    DIR *d = opendir("/sdcard/shots");
    if (d != nullptr) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            /* FATFS returns uppercase 8.3 names (SHOT0000.PNG) with LFN off, so the
             * extension check must be case-insensitive. */
            if (ends_with_png(e->d_name)) {
                names.push_back(e->d_name);
            }
        }
        closedir(d);
    }

    if (names.empty()) {
        lv_obj_t *lbl = lv_label_create(gallery_list_);
        lv_label_set_text(lbl, "No screenshots yet.");
        return;
    }

    /* Newest first: the 4-digit zero-padded names sort numerically. */
    std::sort(names.rbegin(), names.rend());

    for (std::string name : names) {
        /* FATFS returns uppercase 8.3 names; display them lowercase. */
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        gallery_files_.push_back("/sdcard/shots/" + name);
    }

    for (const std::string &path : gallery_files_) {
        lv_obj_t *btn = lv_btn_create(gallery_list_);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 52);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, path.c_str() + strlen("/sdcard/shots/"));
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, onGalleryFileClick, LV_EVENT_CLICKED, (void *)path.c_str());
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
    showScreen(SCREEN_GALLERY);
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
        app->showScreen(SCREEN_GALLERY);
    } else if (act == ACT_BACK) {
        if (app->active_screen_ == SCREEN_VIEWER) {
            app->closeViewer();
        } else if (app->active_screen_ == SCREEN_GALLERY) {
            app->showScreen(SCREEN_MAIN);
        }
    }
    app->refresh();
}

void ScreenCaptureApp::onGalleryFileClick(lv_event_t *e)
{
    const char *path = (const char *)lv_event_get_user_data(e);
    ScreenCaptureApp *app = g_app;
    if (app != nullptr && path != nullptr) {
        app->openViewer(path);
    }
}

void ScreenCaptureApp::onTick(lv_timer_t *t)
{
    ScreenCaptureApp *app = (ScreenCaptureApp *)lv_timer_get_user_data(t);
    if (app != nullptr) {
        app->refresh();
    }
}
