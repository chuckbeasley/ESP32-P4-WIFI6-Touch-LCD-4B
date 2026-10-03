/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — co-processor initialisation.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wifi_toolbox_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the toolbox's custom-RPC handlers on esp-hosted's peer-data channel.
 * Requires CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA=y; returns that feature's error if
 * it is missing rather than silently doing nothing. Idempotent. */
esp_err_t wifi_toolbox_rpc_init(void);

/* Record whether the libnet80211 raw-frame sanity check has been relaxed on this
 * image. Reported to the host so the UI can refuse the deauth/disassoc modes
 * instead of letting them fail at Start (without the patch the driver drops them
 * with "unsupport frame type: 0c0" while beacon/probe still transmit). */
void wifi_toolbox_set_raw_frame_patch(bool patched);

/* ---- Injection engine --------------------------------------------------- */

/* Create the session mutex. Must be called before any other injection call. This
 * also brings up the shared radio lock, so the capture engine does not need a
 * separate ordering rule. */
esp_err_t wifi_toolbox_inject_init(void);

/* Open the arming gate (spec 10.2). Refuses when `acknowledged` is false, so the
 * caller has to affirm it is authorised. RAM-only; cleared when a session ends
 * and on disarm. */
esp_err_t wifi_toolbox_arm(bool acknowledged, wifi_toolbox_arm_mode_t mode);

/* Close the gate and stop anything running. */
void wifi_toolbox_disarm(void);

bool wifi_toolbox_is_armed(wifi_toolbox_arm_mode_t mode);

/* Start a session. Returns WIFI_TOOLBOX_ERR_NOT_ARMED if the gate is shut and
 * WIFI_TOOLBOX_ERR_RADIO_BUSY if one is already running. The duration is clamped
 * here, not trusted from the caller. */
wifi_toolbox_result_t wifi_toolbox_inject_start(const wifi_toolbox_inject_params_t *req);

/* Ask the running session to stop and wait for the task to exit, so "stop
 * returned" means "not transmitting". No-op when idle. */
void wifi_toolbox_inject_stop(void);

/* Snapshot counters and state. Safe to call at any time. */
void wifi_toolbox_inject_get_stats(wifi_toolbox_stats_t *out);

/* True while a session task exists. Used by the capture engine to refuse a
 * second session on the one radio rather than letting two sessions fight over
 * the channel. */
bool wifi_toolbox_inject_is_running(void);

/* The armed mode, or 0xFF when the gate is shut. For the radio-state report. */
uint8_t wifi_toolbox_armed_mode(void);

#ifdef __cplusplus
}
#endif
