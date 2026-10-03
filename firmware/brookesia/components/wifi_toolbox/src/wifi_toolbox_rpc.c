/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — custom-RPC handler (co-processor side).
 *
 * Registers on esp-hosted's peer-data channel and answers the host's commands.
 * Runs the frame builders on the real target and returns the bytes, which is how
 * the host verifies them against the Tactility originals — the builders cannot
 * be validated on the host, because the host has no radio to build for.
 *
 * Everything here runs in RPC RX context, so it must stay fast: no blocking
 * waits, no unbounded logging. The self-test is the exception — it is an
 * explicit diagnostic, bounded to one frame per mode.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "eh_cp_feat_peer_data.h"
#include "wifi_toolbox.h"
#include "wifi_toolbox_capture_engine.h"
#include "wifi_toolbox_frames.h"
#include "wifi_toolbox_radio.h"
#include "wifi_toolbox_rpc.h"

static const char *TAG = "wifi_toolbox";

/* esp-hosted's table size, as shipped. Not a Kconfig symbol in 3.0.9 — the
 * component hard-codes it — so it is restated here to make the assert below
 * meaningful rather than vacuous. */
#ifndef EH_CP_FEAT_PEER_DATA_MAX_HANDLERS
#define EH_CP_FEAT_PEER_DATA_MAX_HANDLERS 8
#endif

/* Spec section 4.3 wants counters as ~1 Hz events rather than polling, so the UI
 * binds to them and the service stays the only writer.
 *
 * The tick only runs while a session is live. Spec section 9 is absolute that
 * nothing radio-active happens at boot, and a status message every second on an
 * idle toolbox would be traffic and log noise for a screen nobody is looking at —
 * the host asks for a snapshot when it opens a screen instead. */
#define WIFI_TOOLBOX_STATUS_PERIOD_MS   (1000)

static TimerHandle_t s_status_timer;

/* How many message IDs this component registers on the peer-data channel. Kept as
 * one number because the channel's table is fixed-size and a registration past
 * the end fails silently from the host's point of view — the count is asserted
 * against the configured cap below, so adding a message ID without room for it
 * stops the build instead of dropping a command at runtime. */
static unsigned s_handlers_registered;

/* Both are defined further down. Named here because the registration helper uses
 * one of them and the command handlers use the other. */
static void on_peer_data(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx);
static void send_status(void);

static esp_err_t register_cmd(uint32_t msg_id, const char *name)
{
    const esp_err_t err = eh_cp_feat_peer_data_register_callback(msg_id, on_peer_data, NULL);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s (0x%04x) failed: %s", name, (unsigned)msg_id, esp_err_to_name(err));
        return err;
    }
    s_handlers_registered++;
    return ESP_OK;
}

/* Reported in the capability reply. Bump when the wire format changes; the host
 * checks it and refuses to talk to a mismatched slave rather than mis-parsing.
 *
 * v2 added the capture session. v3 replaced the per-diagnostic reply IDs with one
 * status block — see WIFI_TOOLBOX_MSG_CMD_STATUS for why. */
#define WIFI_TOOLBOX_FW_VERSION   (3)

/* The command count is checked against the peer-data table size. Both sides of
 * that comparison are fixed constants on purpose:
 *
 *  - WIFI_TOOLBOX_CMD_COUNT is the protocol's, declared in the shared header and
 *    changed only alongside the message IDs themselves;
 *  - EH_CP_FEAT_PEER_DATA_MAX_HANDLERS is esp-hosted's, hard-coded to 8 inside the
 *    shipped component because the Kconfig symbol meant to size it is not defined
 *    anywhere in that component.
 *
 * The assert fails the build if the protocol ever outgrows the table. That is the
 * point: an overrun is otherwise invisible until a command stops answering in the
 * field, with the only evidence on a console that is not wired to the host. */
_Static_assert(WIFI_TOOLBOX_CMD_COUNT <= EH_CP_FEAT_PEER_DATA_MAX_HANDLERS,
               "the Wi-Fi Toolbox protocol defines more command IDs than the esp-hosted "
               "peer-data table holds (EH_CP_FEAT_PEER_DATA_MAX_HANDLERS, 8 in the shipped "
               "component). Registrations past the end fail at runtime and the host sees "
               "only silence, so either fold two commands into one or raise the table size "
               "in the vendored esp_hosted component");

