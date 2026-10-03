/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — capture engine (co-processor side).
 *
 * Owns the receiving side: a promiscuous RX callback that runs
 * wifi_toolbox_classify() on every frame and queues the interesting ones for a
 * sender task to push up the custom-RPC channel as EVT_FRAME records.
 *
 * Why the split between callback and task: the promiscuous callback runs in the
 * Wi-Fi driver's RX context and must stay allocation-free and cheap (spec
 * section 3.3). It classifies and copies into a pre-allocated ring; every
 * blocking or variable-cost operation — the SDIO send — happens in the sender
 * task instead. Doing the send inline would stall the radio's receive path for
 * the duration of a transport round trip and drop frames that the driver had
 * already received.
 *
 * A full ring drops the NEWEST frame and counts it. Dropping the oldest would
 * mean the sender task touching the producer's slot, which needs a lock shared
 * with the RX callback; counting a drop is honest and the counter is reported
 * up, so the host can say "this capture is lossy" instead of silently thinning
 * the file.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "wifi_toolbox_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the frame ring. Must be called before capture_start(). */
esp_err_t wifi_toolbox_capture_init(void);

/* Would a frame of this shape be streamed under `filter_mask`?
 *
 * Split out from the RX callback so it can be exercised directly, and so the
 * boot-time self-check tests the decision the radio path actually makes rather
 * than a copy of it. The callback calls this on every received frame, so it must
 * stay allocation-free and cheap. */
bool wifi_toolbox_capture_matches(const uint8_t *payload, size_t length, uint32_t filter_mask);

/* Open a session described by `req->u.capture`, on `req->channel` or hopping.
 *
 * Requires the slave to be armed for capture: like injection, capture is a radio
 * operation the host asks for, and the same reasoning applies — the host is what
 * the gate guards against. Returns WIFI_TOOLBOX_ERR_NOT_ARMED otherwise, and
 * WIFI_TOOLBOX_ERR_RADIO_BUSY when a session is already running. */
wifi_toolbox_result_t wifi_toolbox_capture_start(const wifi_toolbox_capture_params_t *params);

/* Fire the deauth burst a capture session uses to force a handshake, aimed at the
 * session's target BSSID. Broadcast when no target was set. Requires an armed,
 * running capture session: elicitation is an injection, and spec section 10.2
 * subjects it to the same gate as any other transmission.
 *
 * Returns WIFI_TOOLBOX_ERR_NOT_ARMED when the gate is shut, WIFI_TOOLBOX_ERR_BAD_PARAM
 * when no session is running. */
wifi_toolbox_result_t wifi_toolbox_capture_elicit(const uint8_t bssid[6]);

/* End the session and wait for it to be over, so "stop returned" means the radio
 * is no longer in promiscuous mode and no frame can still be sent up. Safe to
 * call when idle. */
void wifi_toolbox_capture_stop(void);

/* Snapshot counters and state. Safe to call at any time. */
void wifi_toolbox_capture_get_stats(wifi_toolbox_capture_stats_t *out);

bool wifi_toolbox_capture_is_running(void);

#ifdef __cplusplus
}
#endif
