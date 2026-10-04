/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device-wide screen capture: a PNG screenshot of whatever is on screen, and a
 * background MJPEG (AVI) screen recording. Both survive app close because the work
 * runs on FreeRTOS tasks rather than the app's LVGL timers.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the SD card (idempotent), create the capture directories, and start the
 * hardware JPEG encoder. Call once, e.g. from an app's run(). */
esp_err_t screen_capture_init(void);

/* Take an immediate screenshot of the current screen as a PNG and save it under
 * /sdcard/shots/. */
esp_err_t screen_capture_screenshot(void);

/* Schedule a screenshot after `delay_ms`. This runs on its own task, so it fires
 * even after the app that scheduled it is closed — which is what lets a user tap
 * Screenshot and then switch to the app they actually want captured. */
esp_err_t screen_capture_screenshot_delayed(uint32_t delay_ms);

/* Start / stop a background MJPEG (AVI) recording of the screen. Recording also
 * continues after the scheduling app is closed. */
esp_err_t screen_capture_record_start(void);
esp_err_t screen_capture_record_stop(void);

bool screen_capture_is_recording(void);
int  screen_capture_recorded_frames(void);

/* Decode a PNG written by screen_capture_screenshot (8-bit truecolour, stored
 * deflate) into an RGB565 buffer. The caller frees *out_rgb565. Returns true on
 * success and sets *out_w / *out_h. */
bool screen_capture_png_to_rgb565(const uint8_t *png, size_t png_len,
                                  int *out_w, int *out_h, uint8_t **out_rgb565);

#ifdef __cplusplus
}
#endif
