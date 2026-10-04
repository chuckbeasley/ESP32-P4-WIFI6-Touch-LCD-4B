/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen Capture phone app: device-wide screenshots and screen recording, plus a
 * gallery that lists and displays the captured screenshots.
 */
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

#include <string>
#include <vector>

class ScreenCaptureApp : public ESP_Brookesia_PhoneApp
{
public:
    ScreenCaptureApp();
    ~ScreenCaptureApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    /* Static callbacks are public so the free helper functions that build the
     * panels can reference onEvent. */
    static void onEvent(lv_event_t *e);
    static void onGalleryFileClick(lv_event_t *e);
    static void onTick(lv_timer_t *t);

private:
    enum Screen { SCREEN_MAIN = 0, SCREEN_GALLERY = 1, SCREEN_VIEWER = 2 };

    void refresh(void);
    void showScreen(Screen s);
    void rebuildGallery(void);
    void openViewer(const char *path);
    void closeViewer(void);

    lv_obj_t  *status_label_;
    lv_obj_t  *rec_label_;
    lv_timer_t *ui_timer_;

    lv_obj_t  *main_panel_;
    lv_obj_t  *gallery_panel_;
    lv_obj_t  *viewer_panel_;
    lv_obj_t  *gallery_list_;
    lv_obj_t  *viewer_img_;

    std::vector<std::string> gallery_files_;
    uint8_t   *viewer_data_;
    lv_image_dsc_t viewer_dsc_;
    Screen    active_screen_;
};
