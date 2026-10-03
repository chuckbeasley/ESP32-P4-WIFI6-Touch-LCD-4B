/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — phone app.
 *
 * Screens as an esp-brookesia 0.4.2 phone app. Note the deliberate scope
 * difference from BrookesiaSpec section 6: that section assumes Brookesia v0.8,
 * where screens are JSON UI documents resolved by a service framework this tree
 * does not have (see M0-FINDINGS.md). The screens here are the same screens built
 * in the 0.4.2 idiom, and the two safety-relevant additions the spec calls for are
 * implemented rather than deferred:
 *
 *  - an **arming dialog** that names the mode, the channel, the duration and the
 *    transmitting chip before anything is transmitted (spec section 10.2), and
 *  - a **persistent in-app banner** for as long as a session is live, so a screen
 *    the user navigated away from cannot hide an active transmitter (section 10.4).
 */
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class ToolboxApp : public ESP_Brookesia_PhoneApp
{
public:
    ToolboxApp();
    ~ToolboxApp() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    /* Diagnostics for the startup check. Public because the check lives in main.cpp:
     *
     *  - logScreenStructure() reports, per screen, whether the screen's panel is a
     *    child of its container and whether the container is hidden. Switching works
     *    by hiding the containers, so a panel parented anywhere else is never
     *    switched — it stays on top of everything, which is what made a sub-screen's
     *    back button appear to lead to a blank screen.
     *  - showScreenForTest() switches screens without a tap. 0 menu, 1 capture,
     *    2 inject, 3 network. */
    void logScreenStructure(void);
    void showScreenForTest(int index);

    /* Clicks each menu entry by sending a real LV_EVENT_CLICKED, so the handler that
     * a finger reaches is the handler under test. Reports which screen each lands
     * on. Part of the same startup check. */
    void runNavigationSelfTest(void);

    /* Reports, per screen, how far the content overflows its panel: the height of
     * the panel, of the panel's content, and the scrollable distance below the last
     * child. A positive scroll_bottom means content is being clipped, which is the
     * condition a scrollbar exists to fix. */
    void logScrollState(void);

private:
    /* Screens are panels inside the one default screen rather than separate LVGL
     * screens, so the core's visual-area and teardown handling applies once. */
    enum Screen {
        SCREEN_MENU = 0,
        SCREEN_CAPTURE,
        SCREEN_INJECT,
        SCREEN_NETWORK,
        SCREEN_COUNT,
    };

    void buildMenu(void);
    void buildCapture(void);
    void buildInject(void);
    void buildNetwork(void);
    void showScreen(Screen screen);

    /* Bring the host module up, once, on first open. See the note in init(). */
    bool ensureHost(void);

    /* The arming dialog: a modal message box whose text is generated from what is
     * about to be transmitted. Returns through a callback, so the caller does not
     * block while it is up. */
    void askArm(bool for_injection, uint8_t channel, uint32_t duration_s, const char *mode_name);
    void refreshFromStatus(void);
    void updateBanner(void);
    void logLine(const char *text);

    static void onEvent(lv_event_t *e);
    static void onArmConfirmed(lv_event_t *e);
    static void onArmCancelled(lv_event_t *e);

    /* Set by the toolbox callbacks, consumed by the LVGL timer. The callbacks run
     * on esp-hosted's RPC task, so they must not touch LVGL: they stash state and
     * this app's timer renders it. */
    static void statusTick(lv_timer_t *timer);

    lv_obj_t *screens[SCREEN_COUNT];

    /* The arming dialog. It is created on lv_layer_top(), NOT on the app's default
     * screen, so the core's cleanDefaultScreen() does not reach it: if the app is
     * closed with the dialog open, this pointer is the only handle on it and the
     * dialog would sit over the launcher for good. close() deletes it explicitly. */
    lv_obj_t *arm_modal;
    Screen    active_screen;

    /* Banner and the fields it shows. */
    lv_obj_t  *banner;
    lv_obj_t  *banner_label;

    /* Capture screen. */
    lv_obj_t  *cap_state_label;
    lv_obj_t  *cap_counters_label;
    lv_obj_t  *cap_path_label;
    lv_obj_t  *cap_channel_dd;
    lv_obj_t  *cap_hop_switch;
    lv_obj_t  *cap_elicit_switch;
    lv_obj_t  *cap_log;

    /* Inject screen. */
    lv_obj_t  *inj_mode_dd;
    lv_obj_t  *inj_ssid_ta;
    lv_obj_t  *inj_bssid_ta;
    lv_obj_t  *inj_client_ta;
    lv_obj_t  *inj_channel_dd;
    lv_obj_t  *inj_duration_slider;
    lv_obj_t  *inj_duration_label;
    lv_obj_t  *inj_counters_label;
    lv_obj_t  *inj_log;

    /* Network screen. */
    lv_obj_t  *net_mode_dd;
    lv_obj_t  *net_preset_dd;
    lv_obj_t  *net_target_ta;
    lv_obj_t  *net_results;
    lv_obj_t  *net_stats_label;

    lv_timer_t *ui_timer;

    /* Latched for the arming dialog's confirm path. */
    bool      arm_pending_inject;
    uint8_t   arm_pending_channel;
    uint32_t  arm_pending_duration_s;
    char      arm_pending_mode[24];
};
