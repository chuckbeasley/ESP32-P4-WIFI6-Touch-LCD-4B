#include "TimeZonePage.hpp"
#include "SettingsUI.hpp"
#include "nvs.h"
#include "esp_log.h"
#include "../Settings.hpp"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <mutex>

#define ESP_UTILS_LOG_TAG "BS:TimeZonePage"
#include "esp_lib_utils.h"

namespace esp_brookesia::apps
{
    // -------------------------------------------------------------------------
    // Time zone table + persistence
    // -------------------------------------------------------------------------

    // POSIX TZ offset: the value added to LOCAL time to obtain UTC (positive is
    // west of Greenwich, negative is east). So "CST-8" = local is UTC+8.
    //
    // Zones that observe DST carry the full "std offset dst,start,end" form so
    // the offset tracks the season (e.g. "EST5EDT,M3.2.0,M11.1.0" = second
    // Sunday of March through first Sunday of November in daylight time).
    static const TimeZoneEntry k_timezones[] = {
        { "UTC (GMT+0)",             "UTC0" },
        { "China (UTC+8)",           "CST-8" },
        { "Japan (UTC+9)",           "JST-9" },
        { "India (UTC+5:30)",        "IST-5:30" },
        { "Central Europe (CET/CEST)","CET-1CEST,M3.5.0,M10.5.0" },
        { "UK (GMT/BST)",            "GMT0BST,M3.5.0,M10.5.0" },
        { "US Eastern (EST/EDT)",    "EST5EDT,M3.2.0,M11.1.0" },
        { "US Central (CST/CDT)",    "CST6CDT,M3.2.0,M11.1.0" },
        { "US Mountain (MST/MDT)",   "MST7MDT,M3.2.0,M11.1.0" },
        { "US Pacific (PST/PDT)",    "PST8PDT,M3.2.0,M11.1.0" },
        { "Australia East (AEST/AEDT)","AEST-10AEDT,M10.1.0,M4.1.0" },
    };

    static const int k_default_timezone_index = 1;  // China, matching the prior CST-8 default

    static constexpr const char *kTzNvsNamespace = "storage";
    static constexpr const char *kTzNvsKey = "timezone";

    // setenv()/tzset() mutate process-global state and can run on the SNTP task
    // and the UI task concurrently, so guard them.
    static std::mutex s_tz_mutex;

    size_t timezone_count(void)
    {
        return sizeof(k_timezones) / sizeof(k_timezones[0]);
    }

    const char *timezone_label(size_t index)
    {
        return (index < timezone_count()) ? k_timezones[index].label : "";
    }

    const char *timezone_tz(size_t index)
    {
        return (index < timezone_count()) ? k_timezones[index].tz : "UTC0";
    }

    static int clamp_index(int index)
    {
        if (index < 0 || index >= (int)timezone_count()) {
            return k_default_timezone_index;
        }
        return index;
    }

    int timezone_load(void)
    {
        int32_t index = k_default_timezone_index;
        nvs_handle_t handle;
        if (nvs_open(kTzNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
            if (nvs_get_i32(handle, kTzNvsKey, &index) != ESP_OK) {
                index = k_default_timezone_index;
            }
            nvs_close(handle);
        }
        return clamp_index((int)index);
    }

    bool timezone_apply(int index)
    {
        index = clamp_index(index);
        std::lock_guard<std::mutex> lock(s_tz_mutex);
        setenv("TZ", timezone_tz(index), 1);
        tzset();
        return true;
    }

    bool timezone_set(int index)
    {
        index = clamp_index(index);
        if (!timezone_apply(index)) {
            return false;
        }

        nvs_handle_t handle;
        if (nvs_open(kTzNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_i32(handle, kTzNvsKey, (int32_t)index);
            nvs_commit(handle);
            nvs_close(handle);
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // Time zone page UI
    // -------------------------------------------------------------------------

    TimeZonePage *TimeZonePage::_instance = nullptr;

    TimeZonePage *TimeZonePage::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new TimeZonePage(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    TimeZonePage::TimeZonePage(bool use_status_bar, bool use_navigation_bar)
        : App("Time Zone", nullptr, true, use_status_bar, use_navigation_bar),
          page_root(nullptr), list1(nullptr)
    {
    }

    TimeZonePage::~TimeZonePage()
    {
    }

    bool TimeZonePage::run()
    {
        ESP_UTILS_LOGD("TimeZonePage Run");
        lv_obj_clean(lv_scr_act());

        page_root = settings_ui::create_page(lv_scr_act());
        settings_ui::create_header(page_root, "Time Zone", [](lv_event_t *e) {
            (void)e;
            lv_async_call([](void *param) {
                (void)param;
                Settings::requestInstance()->showRootPage();
            }, nullptr);
        });

        settings_ui::init_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        list1 = settings_ui::create_content_list(page_root);
        lv_obj_add_style(list1, &style_list, LV_PART_MAIN);

        settings_ui::add_section(list1, "Select time zone", style_list_text);

        const int selected = timezone_load();
        const size_t count = timezone_count();
        for (size_t i = 0; i < count; ++i) {
            char text[96];
            if ((int)i == selected) {
                snprintf(text, sizeof(text), "%s  %s", timezone_label(i), LV_SYMBOL_OK);
            } else {
                snprintf(text, sizeof(text), "%s", timezone_label(i));
            }

            lv_obj_t *btn = lv_list_add_button(list1, nullptr, text);
            lv_obj_add_style(btn, &style_list_btn, LV_PART_MAIN);
            lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
            settings_ui::use_ellipsis_for_button_label(btn);
            lv_obj_add_event_cb(btn, event_handler_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        return true;
    }

    void TimeZonePage::event_handler_cb(lv_event_t *e)
    {
        const int index = (int)(intptr_t)lv_event_get_user_data(e);

        timezone_set(index);
        ESP_UTILS_LOGI("Time zone set to %s (%s)", timezone_label(index), timezone_tz(index));

        // Defer the rebuild so we do not delete widgets from inside their own event.
        lv_async_call([](void *param) {
            (void)param;
            if (TimeZonePage::_instance != nullptr) {
                TimeZonePage::_instance->run();
            }
        }, nullptr);
    }

    bool TimeZonePage::back()
    {
        ESP_UTILS_LOGD("TimeZonePage Back");
        Settings::requestInstance()->showRootPage();
        return true;
    }

    bool TimeZonePage::close()
    {
        ESP_UTILS_LOGD("TimeZonePage Close");
        if (page_root != nullptr)
        {
            lv_obj_del(page_root);
            page_root = nullptr;
            list1 = nullptr;
            settings_ui::reset_list_styles(style_list, style_list_btn, style_list_text, style_list_btn_pressed);
        }
        return true;
    }

} // namespace esp_brookesia::apps