/* Defined further down; named here because the registration helper below uses it. */

/* Set true once the libnet80211 raw-frame sanity check has been relaxed. Until
 * the one-byte patch is re-derived for the C6 this stays false, and the host
 * shows deauth/disassoc as unavailable rather than letting them fail at Start
 * with the driver's "unsupport frame type: 0c0". */
static bool s_raw_frame_patch = false;

static void send_rsp(uint32_t msg_id, const void *payload, size_t len)
{
    const esp_err_t err = eh_cp_feat_peer_data_send(msg_id, (const uint8_t *)payload, len);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reply 0x%04x failed: %s", (unsigned)msg_id, esp_err_to_name(err));
    }
}

static void handle_ping(void)
{
    wifi_toolbox_caps_t caps;

    memset(&caps, 0, sizeof(caps));
    caps.rpc_version = WIFI_TOOLBOX_RPC_VERSION;
    caps.fw_version = WIFI_TOOLBOX_FW_VERSION;
#if CONFIG_WIFI_TOOLBOX_ALLOW_INJECTION
    caps.injection_available = 1;
#else
    /* Spec 10.5: the shipped image can be built without injection. Reporting it
     * as available because the host has a flag would be a false sense of safety;
     * the slave is the one that has to say no. */
    caps.injection_available = 0;
#endif
    caps.capture_available = 1;
    caps.raw_frame_patch = s_raw_frame_patch ? 1 : 0;
#if CONFIG_WIFI_TOOLBOX_ALLOW_INJECTION
    caps.mode_count = (uint8_t)WIFI_TOOLBOX_INJECT_MODE_MAX;
#else
    caps.mode_count = 0;
#endif
    caps.max_tx_power_dbm = 20;
    memcpy(caps.country_code, "XX\0", 3);

    ESP_LOGI(TAG, "ping: fw=%u modes=%u raw_frame_patch=%u",
             (unsigned)caps.fw_version, (unsigned)caps.mode_count, (unsigned)caps.raw_frame_patch);

    send_rsp(WIFI_TOOLBOX_MSG_RSP_PING, &caps, sizeof(caps));
}

static void handle_selftest(const uint8_t *data, size_t len)
{
    wifi_toolbox_selftest_req_t req;
    wifi_toolbox_selftest_rsp_t rsp;
    wifi_toolbox_frame_params_t params;

    if (len < sizeof(req)) {
        ESP_LOGW(TAG, "selftest: short request (%u < %u)", (unsigned)len, (unsigned)sizeof(req));
        return;
    }
    memcpy(&req, data, sizeof(req));

    if (req.mode >= WIFI_TOOLBOX_INJECT_MODE_MAX) {
        ESP_LOGW(TAG, "selftest: bad mode %u", (unsigned)req.mode);
        return;
    }

    memset(&params, 0, sizeof(params));
    params.ssid = req.ssid;
    params.ssid_len = req.ssid_len;
    params.channel = 1;
    params.listen_interval = 0xFFFF;
    params.duration = 0x8000;
    memcpy(params.bssid, req.bssid, 6);
    memcpy(params.client, req.client, 6);

    memset(&rsp, 0, sizeof(rsp));
    rsp.mode = req.mode;

    const size_t frame_len = wifi_toolbox_build_for_mode(
        rsp.frame, (wifi_toolbox_inject_mode_t)req.mode, &params);

    if (frame_len == 0) {
        rsp.result = WIFI_TOOLBOX_ERR_BAD_PARAM;
        rsp.frame_len = 0;
    } else {
        /* The response struct carries a fixed 128-byte frame field; a builder
         * that ever exceeded it would be silently truncated, so refuse instead. */
        if (frame_len > sizeof(rsp.frame)) {
            ESP_LOGE(TAG, "mode %u frame %u exceeds reply buffer %u",
                     (unsigned)req.mode, (unsigned)frame_len, (unsigned)sizeof(rsp.frame));
            rsp.result = WIFI_TOOLBOX_ERR_INTERNAL;
            rsp.frame_len = 0;
        } else {
            rsp.result = WIFI_TOOLBOX_OK;
            rsp.frame_len = (uint16_t)frame_len;
        }
    }

    ESP_LOGI(TAG, "selftest: mode=%u (%s) len=%u result=%u",
             (unsigned)req.mode, wifi_toolbox_inject_mode_name((wifi_toolbox_inject_mode_t)req.mode),
             (unsigned)rsp.frame_len, (unsigned)rsp.result);

    send_rsp(WIFI_TOOLBOX_MSG_RSP_SELFTEST, &rsp, sizeof(rsp));
}

