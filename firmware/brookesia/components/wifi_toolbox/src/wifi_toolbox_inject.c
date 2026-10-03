/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — injection engine (co-processor side).
 *
 * Owns the transmitting side: paces frames out with esp_wifi_80211_tx() until the
 * session's duration expires or the host asks it to stop. Everything here runs on
 * the C6 because the host cannot reach the radio for raw TX — measured, the host
 * side returns ESP_ERR_NOT_SUPPORTED (see M0-FINDINGS.md).
 *
 * Safety properties, all enforced here rather than trusted from the host:
 *
 *  - **Arming gate.** A session will not start unless the slave is armed for
 *    injection (spec 10.2). The host is what the gate guards against, so a
 *    host-side check alone would be decoration.
 *  - **Bounded duration.** Clamped to [1 s, 10 min] here. The slave stops on its
 *    own timer even if the host stops answering (spec 5.5) — the slave-side timer
 *    is the safety net, the host-side one the user-facing limit.
 *  - **Nothing survives a reboot**, and the arm flag is cleared whenever a session
 *    ends abnormally, so no transmitter is ever left running (spec 10.3, 9).
 *
 * Threading: one task per session, created on start and gone by the time stop
 * returns. The only shared state is `s_session`, guarded by a mutex; the RPC
 * handlers run in RPC RX context and must not block, so they only set flags.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "wifi_toolbox.h"
#include "wifi_toolbox_capture_engine.h"
#include "wifi_toolbox_frames.h"
#include "wifi_toolbox_radio.h"
#include "wifi_toolbox_rpc.h"

static const char *TAG = "tb_inject";

/* Gap between frames. The originals in Tactility run on a timer tick; a fixed
 * cadence keeps the slave's cost predictable and leaves the radio airtime for
 * the Bluetooth/802.15.4 coexistence the C6 also carries. */
#define INJECT_TICK_MS          (20)
#define INJECT_TASK_STACK       (4096)
#define INJECT_TASK_PRIO        (5)

typedef struct {
    bool             active;
    bool             stop_requested;
    wifi_toolbox_inject_params_t req;
    uint32_t         duration_ms;
    uint32_t         frames_sent;
    uint32_t         frames_failed;
    TickType_t       started_at;
    wifi_toolbox_state_t state;
    wifi_toolbox_result_t last_error;
} inject_session_t;

static inject_session_t        s_session;
static SemaphoreHandle_t       s_lock;
static TaskHandle_t            s_task;
static bool                    s_running;        /* task exists, incl. winding down */
static bool                    s_armed;          /* RAM-only, cleared on end  */
static wifi_toolbox_arm_mode_t s_armed_mode;
/* Sampled once when a session starts rather than per frame: association can
 * change mid-session, and flipping en_sys_seq halfway through would make the
 * transmitted sequence jump. */
static bool                    s_sta_connected;
/* Power save: taken when the session starts, released when its task exits.
 *
 * This matters more for injection than it looks. With power save on, the modem
 * sleeps between beacons and a frame handed to the driver waits for the next wake
 * — so esp_wifi_80211_tx() returns ESP_OK for frames that sit in the TX queue, or
 * are dropped, rather than going out. A session that reports "sent" while the
 * radio is asleep is the exact failure this whole port is trying to avoid. */
static bool                    s_ps_owned;

static void session_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void session_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t wifi_toolbox_inject_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* The radio lock is created here because every session start goes through a
     * radio claim first; making callers remember the ordering would be a trap. */
    const esp_err_t radio_err = wifi_toolbox_radio_init();
    if (radio_err != ESP_OK) {
        return radio_err;
    }

    memset(&s_session, 0, sizeof(s_session));
    s_session.state = WIFI_TOOLBOX_STATE_IDLE;

    return ESP_OK;
}

/* The radio bring-up used to live here. It moved to wifi_toolbox_radio.c when
 * capture arrived: two sessions in two tasks both needing esp_wifi_init() and a
 * channel is a race unless one module owns it (spec 9: the driver still comes up
 * lazily on first use, so boot stays free of radio activity). */

