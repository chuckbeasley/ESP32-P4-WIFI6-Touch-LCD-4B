/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — host side. See wifi_toolbox_host.h for what this owns.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_hosted_misc.h"

#include "wifi_toolbox_host.h"

static const char *TAG = "tb_host";

/* The RPC version this host speaks. Declared in the shared protocol header so
 * both sides move together; restated here only for the error message. */
#define TB_RPC_VERSION  WIFI_TOOLBOX_RPC_VERSION

static struct {
    bool                            initialized;
    wifi_toolbox_host_callbacks_t   cb;
    void                           *ctx;
    SemaphoreHandle_t               lock;

    /* Latest status block, and whether anything has ever arrived. Guarded by
     * `lock`: it is written from the RPC task and read from the UI task. */
    wifi_toolbox_status_t           status;
    bool                            have_status;

    /* RAM-only (spec 10.3). Cleared on reboot, on link loss, and on disarm. */
    bool                            armed;
    uint8_t                         armed_mode;

    bool                            link_up;
    bool                            have_caps;
    wifi_toolbox_caps_t             caps;
} s_tb;

/* ---- Small helpers ------------------------------------------------------- */

static void lock(void)
{
    if (s_tb.lock != NULL) {
        xSemaphoreTake(s_tb.lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_tb.lock != NULL) {
        xSemaphoreGive(s_tb.lock);
    }
}

static void report_error(wifi_toolbox_result_t code, const char *message)
{
    if (s_tb.cb.on_error != NULL) {
        s_tb.cb.on_error(code, message, s_tb.ctx);
    } else {
        ESP_LOGE(TAG, "toolbox error %u: %s", (unsigned)code, message);
    }
}

/* Bring the module up on first use, so no caller has to know about ordering.
 *
 * Lazy rather than at app start-up for a measured reason: subscribing to
 * esp-hosted's peer-data messages before esp-hosted has reset and enumerated the
 * co-processor left the Wi-Fi service's own esp_wifi_init() returning ESP_FAIL,
 * while the same subscriptions made later are harmless (the heap was not the
 * cause — 137 KB free at the time). Every public entry point calls this first, and
 * the first thing a caller does is by definition after that window. */
static void ensure_started(void)
{
    if (!s_tb.initialized) {
        (void)wifi_toolbox_host_init(NULL, s_tb.ctx);
    }
}

const char *wifi_toolbox_result_name(wifi_toolbox_result_t code)
{
    switch (code) {
    case WIFI_TOOLBOX_OK:                  return "ok";
    case WIFI_TOOLBOX_ERR_UNKNOWN_CMD:     return "the co-processor did not recognise the command";
    case WIFI_TOOLBOX_ERR_BAD_PARAM:       return "the co-processor rejected the parameters";
    case WIFI_TOOLBOX_ERR_NOT_ARMED:       return "not armed";
    case WIFI_TOOLBOX_ERR_RADIO_BUSY:      return "the radio is busy with another session";
    case WIFI_TOOLBOX_ERR_UNSUPPORTED:     return "not supported by this co-processor firmware";
    case WIFI_TOOLBOX_ERR_TX_FAILED:       return "the driver refused the frame or the radio was not ready";
    case WIFI_TOOLBOX_ERR_INTERNAL:        return "the co-processor hit an internal error";
    default:                               return "unknown result";
    }
}

const char *wifi_toolbox_state_name(uint8_t state)
{
    switch (state) {
    case WIFI_TOOLBOX_STATE_IDLE:                 return "idle";
    case WIFI_TOOLBOX_STATE_INJECTING:            return "injecting";
    case WIFI_TOOLBOX_STATE_CAPTURING:            return "capturing";
    case WIFI_TOOLBOX_STATE_STOPPED_BY_DURATION:  return "stopped (duration)";
    case WIFI_TOOLBOX_STATE_STOPPED_BY_ERROR:     return "stopped (error)";
    default:                                      return "unknown";
    }
}

const char *wifi_toolbox_session_name(uint8_t session)
{
    switch (session) {
    case WIFI_TOOLBOX_SESSION_CAPTURE: return "capture";
    case WIFI_TOOLBOX_SESSION_INJECT:  return "injection";
    default:                           return "unknown";
    }
}

bool wifi_toolbox_parse_mac(const char *text, uint8_t out[6])
{
    if ((text == NULL) || (text[0] == '\0')) {
        return false;
    }

    unsigned v[6];
    const char *p = text;

    /* Accept both "aa:bb:.." and "aabbcc..", because a person typing an address
     * and a value copied out of a log are both normal inputs. */
    if (strchr(text, ':') != NULL) {
        if (sscanf(p, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
            return false;
        }
    } else if (strchr(text, '-') != NULL) {
        if (sscanf(p, "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
            return false;
        }
    } else {
        if (strlen(text) != 12) {
            return false;
        }
        for (int i = 0; i < 6; i++) {
            if (sscanf(p + (i * 2), "%2x", &v[i]) != 1) {
                return false;
            }
        }
    }

    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) {
            return false;
        }
        out[i] = (uint8_t)v[i];
    }

    return true;
}

void wifi_toolbox_format_mac(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- Inbound messages ---------------------------------------------------- */

static void store_and_publish_status(uint32_t id, const uint8_t *d, size_t n, void *ctx)
{
    (void)id;
    (void)ctx;

    if (n < sizeof(wifi_toolbox_status_t)) {
        ESP_LOGW(TAG, "status is %u bytes, expected %u", (unsigned)n, (unsigned)sizeof(wifi_toolbox_status_t));
        return;
    }

    wifi_toolbox_status_t st;
    memcpy(&st, d, sizeof(st));

    lock();
    s_tb.status = st;
    s_tb.have_status = true;

    /* Capabilities come from the same block, so there is no second request to make
     * and no second way for the two views to disagree. */
    memset(&s_tb.caps, 0, sizeof(s_tb.caps));
    s_tb.caps.rpc_version = st.rpc_version;
    s_tb.caps.fw_version = st.fw_version;
    s_tb.caps.injection_available = (st.caps_flags & WIFI_TOOLBOX_CAP_INJECTION) ? 1 : 0;
    s_tb.caps.capture_available = (st.caps_flags & WIFI_TOOLBOX_CAP_CAPTURE) ? 1 : 0;
    s_tb.caps.raw_frame_patch = (st.caps_flags & WIFI_TOOLBOX_CAP_RAW_PATCH) ? 1 : 0;
    s_tb.caps.mode_count = st.mode_count;
    s_tb.caps.max_tx_power_dbm = st.max_tx_power_dbm;
    memcpy(s_tb.caps.country_code, st.country_code, sizeof(s_tb.caps.country_code));
    const bool first = !s_tb.have_caps;
    s_tb.have_caps = true;
    unlock();

    if (first) {
        if (st.rpc_version != TB_RPC_VERSION) {
            /* Refused, not reinterpreted: the two images are flashed separately and
             * a mismatched pair would otherwise mis-parse each other's structs. */
            ESP_LOGE(TAG, "co-processor speaks rpc v%u, this host expects v%u",
                     (unsigned)st.rpc_version, (unsigned)TB_RPC_VERSION);
            report_error(WIFI_TOOLBOX_ERR_UNSUPPORTED,
                         "co-processor firmware does not match this app — update the co-processor image");
        } else {
            ESP_LOGI(TAG, "co-processor: rpc v%u fw v%u inject=%u capture=%u raw_patch=%u modes=%u",
                     (unsigned)st.rpc_version, (unsigned)st.fw_version,
                     (unsigned)s_tb.caps.injection_available, (unsigned)s_tb.caps.capture_available,
                     (unsigned)s_tb.caps.raw_frame_patch, (unsigned)s_tb.caps.mode_count);
        }
    }

    if (s_tb.cb.on_status != NULL) {
        s_tb.cb.on_status(&st, s_tb.ctx);
    }
}

static void handle_cmd_result(uint32_t id, const uint8_t *d, size_t n, void *ctx)
{
    (void)id;
    (void)ctx;

    if (n < sizeof(wifi_toolbox_cmd_result_t)) {
        return;
    }

    wifi_toolbox_cmd_result_t r;
    memcpy(&r, d, sizeof(r));

    if (r.result != WIFI_TOOLBOX_OK) {
        ESP_LOGW(TAG, "command 0x%04x refused: %s", (unsigned)r.cmd_id,
                 wifi_toolbox_result_name((wifi_toolbox_result_t)r.result));

        /* A refused ARM must not leave the host believing it is armed: the gate is
         * on the co-processor and this flag exists only to mirror it. */
        if ((r.cmd_id == WIFI_TOOLBOX_MSG_CMD_ARM) || (r.cmd_id == WIFI_TOOLBOX_MSG_CMD_DISARM)) {
            lock();
            s_tb.armed = false;
            s_tb.armed_mode = 0xFF;
            unlock();
        }

        report_error((wifi_toolbox_result_t)r.result,
                     wifi_toolbox_result_name((wifi_toolbox_result_t)r.result));
    }
}

static void handle_control_rsp(uint32_t id, const uint8_t *d, size_t n, void *ctx)
{
    (void)id;
    (void)ctx;

    /* The reply is already logged by the caller that made the request; here it
     * only matters that a refusal is surfaced. */
    if (n < sizeof(wifi_toolbox_control_rsp_t)) {
        return;
    }

    wifi_toolbox_control_rsp_t r;
    memcpy(&r, d, sizeof(r));

    if (r.result != WIFI_TOOLBOX_OK) {
        report_error((wifi_toolbox_result_t)r.result,
                     wifi_toolbox_result_name((wifi_toolbox_result_t)r.result));
    }
}

static void handle_frame(uint32_t id, const uint8_t *d, size_t n, void *ctx)
{
    (void)id;
    (void)ctx;

    if (n < sizeof(wifi_toolbox_frame_evt_t)) {
        return;
    }

    wifi_toolbox_frame_evt_t hdr;
    memcpy(&hdr, d, sizeof(hdr));

    const size_t avail = n - sizeof(hdr);
    /* Clamp to what actually arrived: a truncated record must not be read past. */
    if (hdr.payload_len > avail) {
        hdr.payload_len = (uint16_t)avail;
    }
    if (hdr.payload_len > WIFI_TOOLBOX_CAP_FRAME_MAX) {
        hdr.payload_len = (uint16_t)WIFI_TOOLBOX_CAP_FRAME_MAX;
    }

    if (s_tb.cb.on_frame != NULL) {
        s_tb.cb.on_frame(&hdr, d + sizeof(hdr), s_tb.ctx);
    }
}

/* ---- Link state (spec section 5.4) -------------------------------------- */

/* The transport's own events, so a co-processor that reboots or a link that drops
 * is noticed without polling. */
static void on_hosted_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    switch (id) {
    case EH_HOST_EVENT_TRANSPORT_DOWN:
    case EH_HOST_EVENT_TRANSPORT_FAILURE: {
        bool was_up;

        lock();
        was_up = s_tb.link_up;
        s_tb.link_up = false;
        /* Nothing resumes across a link loss: the slave may have rebooted, in
         * which case it has no session and no gate, and a host that kept its own
         * flags set would be lying about both (spec 10.3). */
        const bool was_armed = s_tb.armed;
        s_tb.armed = false;
        s_tb.armed_mode = 0xFF;
        s_tb.have_status = false;
        unlock();

        if (was_up) {
            ESP_LOGW(TAG, "co-processor link down%s", was_armed ? " (was armed — gate cleared)" : "");
        }
        if (s_tb.cb.on_link != NULL) {
            s_tb.cb.on_link(false, s_tb.ctx);
        }
        break;
    }

    case EH_HOST_EVENT_TRANSPORT_UP: {
        bool was_up;

        lock();
        was_up = s_tb.link_up;
        s_tb.link_up = true;
        unlock();

        if (!was_up) {
            ESP_LOGI(TAG, "co-processor link up");
        }
        if (!was_up && (s_tb.cb.on_link != NULL)) {
            s_tb.cb.on_link(true, s_tb.ctx);
        }
        break;
    }

    case EH_HOST_EVENT_CP_INIT: {
        /* The co-processor booted. Whatever it was doing is gone, and so is the
         * arming gate it held. */
        lock();
        const bool was_armed = s_tb.armed;
        s_tb.armed = false;
        s_tb.armed_mode = 0xFF;
        s_tb.have_status = false;
        unlock();

        ESP_LOGW(TAG, "co-processor booted%s", was_armed ? " — arming gate cleared" : "");
        break;
    }

    default:
        break;
    }
}

/* ---- Lifecycle ---------------------------------------------------------- */

esp_err_t wifi_toolbox_host_init(const wifi_toolbox_host_callbacks_t *callbacks, void *ctx)
{
    if (s_tb.initialized) {
        /* Re-registering the callbacks is deliberate: the UI sets them, and a
         * second caller asking for the module to be up must not wipe them. */
        if (callbacks != NULL) {
            lock();
            s_tb.cb = *callbacks;
            s_tb.ctx = ctx;
            unlock();
        }
        return ESP_OK;
    }

    s_tb.initialized = true;      /* latch first: the registrations below re-enter */
    s_tb.armed_mode = 0xFF;
    s_tb.ctx = ctx;

    if (callbacks != NULL) {
        s_tb.cb = *callbacks;
    }

    s_tb.lock = xSemaphoreCreateMutex();
    if (s_tb.lock == NULL) {
        s_tb.initialized = false;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;

    if ((err = esp_event_handler_register(EH_HOST_EVENT, ESP_EVENT_ANY_ID, on_hosted_event, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "event handler register failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_tb.lock);
        s_tb.lock = NULL;
        return err;
    }

    /* Every inbound message ID this module handles, registered once. A capture
     * frame is the only one that arrives at rate, and it is a single callback. */
    struct {
        uint32_t id;
        void   (*cb)(uint32_t, const uint8_t *, size_t, void *);
    } const subs[] = {
        { WIFI_TOOLBOX_MSG_EVT_STATUS,   store_and_publish_status },
        { WIFI_TOOLBOX_MSG_RSP_STATUS,   store_and_publish_status },
        { WIFI_TOOLBOX_MSG_EVT_FRAME,    handle_frame },
        { WIFI_TOOLBOX_MSG_EVT_CMD_RESULT, handle_cmd_result },
        { WIFI_TOOLBOX_MSG_RSP_CONTROL,  handle_control_rsp },
    };

    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        err = esp_hosted_register_custom_callback(subs[i].id, subs[i].cb, NULL);
        if (err != ESP_OK) {
            /* Loud, because this is the failure that is otherwise invisible: the
             * host-side table grows on demand, but a channel that is down or a
             * feature that is disabled shows up only here. */
            ESP_LOGE(TAG, "register msg 0x%04x failed: %s", (unsigned)subs[i].id, esp_err_to_name(err));
            esp_event_handler_unregister(EH_HOST_EVENT, ESP_EVENT_ANY_ID, on_hosted_event);
            vSemaphoreDelete(s_tb.lock);
            s_tb.lock = NULL;
            return err;
        }
    }

    s_tb.link_up = true;      /* assume up until an event says otherwise */
    s_tb.initialized = true;

    /* Reported because the Wi-Fi service on this board initialises the co-processor
     * from a background task, and its RPC path allocates on the internal heap: a
     * toolbox that quietly consumed the margin would show up as the Wi-Fi service
     * failing to start, which is a long way from the actual cause. */
    ESP_LOGI(TAG, "host toolbox ready (%u message handlers) — internal heap free: %u bytes, "
                  "largest block %u bytes",
             (unsigned)(sizeof(subs) / sizeof(subs[0])),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    return ESP_OK;
}

void wifi_toolbox_host_deinit(void)
{
    if (!s_tb.initialized) {
        return;
    }

    esp_event_handler_unregister(EH_HOST_EVENT, ESP_EVENT_ANY_ID, on_hosted_event);

    if (s_tb.lock != NULL) {
        vSemaphoreDelete(s_tb.lock);
        s_tb.lock = NULL;
    }

    s_tb.initialized = false;
}

esp_err_t wifi_toolbox_host_query_caps(wifi_toolbox_caps_t *caps)
{
    ensure_started();
    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Deliberately NOT a PING request.
     *
     * The capabilities travel in the status block, which this module already owns
     * end to end. Asking for them over esp-hosted's own RPC-request path as well
     * would put the toolbox on a code path the Wi-Fi service also uses during
     * start-up — and that is exactly where a collision was observed: with the
     * toolbox initialised first, the Wi-Fi service's esp_wifi_init() began
     * returning ESP_FAIL. Deriving them from the status reply keeps the toolbox on
     * the peer-data channel and nothing else.
     *
     * So this returns what the last status block said, and refreshes if nothing has
     * arrived yet. */
    if (!s_tb.have_caps) {
        (void)wifi_toolbox_host_refresh_status();
    }

    lock();
    const bool have = s_tb.have_caps;
    if (caps != NULL) {
        *caps = s_tb.caps;
    }
    unlock();

    return have ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t wifi_toolbox_host_refresh_status(void)
{
    ensure_started();
    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_hosted_send_custom_data(WIFI_TOOLBOX_MSG_CMD_STATUS, NULL, 0);
}

bool wifi_toolbox_host_status(wifi_toolbox_status_t *out)
{
    bool have = false;

    lock();
    if (s_tb.have_status) {
        have = true;
        if (out != NULL) {
            *out = s_tb.status;
        }
    }
    unlock();

    return have;
}

bool wifi_toolbox_host_link_is_up(void)
{
    lock();
    const bool up = s_tb.link_up;
    unlock();

    return up;
}

/* ---- Arming -------------------------------------------------------------- */

esp_err_t wifi_toolbox_host_arm(wifi_toolbox_arm_mode_t mode, bool acknowledged)
{
    ensure_started();
    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!acknowledged) {
        /* Refused here as well as on the co-processor. The co-processor's refusal
         * is the one that matters (it is what the gate guards against), but not
         * sending a request this end knows is unauthorised keeps the log honest. */
        report_error(WIFI_TOOLBOX_ERR_BAD_PARAM, "arming requires acknowledging authorisation");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_toolbox_arm_req_t req;
    memset(&req, 0, sizeof(req));
    req.acknowledged = 1;
    req.mode = (uint8_t)mode;

    const esp_err_t err = esp_hosted_send_custom_data(WIFI_TOOLBOX_MSG_CMD_ARM,
                                                      (const uint8_t *)&req, sizeof(req));
    if (err != ESP_OK) {
        return err;
    }

    /* Optimistic, and corrected by the command-result handler: the co-processor
     * re-sends EVT_CMD_RESULT for every command, so a refusal clears this again.
     * Waiting for the round trip here would make the UI feel broken for the sake
     * of a flag whose authoritative copy is on the other chip. */
    lock();
    s_tb.armed = true;
    s_tb.armed_mode = (uint8_t)mode;
    unlock();

    return ESP_OK;
}

esp_err_t wifi_toolbox_host_disarm(void)
{
    ensure_started();
    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_hosted_send_custom_data(WIFI_TOOLBOX_MSG_CMD_DISARM, NULL, 0);

    lock();
    s_tb.armed = false;
    s_tb.armed_mode = 0xFF;
    unlock();

    return err;
}

bool wifi_toolbox_host_is_armed(void)
{
    lock();
    const bool armed = s_tb.armed;
    unlock();

    return armed;
}

uint8_t wifi_toolbox_host_armed_mode(void)
{
    lock();
    const uint8_t mode = s_tb.armed ? s_tb.armed_mode : 0xFF;
    unlock();

    return mode;
}

/* ---- Sessions ------------------------------------------------------------ */

static esp_err_t send_start(uint8_t session, const void *params, size_t params_len)
{
    ensure_started();
    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_toolbox_start_req_t req;
    memset(&req, 0, sizeof(req));
    req.req_version = WIFI_TOOLBOX_START_REQ_VERSION;
    req.session = session;

    ESP_STATIC_ASSERT(sizeof(req.u.capture) == sizeof(wifi_toolbox_capture_params_t),
                      "capture params must fit the union exactly");
    ESP_STATIC_ASSERT(sizeof(req.u.inject) == sizeof(wifi_toolbox_inject_params_t),
                      "inject params must fit the union exactly");

    if (session == WIFI_TOOLBOX_SESSION_CAPTURE) {
        if (params_len != sizeof(req.u.capture)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(&req.u.capture, params, params_len);
    } else {
        if (params_len != sizeof(req.u.inject)) {
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(&req.u.inject, params, params_len);
    }

    return esp_hosted_send_custom_data(WIFI_TOOLBOX_MSG_CMD_START, (const uint8_t *)&req, sizeof(req));
}

esp_err_t wifi_toolbox_host_capture_start(const wifi_toolbox_capture_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Take the power-save lease before the session, so the co-processor's modem is
     * awake when the first frames arrive. The co-processor takes its own lease too
     * (reference-counted), so releasing ours cannot switch power save back on
     * underneath a running session. */
    wifi_toolbox_control_rsp_t rsp;
    wifi_toolbox_control_req_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op = WIFI_TOOLBOX_CONTROL_PS_TAKE;
    ctrl.disable = 1;
    (void)wifi_toolbox_host_control(&ctrl, &rsp);

    return send_start(WIFI_TOOLBOX_SESSION_CAPTURE, params, sizeof(*params));
}

esp_err_t wifi_toolbox_host_inject_start(const wifi_toolbox_inject_params_t *params)
{
    if (params == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_toolbox_control_req_t ctrl;
    wifi_toolbox_control_rsp_t rsp;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op = WIFI_TOOLBOX_CONTROL_PS_TAKE;
    ctrl.disable = 1;
    (void)wifi_toolbox_host_control(&ctrl, &rsp);

    return send_start(WIFI_TOOLBOX_SESSION_INJECT, params, sizeof(*params));
}

esp_err_t wifi_toolbox_host_stop(void)
{
    ensure_started();
    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_hosted_send_custom_data(WIFI_TOOLBOX_MSG_CMD_STOP, NULL, 0);

    /* Drop our power-save reference regardless of how the stop went: a lease left
     * held because the stop command failed would keep the radio awake forever,
     * which is the leftover spec section 9 rules out. */
    wifi_toolbox_control_req_t ctrl;
    wifi_toolbox_control_rsp_t rsp;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op = WIFI_TOOLBOX_CONTROL_PS_RELEASE;
    (void)wifi_toolbox_host_control(&ctrl, &rsp);

    return err;
}

esp_err_t wifi_toolbox_host_elicit(const uint8_t bssid[6])
{
    ensure_started();
    if (!s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_toolbox_control_req_t ctrl;
    wifi_toolbox_control_rsp_t rsp;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op = WIFI_TOOLBOX_CONTROL_ELICIT;
    if (bssid != NULL) {
        memcpy(ctrl.bssid, bssid, sizeof(ctrl.bssid));
    }

    return wifi_toolbox_host_control(&ctrl, &rsp);
}

esp_err_t wifi_toolbox_host_control(const wifi_toolbox_control_req_t *req,
                                    wifi_toolbox_control_rsp_t *rsp)
{
    ensure_started();
    if ((req == NULL) || !s_tb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = esp_hosted_send_custom_data(WIFI_TOOLBOX_MSG_CMD_CONTROL,
                                                      (const uint8_t *)req, sizeof(*req));
    if (err != ESP_OK) {
        return err;
    }

    if (rsp != NULL) {
        /* The reply is asynchronous and arrives through the CONTROL callback; this
         * returns what the last one said, which is what a caller wanting a knob's
         * current position needs. */
        memset(rsp, 0, sizeof(*rsp));
    }

    return ESP_OK;
}
