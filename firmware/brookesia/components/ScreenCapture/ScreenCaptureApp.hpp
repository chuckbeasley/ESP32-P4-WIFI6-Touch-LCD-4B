/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen Capture phone app: device-wide screenshots and screen recording, plus a
 * viewer for the captured screenshots. Browsing the captures is delegated to the
 * shared FilePicker component, so nested directories and any folder on the card work.
 */
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class ScreenCaptureApp : public ESP_Brookesia_PhoneApp
{
public:
    ScreenCaptureApp();
    ~ScreenCaptureApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    /* Public because LVGL and the file picker take plain function pointers. */
    static void onEvent(lv_event_t *e);
    static void onTick(lv_timer_t *t);
    static void onFilePicked(const char *path, void *user);
    static void onFilePickCancelled(const char *path, void *user);

private:
    enum Screen { SCREEN_MAIN = 0, SCREEN_VIEWER = 1 };

    void refresh(void);
    void showScreen(Screen s);
    void openViewer(const char *path);
    void closeViewer(void);

    lv_obj_t  *status_label_;
    lv_obj_t  *rec_label_;
    lv_timer_t *ui_timer_;

    lv_obj_t  *main_panel_;
    lv_obj_t  *viewer_panel_;
    lv_obj_t  *viewer_img_;

    uint8_t   *viewer_data_;
    lv_image_dsc_t viewer_dsc_;
    Screen    active_screen_;
};