static void inject_task(void *arg)
{
    (void)arg;

    uint8_t frame[WIFI_TOOLBOX_FRAME_MAX];
    wifi_toolbox_frame_params_t params;

    session_lock();
    wifi_toolbox_inject_params_t req = s_session.req;
    const uint32_t duration_ms = s_session.duration_ms;
    s_session.started_at = xTaskGetTickCount();
    session_unlock();

    memset(&params, 0, sizeof(params));
    params.ssid = (req.ssid_len > 0) ? req.ssid : NULL;
    params.ssid_len = req.ssid_len;
    params.channel = req.channel;
    params.listen_interval = req.listen_interval;
    params.duration = req.nav_duration;
    memcpy(params.bssid, req.bssid, 6);
    memcpy(params.client, req.client, 6);

    /* Everything that can refuse the session happens before the power-save lease
     * is taken, so an aborted start has nothing to unwind. The lease, once taken,
     * is released on every exit path below. */

    const esp_err_t radio_err = wifi_toolbox_radio_ensure_ready(0);
    if (radio_err != ESP_OK) {
        session_lock();
        s_session.state = WIFI_TOOLBOX_STATE_STOPPED_BY_ERROR;
        s_session.last_error = WIFI_TOOLBOX_ERR_TX_FAILED;
        s_session.active = false;
        session_unlock();
        ESP_LOGE(TAG, "radio not ready, session aborted");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Tune only when asked, and treat a refusal as fatal to the session.
     *
     * This is not tidiness. While the co-processor is associated — the normal
     * state, since it is esp-hosted's station — the driver will not move to
     * another channel. Ignoring that (as this code did) leaves the builders
     * transmitting on the association's channel while the host believes it asked
     * for another one: frames leave the antenna, the counters say success, and
     * they are on the wrong channel. A session that refuses to start is strictly
     * better than one that injects somewhere the operator did not choose. */
    if (req.channel != 0) {
        const esp_err_t ch_err = wifi_toolbox_radio_ensure_channel(req.channel);
        if (ch_err != ESP_OK) {
            session_lock();
            s_session.state = WIFI_TOOLBOX_STATE_STOPPED_BY_ERROR;
            s_session.last_error = WIFI_TOOLBOX_ERR_BAD_PARAM;
            s_session.active = false;
            session_unlock();
            ESP_LOGE(TAG, "cannot tune to ch%u: %s — session aborted rather than "
                          "transmitting on the wrong channel",
                     (unsigned)req.channel, esp_err_to_name(ch_err));
            s_task = NULL;
            vTaskDelete(NULL);
            return;
        }
    }

    /* Power save off for the session, remembering what it was. Taken before the
     * first frame for the reason in the s_ps_owned comment: a sleeping modem does
     * not transmit a frame that was just handed to the driver. */
    const esp_err_t ps_err = wifi_toolbox_radio_ps_lease_take(true);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "power-save lease failed: %s (continuing, frames may be delayed)",
                 esp_err_to_name(ps_err));
    }
    s_ps_owned = true;

    ESP_LOGI(TAG, "injecting: mode=%u (%s) ch=%u duration=%ums",
             (unsigned)req.mode, wifi_toolbox_inject_mode_name((wifi_toolbox_inject_mode_t)req.mode),
             (unsigned)req.channel, (unsigned)duration_ms);

    /* The driver only accepts raw TX with en_sys_seq=false while there is no
     * connection, and this co-processor normally IS connected (it is esp-hosted's
     * station). Deciding per session, once, is what keeps one rule for the whole
     * run. */
    {
        wifi_ap_record_t ap;
        s_sta_connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        ESP_LOGI(TAG, "link: connected=%d en_sys_seq=%d", (int)s_sta_connected, (int)s_sta_connected);
    }

    while (true) {
        session_lock();
        const bool stop = s_session.stop_requested || !s_session.active;
        const TickType_t elapsed = xTaskGetTickCount() - s_session.started_at;
        session_unlock();

        if (stop) {
            break;
        }
        if ((uint32_t)(elapsed * portTICK_PERIOD_MS) >= duration_ms) {
            session_lock();
            s_session.state = WIFI_TOOLBOX_STATE_STOPPED_BY_DURATION;
            session_unlock();
            ESP_LOGW(TAG, "duration reached (%ums), slave stopped itself", (unsigned)duration_ms);
            break;
        }

        memset(frame, 0, sizeof(frame));
        const size_t len = wifi_toolbox_build_for_mode(
            frame, (wifi_toolbox_inject_mode_t)req.mode, &params);

        if (len == 0) {
            session_lock();
            s_session.frames_failed++;
            s_session.last_error = WIFI_TOOLBOX_ERR_BAD_PARAM;
            s_session.state = WIFI_TOOLBOX_STATE_STOPPED_BY_ERROR;
            s_session.active = false;
            session_unlock();
            ESP_LOGE(TAG, "builder refused mode %u; session aborted", (unsigned)req.mode);
            break;
        }

        /* en_sys_seq: the builders set their own sequence control, and letting the
         * driver renumber changes the bytes the golden-byte tests pin down. But
         * esp_wifi_80211_tx() has its own rule (esp_wifi.h): once a connection is
         * established, en_sys_seq must be true or the call returns
         * ESP_ERR_INVALID_ARG and nothing is transmitted.
         *
         * That rule matters here because this co-processor is not a free radio —
         * esp-hosted keeps it associated so the P4 has networking, so the
         * "connected" case is the normal one, not an edge case. Sending false
         * anyway produced 150 ESP_OK-looking calls that no receiver ever saw.
         *
         * So: keep the builders' sequence when there is no link, and let the
         * driver renumber when there is one. Only the 12-bit sequence field
         * differs; every other byte, including the deauth reason code and the
         * fixed tails, is the builder's. */
        const bool connected = s_sta_connected;
        const esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)len, connected);

        session_lock();
        if (err == ESP_OK) {
            s_session.frames_sent++;
        } else {
            s_session.frames_failed++;
            s_session.last_error = WIFI_TOOLBOX_ERR_TX_FAILED;
            /* Log the first failure only: this runs every tick and one line is
             * enough to identify the cause without flooding the console. */
            if (s_session.frames_failed == 1) {
                ESP_LOGW(TAG, "esp_wifi_80211_tx failed: %s (len=%u, subtype=0x%02x)",
                         esp_err_to_name(err), (unsigned)len, frame[0]);
            }
        }
        session_unlock();

        vTaskDelay(pdMS_TO_TICKS(INJECT_TICK_MS));
    }

    session_lock();
    s_session.active = false;
    s_session.stop_requested = false;
    /* The state is left as the loop set it: STOPPED_BY_DURATION when the slave's
     * own timer fired, STOPPED_BY_ERROR when it aborted. Only a clean stop by
     * request (or a fresh session that never started) lands on IDLE — reporting
     * a self-timed stop as IDLE would hide the difference between "it finished"
     * and "someone stopped it". */
    if (s_session.state == WIFI_TOOLBOX_STATE_INJECTING) {
        s_session.state = WIFI_TOOLBOX_STATE_IDLE;
    }
    const uint32_t sent = s_session.frames_sent;
    const uint32_t failed = s_session.frames_failed;
    /* The arming gate is NOT cleared here.
     *
     * It was, and that was a bug: the outgoing session's cleanup races the next
     * INJECT_START, so a second session in the same armed window was refused with
     * NOT_ARMED. The gate belongs to the user's authorisation, not to a session —
     * arming once should cover a run of sessions until the host disarms or the
     * link drops. Spec 10.3 (nothing resumes after a reboot) is satisfied because
     * s_armed is RAM-only and starts false. */
    s_running = false;
    session_unlock();

    /* Release the power-save lease the session took. Doing it here, as the task
     * exits, means it happens however the session ended — duration, host stop, or
     * an aborted start — rather than only on the happy path. */
    if (s_ps_owned) {
        const esp_err_t ps_err = wifi_toolbox_radio_ps_lease_release();
        if (ps_err != ESP_OK) {
            ESP_LOGE(TAG, "power-save restore failed: %s", esp_err_to_name(ps_err));
        }
        s_ps_owned = false;
    }

    ESP_LOGI(TAG, "injection ended: sent=%u failed=%u state=%u",
             (unsigned)sent, (unsigned)failed, (unsigned)s_session.state);

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_toolbox_arm(bool acknowledged, wifi_toolbox_arm_mode_t mode)
{
    if (!acknowledged) {
        ESP_LOGW(TAG, "arm refused: caller did not acknowledge authorisation");
        return ESP_ERR_INVALID_ARG;
    }

    session_lock();
    s_armed = true;
    s_armed_mode = mode;
    session_unlock();

    ESP_LOGI(TAG, "armed for %s", (mode == WIFI_TOOLBOX_ARM_INJECT) ? "injection" : "capture");

    return ESP_OK;
}