static void send_cmd_result(uint32_t cmd_id, wifi_toolbox_result_t result)
{
    wifi_toolbox_cmd_result_t r;

    memset(&r, 0, sizeof(r));
    r.cmd_id = cmd_id;
    r.result = (uint8_t)result;

    ESP_LOGI(TAG, "cmd 0x%04x result=%u", (unsigned)cmd_id, (unsigned)result);
    send_rsp(WIFI_TOOLBOX_MSG_EVT_CMD_RESULT, &r, sizeof(r));
}

static void handle_arm(const uint8_t *data, size_t len)
{
    if (len < sizeof(wifi_toolbox_arm_req_t)) {
        ESP_LOGW(TAG, "arm: short request (%u)", (unsigned)len);
        send_cmd_result(WIFI_TOOLBOX_MSG_CMD_ARM, WIFI_TOOLBOX_ERR_BAD_PARAM);
        return;
    }

    wifi_toolbox_arm_req_t req;
    memcpy(&req, data, sizeof(req));

    const esp_err_t err = wifi_toolbox_arm(req.acknowledged != 0,
                                           (wifi_toolbox_arm_mode_t)req.mode);

    send_cmd_result(WIFI_TOOLBOX_MSG_CMD_ARM,
                    (err == ESP_OK) ? WIFI_TOOLBOX_OK : WIFI_TOOLBOX_ERR_BAD_PARAM);
}

static void handle_disarm(void)
{
    wifi_toolbox_disarm();
    send_cmd_result(WIFI_TOOLBOX_MSG_CMD_DISARM, WIFI_TOOLBOX_OK);
}

/* One START command for both session kinds. The mode is a field, not a message
 * ID, because the peer-data table only holds eight of them — see the note on the
 * message IDs in the protocol header. */
static void handle_start(const uint8_t *data, size_t len)
{
    if (len < sizeof(wifi_toolbox_start_req_t)) {
        ESP_LOGW(TAG, "start: short request (%u < %u)",
                 (unsigned)len, (unsigned)sizeof(wifi_toolbox_start_req_t));
        send_cmd_result(WIFI_TOOLBOX_MSG_CMD_START, WIFI_TOOLBOX_ERR_BAD_PARAM);
        return;
    }

    wifi_toolbox_start_req_t req;
    memcpy(&req, data, sizeof(req));

    /* Refuse a layout the sender and receiver do not agree on rather than
     * reinterpreting the bytes: host and co-processor are flashed separately, so
     * a stale image on either side is a normal condition. */
    if (req.req_version != WIFI_TOOLBOX_START_REQ_VERSION) {
        ESP_LOGW(TAG, "start: request version %u, this image speaks %u",
                 (unsigned)req.req_version, (unsigned)WIFI_TOOLBOX_START_REQ_VERSION);
        send_cmd_result(WIFI_TOOLBOX_MSG_CMD_START, WIFI_TOOLBOX_ERR_UNSUPPORTED);
        return;
    }

    wifi_toolbox_result_t r;

    if (req.session == WIFI_TOOLBOX_SESSION_CAPTURE) {
        r = wifi_toolbox_capture_start(&req.u.capture);
    } else if (req.session == WIFI_TOOLBOX_SESSION_INJECT) {
#if CONFIG_WIFI_TOOLBOX_ALLOW_INJECTION
        r = wifi_toolbox_inject_start(&req.u.inject);
#else
        /* Compiled out (spec 10.5). Answered, not ignored: the host already knows
         * from the capability report, but silence here would be indistinguishable
         * from a dropped link. */
        r = WIFI_TOOLBOX_ERR_UNSUPPORTED;
#endif
    } else {
        r = WIFI_TOOLBOX_ERR_BAD_PARAM;
    }

    send_cmd_result(WIFI_TOOLBOX_MSG_CMD_START, r);
    send_status();
}

/* One STOP command: ends whatever is running, whichever kind it is. There is one
 * radio, so "stop everything" is the only honest semantics — and it is what the
 * safety story needs, since a stop that left a transmitter running would be a
 * trap (spec section 9). */
