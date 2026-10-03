/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — host side.
 *
 * The single owner of the custom-RPC channel to the co-processor's toolbox. The
 * UI, the PCAP writer and the net utilities go through this; nothing else touches
 * the channel, and nothing here calls esp_wifi_* for radio work, because on this
 * target the radio is on the other chip (see M0-FINDINGS.md — the host's
 * esp_wifi_remote entry points for raw TX and promiscuous mode are weak
 * UNSUPPORTED stubs).
 *
 * What this owns and why:
 *
 *  - **One lease.** Sessions take the radio through `arm()`/`disarm()` here, so a
 *    second requester asks this module instead of opening its own channel (spec
 *    section 5.1).
 *  - **The arming gate's host half.** The gate is *enforced* on the co-processor,
 *    because the host is what it guards against. This side keeps the RAM-only flag
 *    so the UI can render state, and re-sends the authorisation on every arm.
 *  - **Not resuming after a reboot** (spec 10.3): the armed flag here is RAM-only
 *    and is cleared when the co-processor reboots or the link drops, because a
 *    slave that rebooted has no session and must not appear to have one.
 *  - **Power save.** The lease is taken on the co-processor, not here: every
 *    esp_wifi_remote_* call in this tree is a weak UNSUPPORTED stub, so the host
 *    cannot read or restore the value. It goes through CONTROL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wifi_toolbox_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Callbacks ----------------------------------------------------------
 *
 * One subscriber, set before init(). Everything arrives on esp-hosted's RPC task,
 * so a callback must not block and must not call back into this module.
 */
typedef struct {
    /* One filtered frame, already decoded from the wire record. `payload` is
     * valid only for the duration of the call. */
    void (*on_frame)(const wifi_toolbox_frame_evt_t *hdr, const uint8_t *payload, void *ctx);
    /* A status block: pushed at ~1 Hz while a session runs, and after every
     * command that could have changed it. */
    void (*on_status)(const wifi_toolbox_status_t *st, void *ctx);
    /* The link to the co-processor went down or came back. A session that was
     * running is already stopped and the armed flag cleared by the time this
     * fires (spec section 5.4). */
    void (*on_link)(bool up, void *ctx);
    /* Something went wrong, with a code a UI can act on (spec section 9). */
    void (*on_error)(wifi_toolbox_result_t code, const char *message, void *ctx);
} wifi_toolbox_host_callbacks_t;

/* ---- Lifecycle ---------------------------------------------------------- */

esp_err_t wifi_toolbox_host_init(const wifi_toolbox_host_callbacks_t *callbacks, void *ctx);
void      wifi_toolbox_host_deinit(void);

/* Ask the co-processor what it can do. Fills `caps` and returns ESP_OK only when
 * the reply agrees on the RPC version; a mismatch is reported as
 * ESP_ERR_INVALID_VERSION rather than mis-parsed (host and co-processor images are
 * flashed separately, so a stale image on either side is a normal condition). */
esp_err_t wifi_toolbox_host_query_caps(wifi_toolbox_caps_t *caps);

/* Ask for a status block. Returns ESP_OK when the request was sent; the answer
 * arrives through on_status. Reading cached state instead is what `status()` is
 * for. */
esp_err_t wifi_toolbox_host_refresh_status(void);

/* The most recent status block, and whether one has ever arrived. */
bool wifi_toolbox_host_status(wifi_toolbox_status_t *out);
bool wifi_toolbox_host_link_is_up(void);

/* ---- Arming (spec section 10.2) ---------------------------------------- */

/* Open the gate on the co-processor for `mode`. `acknowledged` is passed through
 * and must be true: it is the caller's assertion that it is authorised to test the
 * target network, and the co-processor refuses the arm without it. */
esp_err_t wifi_toolbox_host_arm(wifi_toolbox_arm_mode_t mode, bool acknowledged);

/* Close the gate and stop everything. */
esp_err_t wifi_toolbox_host_disarm(void);

bool    wifi_toolbox_host_is_armed(void);
uint8_t wifi_toolbox_host_armed_mode(void);   /* 0xFF when disarmed */

/* ---- Sessions ----------------------------------------------------------- */

esp_err_t wifi_toolbox_host_capture_start(const wifi_toolbox_capture_params_t *params);
esp_err_t wifi_toolbox_host_inject_start(const wifi_toolbox_inject_params_t *params);
esp_err_t wifi_toolbox_host_stop(void);

/* Send the handshake-forcing deauth burst now. Goes through the same gate as any
 * other transmission (spec section 10.2). */
esp_err_t wifi_toolbox_host_elicit(const uint8_t bssid[6]);

/* ---- CONTROL passthrough ------------------------------------------------ */

esp_err_t wifi_toolbox_host_control(const wifi_toolbox_control_req_t *req,
                                    wifi_toolbox_control_rsp_t *rsp);

/* ---- Helpers ------------------------------------------------------------ */

const char *wifi_toolbox_result_name(wifi_toolbox_result_t code);
const char *wifi_toolbox_state_name(uint8_t state);
const char *wifi_toolbox_session_name(uint8_t session);

/* Parse "aa:bb:cc:dd:ee:ff" (or "aabbccddeeff"). Accepts NULL/empty as "no MAC",
 * which is what the optional fields need. */
bool wifi_toolbox_parse_mac(const char *text, uint8_t out[6]);
void wifi_toolbox_format_mac(const uint8_t mac[6], char out[18]);

#ifdef __cplusplus
}
#endif