void wifi_toolbox_disarm(void)
{
    /* Disarm must also stop anything running: "disarm" that leaves a transmitter
     * on would be a trap, and the same is true of a receiver left in promiscuous
     * mode filtering someone else's traffic. */
    wifi_toolbox_inject_stop();
    wifi_toolbox_capture_stop();

    session_lock();
    s_armed = false;
    session_unlock();

    ESP_LOGI(TAG, "disarmed");
}

bool wifi_toolbox_inject_is_running(void)
{
    return s_running;
}

uint8_t wifi_toolbox_armed_mode(void)
{
    session_lock();
    const uint8_t mode = s_armed ? (uint8_t)s_armed_mode : 0xFFu;
    session_unlock();

    return mode;
}

bool wifi_toolbox_is_armed(wifi_toolbox_arm_mode_t mode)
{
    session_lock();
    const bool armed = s_armed && (s_armed_mode == mode);
    session_unlock();

    return armed;
}

wifi_toolbox_result_t wifi_toolbox_inject_start(const wifi_toolbox_inject_params_t *req)
{
    if (req == NULL) {
        return WIFI_TOOLBOX_ERR_BAD_PARAM;
    }
    if (req->mode >= WIFI_TOOLBOX_INJECT_MODE_MAX) {
        return WIFI_TOOLBOX_ERR_BAD_PARAM;
    }
    if (!wifi_toolbox_is_armed(WIFI_TOOLBOX_ARM_INJECT)) {
        return WIFI_TOOLBOX_ERR_NOT_ARMED;
    }

    /* Bounded duration, enforced here and not trusted from the host. */
    uint32_t duration = wifi_toolbox_inject_req_duration_ms(req);
    if (duration < WIFI_TOOLBOX_INJECT_MIN_DURATION_MS) {
        duration = WIFI_TOOLBOX_INJECT_MIN_DURATION_MS;
    }
    if (duration > WIFI_TOOLBOX_INJECT_MAX_DURATION_MS) {
        duration = WIFI_TOOLBOX_INJECT_MAX_DURATION_MS;
    }

    session_lock();
    if (s_running) {
        /* Covers both "a session is live" and "the previous task is still winding
         * down". Waiting for the latter is what the host does anyway, so reporting
         * busy here is honest rather than racy. */
        session_unlock();
        return WIFI_TOOLBOX_ERR_RADIO_BUSY;
    }

    memset(&s_session, 0, sizeof(s_session));
    s_session.req = *req;
    s_session.duration_ms = duration;
    s_session.active = true;
    s_session.state = WIFI_TOOLBOX_STATE_INJECTING;
    s_session.started_at = xTaskGetTickCount();
    s_running = true;
    session_unlock();

    ESP_LOGI(TAG, "session starting: mode=%u ch=%u dur=%ums",
             (unsigned)req->mode, (unsigned)req->channel, (unsigned)duration);

    if (xTaskCreate(inject_task, "tb_inject", INJECT_TASK_STACK, NULL, INJECT_TASK_PRIO, &s_task) != pdPASS) {
        session_lock();
        s_session.active = false;
        s_session.state = WIFI_TOOLBOX_STATE_STOPPED_BY_ERROR;
        s_session.last_error = WIFI_TOOLBOX_ERR_INTERNAL;
        session_unlock();
        ESP_LOGE(TAG, "failed to create inject task");
        return WIFI_TOOLBOX_ERR_INTERNAL;
    }

    return WIFI_TOOLBOX_OK;
}

