#pragma once

#include "esp_brookesia.hpp"
#include "lvgl.h"

namespace esp_brookesia::apps {

// ---------------------------------------------------------------------------
// Time zone data + persistence, shared between the Settings app (which applies
// the zone when NTP syncs) and the TimeZonePage (which lets the user pick one).
// ---------------------------------------------------------------------------

struct TimeZoneEntry {
    const char *label;   // Human-readable name shown in the UI.
    const char *tz;      // POSIX TZ string (fixed offset, no DST rule).
};

size_t     timezone_count(void);
const char *timezone_label(size_t index);
const char *timezone_tz(size_t index);

// Persisted index (defaults to the first entry when unset/invalid).
int  timezone_load(void);
// setenv("TZ", ...) + tzset() so localtime()/localtime_r() reflect this zone.
bool timezone_apply(int index);
// apply() plus persist to NVS.
bool timezone_set(int index);

class TimeZonePage : public systems::phone::App {
public:
    static TimeZonePage *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);

    TimeZonePage(bool use_status_bar, bool use_navigation_bar);
    virtual ~TimeZonePage();

    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void event_handler_cb(lv_event_t *e);

    static TimeZonePage *_instance;
    lv_obj_t *page_root;
    lv_obj_t *list1;
    lv_style_t style_list;
    lv_style_t style_list_btn;
    lv_style_t style_list_text;
    lv_style_t style_list_btn_pressed;
};

} // namespace esp_brookesia::apps
