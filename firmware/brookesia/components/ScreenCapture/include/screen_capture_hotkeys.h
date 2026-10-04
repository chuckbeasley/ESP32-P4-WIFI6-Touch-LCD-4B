/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Capture without leaving the app you are in: three fingers tap for a screenshot, three
 * fingers held for a moment to start or stop recording.
 *
 * The gesture is observed through the LVGL adapter's `custom_touch_read` hook, which
 * replaces the adapter's own hardware read. This hook reads every point the controller
 * reports into its own buffer, feeds the recogniser, and then hands the adapter exactly
 * what it asked for — so the app's own input is bit-for-bit what it would have been,
 * and nothing is consumed or swallowed.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the gesture observer. Idempotent, and safe to call once the LVGL touch device
 * is registered — i.e. any time after the adapter has started. Returns
 * ESP_ERR_NOT_FOUND if no pointer input device exists yet. */
esp_err_t screen_capture_hotkeys_start(void);

/* Remove the observer and stop the worker. The captures themselves keep whatever they
 * were doing; this only stops new gestures from being recognised. */
void screen_capture_hotkeys_stop(void);

#ifdef __cplusplus
}
#endif