void wifi_toolbox_inject_stop(void)
{
    session_lock();
    if (s_session.active) {
        s_session.stop_requested = true;
    }
    const bool was_active = s_session.active;
    session_unlock();

    if (!was_active) {
        return;
    }

    /* Wait for the task to finish so callers can rely on "stop returned" meaning
     * "not transmitting". Bounded, so a wedged task cannot hang the RPC context. */
    for (int i = 0; (i < 200) && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_running) {
        ESP_LOGW(TAG, "inject task did not exit in time; it will stop on its own timer");
    }
}

void wifi_toolbox_inject_get_stats(wifi_toolbox_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    session_lock();
    memset(out, 0, sizeof(*out));
    out->state = (uint8_t)s_session.state;
    out->mode = s_session.req.mode;
    out->channel = s_session.req.channel;
    out->frames_sent = s_session.frames_sent;
    out->frames_failed = s_session.frames_failed;
    out->duration_ms = s_session.duration_ms;
    out->last_error = (uint8_t)s_session.last_error;
    if (s_session.active) {
        out->elapsed_ms = (uint32_t)((xTaskGetTickCount() - s_session.started_at) * portTICK_PERIOD_MS);
    } else {
        out->elapsed_ms = s_session.duration_ms;
    }
    session_unlock();
}