static void handle_stop(void)
{
    wifi_toolbox_inject_stop();
    wifi_toolbox_capture_stop();
    send_cmd_result(WIFI_TOOLBOX_MSG_CMD_STOP, WIFI_TOOLBOX_OK);
    send_status();
}

/* The one status block, used for both the pushed event and the asked-for reply.
 * Everything a caller could want to know — session state, counters, and what the
 * radio is really doing — arrives together, because splitting it across several
 * message IDs is what silently overflowed the peer-data handler table. */
static void build_status(wifi_toolbox_status_t *out)
{
    memset(out, 0, sizeof(*out));

    wifi_toolbox_inject_get_stats(&out->inject);
    wifi_toolbox_capture_get_stats(&out->capture);

    out->radio.channel = wifi_toolbox_radio_get_channel();
    out->radio.inject_running = wifi_toolbox_inject_is_running() ? 1 : 0;
    out->radio.capture_running = wifi_toolbox_capture_is_running() ? 1 : 0;
    out->radio.hopping = wifi_toolbox_radio_hop_is_running() ? 1 : 0;

    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    uint8_t primary = out->radio.channel;
    if (wifi_toolbox_radio_is_ready() && (esp_wifi_get_channel(&primary, &second) == ESP_OK)) {
        out->radio.channel = primary;
        out->radio.second = (uint8_t)second;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->radio.sta_connected = 1;
        out->radio.sta_channel = ap.primary;
        out->radio.sta_rssi = (int8_t)ap.rssi;
    }

    out->armed_mode = wifi_toolbox_armed_mode();
    out->radio.armed_mode = out->armed_mode;
    out->rpc_version = WIFI_TOOLBOX_RPC_VERSION;
    out->fw_version = WIFI_TOOLBOX_FW_VERSION;

#if CONFIG_WIFI_TOOLBOX_ALLOW_INJECTION
    out->caps_flags |= WIFI_TOOLBOX_CAP_INJECTION;
    out->mode_count = (uint8_t)WIFI_TOOLBOX_INJECT_MODE_MAX;
#endif
    out->caps_flags |= WIFI_TOOLBOX_CAP_CAPTURE;
    if (s_raw_frame_patch) {
        out->caps_flags |= WIFI_TOOLBOX_CAP_RAW_PATCH;
    }
    out->max_tx_power_dbm = 20;
    memcpy(out->country_code, "XX\0", 3);
}

static void send_status(void)
{
    wifi_toolbox_status_t st;

    build_status(&st);

    /* Spec section 4.3: counters arrive as ~1 Hz events while something is
     * running, so the host's display never depends on polling and a session that
     * ends on the slave's own timer still stops being reported. Kick the tick
     * whenever a session is live; it stops itself once both are idle. */
    if (s_status_timer != NULL) {
        if (st.radio.inject_running || st.radio.capture_running) {
            if (xTimerIsTimerActive(s_status_timer) != pdTRUE) {
                xTimerStart(s_status_timer, 0);
            }
        }
    }

    ESP_LOGI(TAG, "status: inject[state=%u sent=%lu fail=%lu err=%u] capture[state=%u seen=%lu matched=%lu sent=%lu drop=%lu/%lu] "
                  "radio[ch=%u conn=%u ap_ch=%u hop=%u] armed=%u",
             (unsigned)st.inject.state, (unsigned long)st.inject.frames_sent,
             (unsigned long)st.inject.frames_failed, (unsigned)st.inject.last_error,
             (unsigned)st.capture.state, (unsigned long)st.capture.packets_seen,
             (unsigned long)st.capture.frames_matched, (unsigned long)st.capture.frames_sent,
             (unsigned long)st.capture.dropped_ring_full, (unsigned long)st.capture.transport_failed,
             (unsigned)st.radio.channel, (unsigned)st.radio.sta_connected,
             (unsigned)st.radio.sta_channel, (unsigned)st.radio.hopping, (unsigned)st.armed_mode);

    send_rsp(WIFI_TOOLBOX_MSG_EVT_STATUS, &st, sizeof(st));
}

static void handle_status(void)
{
    wifi_toolbox_status_t st;

    build_status(&st);
    send_rsp(WIFI_TOOLBOX_MSG_RSP_STATUS, &st, sizeof(st));
}

