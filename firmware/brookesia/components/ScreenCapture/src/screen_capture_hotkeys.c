/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Three-finger capture gestures. See the header for the why; this file is the how.
 */
#include "screen_capture_hotkeys.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_input.h"
#include "screen_capture.h"

static const char *TAG = "ScreenCaptureKeys";

/* Three fingers, held this long for the long press, and allowed to drift this far in raw
 * panel units before the gesture stops counting as a tap. */
#define HOTKEY_FINGERS        3
#define HOTKEY_LONG_PRESS_US  (700 * 1000)
#define HOTKEY_MOVE_TOL_PX    48

/* What the GT911 can report; the adapter's own buffer may be smaller in single-pointer
 * mode, which is exactly why we read into ours. */
#define HOTKEY_MAX_POINTS     5

typedef enum {
    HOTKEY_SHOT = 0,
    HOTKEY_RECORD_TOGGLE,
} hotkey_action_t;

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;
static lv_indev_t   *s_indev;
static bool          s_started;

static struct {
    bool    active;      /* HOTKEY_FINGERS are down */
    bool    moved;       /* they wandered, so this is not a tap */
    bool    long_fired;  /* the long press already ran */
    int64_t start_us;
    float   start_x;
    float   start_y;
} s_gesture;

/* Posted from the LVGL task, inside the input read, so it must never block. */
static void hotkey_post(hotkey_action_t action)
{
    if (s_queue != NULL) {
        (void)xQueueSend(s_queue, &action, 0);
    }
}

/* The capture API takes the LVGL lock itself, so it runs here, on an ordinary task, in
 * the same context the app's own delayed screenshot already uses. */
static void hotkey_task(void *arg)
{
    (void)arg;

    for (;;) {
        hotkey_action_t action;
        if (xQueueReceive(s_queue, &action, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (action == HOTKEY_SHOT) {
            ESP_LOGI(TAG, "three-finger tap: screenshot");
            (void)screen_capture_screenshot();
        } else if (screen_capture_is_recording()) {
            ESP_LOGI(TAG, "three-finger hold: stop recording");
            (void)screen_capture_record_stop();
        } else {
            ESP_LOGI(TAG, "three-finger hold: start recording");
            (void)screen_capture_record_start();
        }
    }
}

static void hotkey_feed(const esp_lcd_touch_point_data_t *pts, uint8_t count)
{
    const int64_t now = esp_timer_get_time();

    if (count >= HOTKEY_FINGERS) {
        const float x = ((float)pts[0].x + (float)pts[1].x + (float)pts[2].x) / 3.0f;
        const float y = ((float)pts[0].y + (float)pts[1].y + (float)pts[2].y) / 3.0f;

        if (!s_gesture.active) {
            s_gesture.active = true;
            s_gesture.moved = false;
            s_gesture.long_fired = false;
            s_gesture.start_us = now;
            s_gesture.start_x = x;
            s_gesture.start_y = y;
            return;
        }

        const float dx = x - s_gesture.start_x;
        const float dy = y - s_gesture.start_y;
        if (((dx * dx) + (dy * dy)) > (float)(HOTKEY_MOVE_TOL_PX * HOTKEY_MOVE_TOL_PX)) {
            s_gesture.moved = true;
        }

        /* Fire the long press while the fingers are still down, so the white flash (or
         * the recording start) confirms it before the user lifts off. */
        if (!s_gesture.moved && !s_gesture.long_fired &&
            ((now - s_gesture.start_us) >= HOTKEY_LONG_PRESS_US)) {
            s_gesture.long_fired = true;
            hotkey_post(HOTKEY_RECORD_TOGGLE);
        }
        return;
    }

    if (!s_gesture.active) {
        return;
    }

    const bool was_tap = !s_gesture.moved && !s_gesture.long_fired &&
                         ((now - s_gesture.start_us) < HOTKEY_LONG_PRESS_US);
    s_gesture.active = false;

    if (was_tap) {
        hotkey_post(HOTKEY_SHOT);
    }
}

/* Replaces the adapter's esp_lcd_touch_read_data + esp_lcd_touch_get_data pair. The
 * adapter's buffer is sized to what it expects to use — one point in single-pointer
 * mode — so read the controller's full report into ours and then hand back only what
 * was asked for. The adapter's scaling and LVGL fill then proceed unchanged. */
static esp_err_t hotkey_touch_read(esp_lcd_touch_handle_t tp,
                                   esp_lcd_touch_point_data_t *points, uint8_t *count,
                                   uint8_t max_count, void *user_ctx)
{
    (void)user_ctx;

    esp_lcd_touch_point_data_t all[HOTKEY_MAX_POINTS];
    uint8_t n = 0;

    esp_lcd_touch_read_data(tp);
    const esp_err_t err = esp_lcd_touch_get_data(tp, all, &n, HOTKEY_MAX_POINTS);
    if (err != ESP_OK) {
        *count = 0;
        return err;
    }

    hotkey_feed(all, n);

    const uint8_t give = (n < max_count) ? n : max_count;
    for (uint8_t i = 0; i < give; i++) {
        points[i] = all[i];
    }
    *count = give;
    return ESP_OK;
}

esp_err_t screen_capture_hotkeys_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* The adapter registers the touch device itself, so find the pointer indev it made
     * rather than creating a second handle on the same controller. */
    lv_indev_t *indev = NULL;
    for (lv_indev_t *i = lv_indev_get_next(NULL); i != NULL; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
            indev = i;
            break;
        }
    }
    if (indev == NULL) {
        ESP_LOGW(TAG, "no pointer input device yet, gestures not installed");
        return ESP_ERR_NOT_FOUND;
    }

    s_queue = xQueueCreate(4, sizeof(hotkey_action_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Same stack as the app's own screenshot worker: the encode happens on this task. */
    if (xTaskCreate(hotkey_task, "cap_hotkey", 8192, NULL, 4, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_lv_adapter_touch_callbacks_t cbs = {0};
    cbs.custom_touch_read = hotkey_touch_read;

    const esp_err_t err = esp_lv_adapter_set_touch_callbacks(indev, &cbs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_touch_callbacks: %s", esp_err_to_name(err));
        vTaskDelete(s_task);
        s_task = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return err;
    }

    s_indev = indev;
    s_started = true;
    ESP_LOGI(TAG, "three-finger tap = screenshot, three-finger hold = record toggle");
    return ESP_OK;
}

void screen_capture_hotkeys_stop(void)
{
    if (!s_started) {
        return;
    }

    if (s_indev != NULL) {
        (void)esp_lv_adapter_set_touch_callbacks(s_indev, NULL);
        s_indev = NULL;
    }
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    memset(&s_gesture, 0, sizeof(s_gesture));
    s_started = false;
}
