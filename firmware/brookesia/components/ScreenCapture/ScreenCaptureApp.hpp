/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screen Capture phone app: device-wide screenshots and screen recording, plus a
 * viewer for the captured screenshots. Browsing the captures is delegated to the
 * shared FilePicker component, so nested directories and any folder on the card work.
 */
#pragma once

#include <string>
#include <vector>

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
    static void onViewerPressed(lv_event_t *e);
    static void onViewerReleased(lv_event_t *e);
    static void onViewerHintTimeout(lv_timer_t *t);

private:
    enum Screen { SCREEN_MAIN = 0, SCREEN_VIEWER = 1 };

    void refresh(void);
    void showScreen(Screen s);
    void openViewer(const char *path);
    void closeViewer(void);

    /* The viewer walks the folder the picked file came from, so a swipe means "the next
     * capture", not "reopen the picker". */
    void buildViewerList(const char *picked);
    void showViewerIndex(void);
    void stepViewer(int delta);
    void showViewerHint(const char *text);

    lv_obj_t  *status_label_;
    lv_obj_t  *rec_label_;
    lv_timer_t *ui_timer_;

    lv_obj_t  *main_panel_;
    lv_obj_t  *viewer_panel_;
    lv_obj_t  *viewer_img_;
    lv_obj_t  *viewer_hint_;
    lv_timer_t *viewer_hint_timer_;

    uint8_t   *viewer_data_;
    lv_image_dsc_t viewer_dsc_;
    Screen    active_screen_;

    std::vector<std::string> viewer_list_;
    size_t    viewer_index_;
    int       viewer_press_x_;
    bool      viewer_press_valid_;
};