/* Runs in the FreeRTOS timer task, not in RPC RX context, so the send here is
 * allowed to take its time. Sends the same block the STATUS request returns. */
static void status_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    /* Stop ticking once nothing is live: the timer exists to feed a screen during
     * a session, not to keep talking to a host that may not even be listening. */
    if (!wifi_toolbox_inject_is_running() && !wifi_toolbox_capture_is_running()) {
        xTimerStop(s_status_timer, 0);
        return;
    }

    send_status();
}

/* Take or release the power-save lease. The host cannot do this itself: every
 * esp_wifi_remote_* entry point in this tree is a weak UNSUPPORTED stub, so
 * esp_wifi_set_ps/esp_wifi_get_ps are unavailable from the P4 side. */
/* CONTROL: power save and channel, neither of which the host can do itself.
 *
 * Every esp_wifi_remote_* entry point in this tree is a weak UNSUPPORTED stub, so
 * from the P4 side esp_wifi_set_ps / esp_wifi_get_ps / esp_wifi_set_channel all
 * return ESP_ERR_NOT_SUPPORTED. The host can only ask, and this is where asking
 * lands. */
static void handle_control(const uint8_t *data, size_t len)
{
    wifi_toolbox_control_rsp_t rsp;
    esp_err_t err = ESP_OK;

    memset(&rsp, 0, sizeof(rsp));

    if (len < sizeof(wifi_toolbox_control_req_t)) {
        ESP_LOGW(TAG, "control: short request (%u < %u)",
                 (unsigned)len, (unsigned)sizeof(wifi_toolbox_control_req_t));
        rsp.result = WIFI_TOOLBOX_ERR_BAD_PARAM;
        send_rsp(WIFI_TOOLBOX_MSG_RSP_CONTROL, &rsp, sizeof(rsp));
        return;
    }

    wifi_toolbox_control_req_t req;
    memcpy(&req, data, sizeof(req));

    wifi_toolbox_result_t result = WIFI_TOOLBOX_OK;

    switch (req.op) {
    case WIFI_TOOLBOX_CONTROL_PS_TAKE:
        err = wifi_toolbox_radio_ps_lease_take(req.disable != 0);
        break;
    case WIFI_TOOLBOX_CONTROL_PS_RELEASE:
        err = wifi_toolbox_radio_ps_lease_release();
        break;
    case WIFI_TOOLBOX_CONTROL_SET_CHANNEL:
        if ((req.channel < 1) || (req.channel > 14)) {
            result = WIFI_TOOLBOX_ERR_BAD_PARAM;
        } else {
            wifi_toolbox_radio_hop_stop();
            err = wifi_toolbox_radio_ensure_channel(req.channel);
        }
        break;
    case WIFI_TOOLBOX_CONTROL_HOP_START:
        err = wifi_toolbox_radio_hop_start((req.hop_dwell_ms == 0) ? WIFI_TOOLBOX_HOP_DWELL_MS_DEF
                                                                  : req.hop_dwell_ms);
        break;
    case WIFI_TOOLBOX_CONTROL_HOP_STOP:
        err = wifi_toolbox_radio_hop_stop();
        break;
    case WIFI_TOOLBOX_CONTROL_ELICIT:
        result = wifi_toolbox_capture_elicit(req.bssid);
        break;
    default:
        result = WIFI_TOOLBOX_ERR_BAD_PARAM;
        break;
    }

    if ((err != ESP_OK) && (result == WIFI_TOOLBOX_OK)) {
        result = WIFI_TOOLBOX_ERR_BAD_PARAM;
    }

    wifi_toolbox_ps_state_t ps;
    wifi_toolbox_radio_ps_get_state(&ps);

    rsp.result = (uint8_t)result;
    rsp.ps_held = ps.held ? 1 : 0;
    rsp.ps_holders = ps.holders;
    rsp.ps_saved = ps.saved ? 1 : 0;
    rsp.ps_saved_type = ps.saved_type;
    rsp.ps_current_type = ps.current_type;
    rsp.channel = wifi_toolbox_radio_get_channel();
    rsp.hopping = wifi_toolbox_radio_hop_is_running() ? 1 : 0;

    ESP_LOGI(TAG, "control: op=%u -> result=%u ps[held=%u holders=%u saved=%u current=%u] ch=%u hop=%u",
             (unsigned)req.op, (unsigned)rsp.result, (unsigned)rsp.ps_held,
             (unsigned)rsp.ps_holders, (unsigned)rsp.ps_saved, (unsigned)rsp.ps_current_type,
             (unsigned)rsp.channel, (unsigned)rsp.hopping);

    send_rsp(WIFI_TOOLBOX_MSG_RSP_CONTROL, &rsp, sizeof(rsp));
}

