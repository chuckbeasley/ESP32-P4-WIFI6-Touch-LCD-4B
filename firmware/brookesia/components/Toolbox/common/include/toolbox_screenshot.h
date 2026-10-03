/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screenshot web server — a development aid. See the .c for why it exists and why it is
 * not a product feature.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server and take one capture, so the first request returns an image rather
 * than a 404. Returns ESP_ERR_NOT_SUPPORTED when CONFIG_TOOLBOX_SCREENSHOT_SERVER is off —
 * in which case nothing was compiled in, not merely switched off. */
esp_err_t toolbox_screenshot_start(void);

/* Take a fresh capture on demand, for a caller that wants one at a known moment rather than
 * whenever a browser asks. */
void toolbox_screenshot_capture(void);

#ifdef __cplusplus
}
#endif