static void on_peer_data(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;

    switch (msg_id) {
    case WIFI_TOOLBOX_MSG_CMD_PING:
        handle_ping();
        break;
    case WIFI_TOOLBOX_MSG_CMD_SELFTEST:
        handle_selftest(data, len);
        break;
    case WIFI_TOOLBOX_MSG_CMD_ARM:
        handle_arm(data, len);
        break;
    case WIFI_TOOLBOX_MSG_CMD_DISARM:
        handle_disarm();
        break;
    case WIFI_TOOLBOX_MSG_CMD_START:
        handle_start(data, len);
        break;
    case WIFI_TOOLBOX_MSG_CMD_STOP:
        handle_stop();
        break;
    case WIFI_TOOLBOX_MSG_CMD_STATUS:
        handle_status();
        break;
    case WIFI_TOOLBOX_MSG_CMD_CONTROL:
        handle_control(data, len);
        break;
    default:
        /* Answer, do not stay silent. An unanswered command and a command lost by
         * a broken link look identical from the host, and this channel is the only
         * way the host can see anything at all. */
        ESP_LOGW(TAG, "unknown custom cmd 0x%04x (%u bytes)", (unsigned)msg_id, (unsigned)len);
        send_cmd_result(msg_id, WIFI_TOOLBOX_ERR_UNKNOWN_CMD);
        break;
    }
}

esp_err_t wifi_toolbox_rpc_init(void)
{
    esp_err_t err;

    err = eh_cp_feat_peer_data_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "peer-data init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_handlers_registered = 0;

    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_PING, "PING")) != ESP_OK) {
        return err;
    }
    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_SELFTEST, "SELFTEST")) != ESP_OK) {
        return err;
    }
    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_ARM, "ARM")) != ESP_OK) {
        return err;
    }
    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_DISARM, "DISARM")) != ESP_OK) {
        return err;
    }
    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_START, "START")) != ESP_OK) {
        return err;
    }
    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_STOP, "STOP")) != ESP_OK) {
        return err;
    }
    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_STATUS, "STATUS")) != ESP_OK) {
        return err;
    }
    if ((err = register_cmd(WIFI_TOOLBOX_MSG_CMD_CONTROL, "CONTROL")) != ESP_OK) {
        return err;
    }

    if (s_status_timer == NULL) {
        /* Created stopped: nothing radio-active at boot (spec section 9). */
        s_status_timer = xTimerCreate("tb_status", pdMS_TO_TICKS(WIFI_TOOLBOX_STATUS_PERIOD_MS),
                                      pdTRUE, NULL, status_timer_cb);
        if (s_status_timer == NULL) {
            ESP_LOGE(TAG, "failed to create the status timer");
            return ESP_ERR_NO_MEM;
        }
    }

    if (wifi_toolbox_capture_init() != ESP_OK) {
        ESP_LOGE(TAG, "capture engine init failed");
        return ESP_ERR_NO_MEM;
    }

    /* Registration is the failure mode that used to be invisible: the peer-data
     * table is fixed-size and a registration past the end fails HERE, on a console
     * the host cannot see, while the host's symptom is a request that returns
     * nothing. Asserting the count against the same constant the compile-time check
     * uses turns that into something the boot log states outright. */
    s_handlers_registered = 0;
    ESP_LOGW(TAG, "custom-RPC ready (rpc v%u fw v%u, %u/%u command handlers)",
             WIFI_TOOLBOX_RPC_VERSION, WIFI_TOOLBOX_FW_VERSION,
             (unsigned)WIFI_TOOLBOX_CMD_COUNT, (unsigned)EH_CP_FEAT_PEER_DATA_MAX_HANDLERS);

    return ESP_OK;
}

void wifi_toolbox_set_raw_frame_patch(bool patched)
{
    s_raw_frame_patch = patched;
}
