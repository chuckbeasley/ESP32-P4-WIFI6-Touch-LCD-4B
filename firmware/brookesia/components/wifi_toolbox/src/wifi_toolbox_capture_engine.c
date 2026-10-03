/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — capture engine (co-processor side). See
 * wifi_toolbox_capture_engine.h for the callback/task split and why a full ring
 * drops the newest frame.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "eh_cp_feat_peer_data.h"

#include "wifi_toolbox.h"
#include "wifi_toolbox_capture.h"
#include "wifi_toolbox_capture_engine.h"
#include "wifi_toolbox_frames.h"
#include "wifi_toolbox_radio.h"
#include "wifi_toolbox_rpc.h"

static const char *TAG = "tb_capture";

/* Frames waiting to cross SDIO. Sized from the measured control-plane cost: a
 * transport round trip is on the order of a millisecond, so 16 slots absorb a
 * burst without either the ring or the SDIO queue being the bottleneck. The
 * failure mode when it is too small is a rising dropped_ring_full, which is
 * reported rather than hidden. */
#define CAP_RING_SLOTS          (16)
#define CAP_TASK_STACK          (4096)
#define CAP_TASK_PRIO           (5)

typedef struct {
    wifi_toolbox_frame_evt_t hdr;
    uint8_t                  payload[WIFI_TOOLBOX_CAP_FRAME_MAX];
} cap_slot_t;

static cap_slot_t             *s_ring;
static SemaphoreHandle_t       s_lock;
static TaskHandle_t            s_task;
static portMUX_TYPE            s_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t       s_head;          /* producer writes next        */
static volatile uint32_t       s_tail;          /* consumer reads next         */
static volatile uint32_t       s_count;
static volatile bool           s_running;       /* callback may touch the ring */
static volatile bool           s_stop;
/* A sender task exists, including one still draining after a stop. Distinct from
 * s_task, which the task clears by itself as its last act: a new session must not
 * reset the ring while an old consumer is still reading it, and it cannot rely on
 * that ordering being visible from the RPC context. */
static volatile bool           s_sender_alive;

static wifi_toolbox_state_t    s_state;
static uint8_t                 s_channel;
static uint32_t                s_filter;
static uint16_t                s_seq;
static bool                    s_hop;           /* this session is hopping */
static uint8_t                 s_target_bssid[6];
static bool                    s_ps_owned;      /* this session took the lease */

/* Counters. Written from the RX callback and the sender task, read from the RPC
 * context, so every access is inside the spinlock rather than relying on 32-bit
 * loads being atomic. */
static uint32_t s_packets_seen;
static uint32_t s_frames_matched;
static uint32_t s_frames_sent;
static uint32_t s_dropped_ring_full;
static uint32_t s_transport_failed;
static uint32_t s_frames_clipped;
static uint32_t s_bytes_sent;
static uint32_t s_max_queued;
static uint32_t s_last_error;
static uint32_t s_elicits;

static void count_inc(volatile uint32_t *counter)
{
    portENTER_CRITICAL(&s_mux);
    (*counter)++;
    portEXIT_CRITICAL(&s_mux);
}

static void count_add(volatile uint32_t *counter, uint32_t amount)
{
    portENTER_CRITICAL(&s_mux);
    (*counter) += amount;
    portEXIT_CRITICAL(&s_mux);
}

static void lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* Defined below, next to the task it wakes. Declared here because the RX
 * callback — the producer — is what calls it. */
static void notify_sender(void);

/* ---- Filter -------------------------------------------------------------- */

/* The filter is the classification bits plus two category bits. Only frames that
 * match cross SDIO, so this runs on every received frame and has to stay cheap:
 * a subtype test for the common cases, and the LLC/SNAP scan only when the
 * caller actually asked for EAPOL or PMKID.
 *
 * Category bits deliberately bypass the classifier: a caller that asked for
 * "all management" wants beacons and probe requests streamed without also
 * turning on the EAPOL scan. */
bool wifi_toolbox_capture_matches(const uint8_t *p, size_t len, uint32_t filter)
{
    uint32_t cls = 0;

    if ((p == NULL) || (len < 24)) {
        return false;
    }

    if (filter == 0) {
        filter = WIFI_TOOLBOX_CAP_FILTER_DEFAULT;
    }

    const uint8_t type = (uint8_t)((p[0] >> 2) & 0x3);

    if (type == 0) {
        if (filter & WIFI_TOOLBOX_CAP_FILTER_ALL_MGMT) {
            return true;
        }
        switch ((p[0] >> 4) & 0xF) {
        case 0x0C: cls |= WIFI_TOOLBOX_FRAME_DEAUTH;    break;
        case 0x0A: cls |= WIFI_TOOLBOX_FRAME_DISASSOC;  break;
        case 0x08: cls |= WIFI_TOOLBOX_FRAME_BEACON;    break;
        case 0x04: cls |= WIFI_TOOLBOX_FRAME_PROBE_REQ; break;
        default: break;
        }
    } else if (type == 2) {
        if (filter & WIFI_TOOLBOX_CAP_FILTER_ALL_DATA) {
            return true;
        }
    } else {
        return false;                       /* control and MISC are never kept */
    }

    /* EAPOL rides inside QoS data frames, which is most of what a modern AP
     * sends. The classifier scans a window of offsets for that reason; only pay
     * for it when the filter asks. */
    const uint32_t want = filter & (WIFI_TOOLBOX_FRAME_EAPOL | WIFI_TOOLBOX_FRAME_PMKID);
    if (want != 0) {
        cls |= (uint32_t)wifi_toolbox_classify(p, len) & want;
    }

    return (cls & filter) != 0;
}

/* ---- Ring ---------------------------------------------------------------- */

/* Returns the slot the frame was written to, or NULL when the ring is full.
 *
 * The frame copy happens outside the critical section: the producer is the only
 * writer of the head slot, so nobody can observe a half-written frame. Every
 * header field is therefore filled in before the slot is published, and the
 * publish is the head-index update inside the lock — which is what makes the
 * consumer's read of the slot ordered after this write. */
static cap_slot_t *ring_push(const uint8_t *payload, size_t payload_len,
                             const wifi_promiscuous_pkt_t *pkt,
                             wifi_promiscuous_pkt_type_t type)
{
    portENTER_CRITICAL(&s_mux);

    if (s_count >= CAP_RING_SLOTS) {
        s_dropped_ring_full++;
        if (s_count > s_max_queued) {
            s_max_queued = s_count;
        }
        portEXIT_CRITICAL(&s_mux);
        return NULL;
    }

    const uint32_t slot_index = s_head;

    portEXIT_CRITICAL(&s_mux);

    cap_slot_t *slot = &s_ring[slot_index];

    memset(&slot->hdr, 0, sizeof(slot->hdr));
    slot->hdr.timestamp_us = pkt->rx_ctrl.timestamp;
    slot->hdr.sig_len = (uint16_t)pkt->rx_ctrl.sig_len;
    slot->hdr.payload_len = (uint16_t)payload_len;
    slot->hdr.rssi = (int8_t)pkt->rx_ctrl.rssi;
    slot->hdr.channel = (uint8_t)pkt->rx_ctrl.channel;
    slot->hdr.seq = s_seq++;
    slot->hdr.pkt_type = (uint8_t)type;
    slot->hdr.flags = (uint8_t)(wifi_toolbox_classify(payload, payload_len) & 0xFFu);
    slot->hdr.clipped = (payload_len < (size_t)pkt->rx_ctrl.sig_len) ? 1u : 0u;

    memcpy(slot->payload, payload, payload_len);

    /* Publish. Everything the consumer reads is written before this point. */
    portENTER_CRITICAL(&s_mux);
    s_head = (s_head + 1u) % CAP_RING_SLOTS;
    s_count++;
    if (s_count > s_max_queued) {
        s_max_queued = s_count;
    }
    portEXIT_CRITICAL(&s_mux);

    return slot;
}

/* Runs in the Wi-Fi driver's RX context. Allocation-free, no blocking calls, and
 * it must not log per frame: the only log line here is rate-limited to one per
 * 256 packets, which is already an exceptional state. */
static void promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if ((buf == NULL) || !s_running) {
        return;
    }

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    size_t len = (size_t)pkt->rx_ctrl.sig_len;

    /* sig_len includes the FCS; the buffer holds at least that much. Clamp
     * before indexing rather than trusting the radio's own length. */
    if (len > (size_t)WIFI_TOOLBOX_CAP_FRAME_MAX) {
        len = WIFI_TOOLBOX_CAP_FRAME_MAX;
    }

    count_inc(&s_packets_seen);

    if (len < 24) {
        return;
    }

    if (!wifi_toolbox_capture_matches(payload, len, s_filter)) {
        return;
    }

    count_inc(&s_frames_matched);

    cap_slot_t *slot = ring_push(payload, len, pkt, type);
    if (slot == NULL) {
        /* The ring being full is the one loss the host cannot see in the frame
         * stream itself, so it is reported when it happens — throttled so that a
         * sustained overflow cannot flood the channel that is already behind.
         * The counter is the record; this log is only a hint that it moved. */
        if ((s_dropped_ring_full & 0xFFu) == 1u) {
            ESP_LOGW(TAG, "capture ring full (%u dropped)", (unsigned)s_dropped_ring_full);
        }
        return;
    }

    if (slot->hdr.clipped) {
        count_inc(&s_frames_clipped);
    }

    /* Everything the consumer needs is published; wake it. This is the only
     * cross-context call in the fast path and it is not a blocking one. */
    notify_sender();
}

/* ---- Sender task --------------------------------------------------------- */

/* One transport call per frame, with the header and the payload contiguous so a
 * record can never be split across two messages. A stack buffer rather than a
 * third copy of the frame: the consumer owns the slot it reads, and the memcpy
 * out of it is cheaper than the allocation would be. */
static void send_slot(const cap_slot_t *slot)
{
    uint8_t buf[sizeof(wifi_toolbox_frame_evt_t) + WIFI_TOOLBOX_CAP_FRAME_MAX];
    const size_t total = sizeof(slot->hdr) + slot->hdr.payload_len;

    memcpy(buf, &slot->hdr, sizeof(slot->hdr));
    memcpy(buf + sizeof(slot->hdr), slot->payload, slot->hdr.payload_len);

    const esp_err_t err = eh_cp_feat_peer_data_send(WIFI_TOOLBOX_MSG_EVT_FRAME, buf, total);

    if (err == ESP_OK) {
        count_inc(&s_frames_sent);
        count_add(&s_bytes_sent, (uint32_t)total);
    } else {
        /* The transport refused a frame we had already accepted. It is a loss
         * with a different cause from a full ring, so it gets its own counter —
         * "the link is dropping frames" and "the slave cannot keep up" call for
         * different fixes. */
        count_inc(&s_transport_failed);
        portENTER_CRITICAL(&s_mux);
        s_last_error = (uint32_t)err;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "frame send failed: %s", esp_err_to_name(err));
    }
}

static void capture_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t drained = 0;

        /* Bounded drain per pass: on stop, the loop must reach its exit check
         * promptly even while the ring keeps filling. */
        while (drained++ < 8) {
            portENTER_CRITICAL(&s_mux);
            const bool empty = (s_count == 0);
            if (empty) {
                portEXIT_CRITICAL(&s_mux);
                break;
            }
            const uint32_t idx = s_tail;
            s_tail = (s_tail + 1u) % CAP_RING_SLOTS;
            s_count--;
            portEXIT_CRITICAL(&s_mux);

            send_slot(&s_ring[idx]);
        }

        if (s_stop && (s_count == 0)) {
            break;
        }

        /* 5 ms when frames are flowing, otherwise park — the notify below wakes
         * us as soon as one arrives, so the capture path does not depend on this
         * delay for its latency. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(drained > 1 ? 1 : 5));
    }

    s_task = NULL;
    s_sender_alive = false;
    vTaskDelete(NULL);
}

/* Producer-side wake. Called outside the critical section: a notification is not
 * a blocking call, but there is no reason to hold a spinlock across it. */
static void notify_sender(void)
{
    TaskHandle_t task = s_task;

    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t wifi_toolbox_capture_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_ring == NULL) {
        /* Internal RAM on purpose: this is touched from the RX callback, and
         * PSRAM access from that context costs more than the frame copy. */
        s_ring = calloc(CAP_RING_SLOTS, sizeof(cap_slot_t));
        if (s_ring == NULL) {
            ESP_LOGE(TAG, "no memory for the capture ring (%u x %u bytes)",
                     (unsigned)CAP_RING_SLOTS, (unsigned)sizeof(cap_slot_t));
            return ESP_ERR_NO_MEM;
        }
    }

    s_state = WIFI_TOOLBOX_STATE_IDLE;

    ESP_LOGI(TAG, "capture engine ready: %u slots x %u bytes",
             (unsigned)CAP_RING_SLOTS, (unsigned)sizeof(cap_slot_t));

    return ESP_OK;
}

static void capture_stop_internal(void)
{
    /* Order matters. Kill the producer first: while s_running is false the
     * callback returns immediately without touching the ring, so the task can
     * then drain what is already queued without racing a new frame in. */
    s_running = false;

    if (s_sender_alive) {
        s_stop = true;
        notify_sender();

        /* Bounded wait, so a wedged task cannot hang the RPC context that calls
         * stop. "stop returned" means "not receiving" either way: promiscuous
         * mode is off by the time this returns. */
        for (int i = 0; (i < 200) && s_sender_alive; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_sender_alive) {
            ESP_LOGW(TAG, "capture task did not exit in time");
        }
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);

    /* The hopper belongs to the capture session: stopping the session must stop
     * moving the radio, or the station would be left hopping with nothing
     * receiving — a leftover radio behaviour, which spec section 9 rules out. */
    wifi_toolbox_radio_hop_stop();

    /* Put back the power save value the session found. Reported, not swallowed:
     * "we left power save off" is precisely the leftover this exists to prevent. */
    if (s_ps_owned) {
        const esp_err_t ps_err = wifi_toolbox_radio_ps_lease_release();
        if (ps_err != ESP_OK) {
            ESP_LOGE(TAG, "capture: power-save restore failed: %s", esp_err_to_name(ps_err));
        }
        s_ps_owned = false;
    }

    lock();
    s_state = WIFI_TOOLBOX_STATE_IDLE;
    s_hop = false;
    s_stop = false;
    unlock();

    ESP_LOGI(TAG, "capture stopped");
}

void wifi_toolbox_capture_stop(void)
{
    if (!s_running && !s_sender_alive) {
        return;
    }

    capture_stop_internal();
}

wifi_toolbox_result_t wifi_toolbox_capture_start(const wifi_toolbox_capture_params_t *req)
{
    if (req == NULL) {
        return WIFI_TOOLBOX_ERR_BAD_PARAM;
    }

    const bool hop = (req->hop != 0);

    if (hop) {
        /* A hopping session names no channel: where it hops is the slave's
         * regulatory configuration to decide (spec section 12.8), and the host
         * learns the sequence from the channel stamped on every frame. */
        if ((req->hop_dwell_ms != 0) &&
            ((req->hop_dwell_ms < WIFI_TOOLBOX_HOP_DWELL_MS_MIN) ||
             (req->hop_dwell_ms > WIFI_TOOLBOX_HOP_DWELL_MS_MAX))) {
            ESP_LOGW(TAG, "capture refused: hop dwell %ums out of range",
                     (unsigned)req->hop_dwell_ms);
            return WIFI_TOOLBOX_ERR_BAD_PARAM;
        }
    } else if ((req->channel < 1) || (req->channel > 14)) {
        ESP_LOGW(TAG, "capture refused: channel %u out of range", (unsigned)req->channel);
        return WIFI_TOOLBOX_ERR_BAD_PARAM;
    }

    if (!wifi_toolbox_is_armed(WIFI_TOOLBOX_ARM_CAPTURE)) {
        return WIFI_TOOLBOX_ERR_NOT_ARMED;
    }
    if (s_ring == NULL) {
        return WIFI_TOOLBOX_ERR_INTERNAL;
    }

    lock();
    const bool busy = s_running || s_sender_alive;
    unlock();
    if (busy) {
        return WIFI_TOOLBOX_ERR_RADIO_BUSY;
    }

    /* Injection and capture cannot share the radio: injection holds a fixed
     * channel and transmits continuously, capture puts the receiver into
     * promiscuous mode on the same interface. Refusing is honest; the self-test
     * path runs the two sessions in sequence for exactly this reason. */
    if (wifi_toolbox_inject_is_running()) {
        ESP_LOGW(TAG, "capture refused: an injection session is running");
        return WIFI_TOOLBOX_ERR_RADIO_BUSY;
    }

    /* Bring-up before taking the session lock: it may initialise the driver,
     * which is slow, and nothing here is visible to a producer yet. */
    esp_err_t err = wifi_toolbox_radio_ensure_ready(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture refused: radio not ready: %s", esp_err_to_name(err));
        return WIFI_TOOLBOX_ERR_TX_FAILED;
    }

    /* Tune to the requested channel, if that is a move at all. ensure_channel is a
     * no-op when the radio is already there, which is the common case here: the
     * co-processor normally sits on its association's channel, and a capture on
     * that channel is not a reconfiguration.
     *
     * Refusing a real move is deliberate. Measured while the co-processor is
     * associated (it is esp-hosted's station, so that is the normal state),
     * esp_wifi_set_channel() fails. Letting that failure reach the promiscuous
     * setup left the radio unusable — later sessions came back RADIO_BUSY with the
     * counters frozen until the co-processor rebooted. Taking the radio off its
     * association is the host's decision (spec section 5.2: disconnect, wait for
     * the event, then reconfigure); this end refuses to half-do it.
     *
     * A hopping session asks for no channel and is exempt: the hopper moves the
     * radio itself, and the frames it produces carry the channel they arrived on,
     * so the host never has to assume one. */

    if (!hop) {        wifi_ap_record_t ap;
        if ((esp_wifi_sta_get_ap_info(&ap) == ESP_OK) && (ap.primary != req->channel)) {
            ESP_LOGW(TAG, "capture refused: associated on ch%u, asked for ch%u "
                          "(disconnect before a fixed-channel capture)",
                     (unsigned)ap.primary, (unsigned)req->channel);
            return WIFI_TOOLBOX_ERR_BAD_PARAM;
        }

        const esp_err_t ch_err = wifi_toolbox_radio_ensure_channel(req->channel);
        if (ch_err != ESP_OK) {
            ESP_LOGE(TAG, "capture refused: cannot tune to ch%u: %s",
                     (unsigned)req->channel, esp_err_to_name(ch_err));
            return WIFI_TOOLBOX_ERR_TX_FAILED;
        }
    } else {
        const uint16_t dwell = (req->hop_dwell_ms == 0) ? WIFI_TOOLBOX_HOP_DWELL_MS_DEF
                                                        : req->hop_dwell_ms;
        const esp_err_t hop_err = wifi_toolbox_radio_hop_start(dwell);
        if (hop_err != ESP_OK) {
            ESP_LOGE(TAG, "capture refused: cannot start hopping: %s", esp_err_to_name(hop_err));
            return WIFI_TOOLBOX_ERR_TX_FAILED;
        }
    }

    wifi_promiscuous_filter_t radio_filter = {
        /* Management + data. Control frames are dropped by the radio before the
         * callback rather than classified and discarded by us: on a busy channel
         * they are most of the traffic and none of it is interesting. */
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    err = esp_wifi_set_promiscuous_filter(&radio_filter);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_promiscuous_filter: %s", esp_err_to_name(err));
        return WIFI_TOOLBOX_ERR_INTERNAL;
    }

    err = esp_wifi_set_promiscuous_rx_cb(promiscuous_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_promiscuous_rx_cb: %s", esp_err_to_name(err));
        return WIFI_TOOLBOX_ERR_INTERNAL;
    }

    /* Reset the ring and the counters for the new session. The producer is still
     * disabled, so this needs no synchronisation with it. */
    portENTER_CRITICAL(&s_mux);
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_packets_seen = 0;
    s_frames_matched = 0;
    s_frames_sent = 0;
    s_dropped_ring_full = 0;
    s_frames_clipped = 0;
    s_bytes_sent = 0;
    s_max_queued = 0;
    s_last_error = 0;
    portEXIT_CRITICAL(&s_mux);
    s_seq = 0;
    s_stop = false;
    s_hop = hop;
    s_elicits = 0;
    memcpy(s_target_bssid, req->target_bssid, sizeof(s_target_bssid));
    /* When hopping, the channel in the record is the one the frame was heard on,
     * which the callback fills in per frame; the session-level value is only a
     * label, so it is reported as 0 rather than naming a channel the radio is not
     * necessarily on. */
    s_channel = hop ? 0 : req->channel;
    s_filter = (req->filter_mask == 0) ? WIFI_TOOLBOX_CAP_FILTER_DEFAULT : req->filter_mask;

    /* Take a power-save lease as part of opening the session, not because the host
     * asked: a capture with power save on is a capture that misses frames — the
     * modem sleeps through them. That is exactly the "session left the radio in a
     * poor state" class of bug spec section 9 rules out, so the session fixes it
     * itself and puts back what it found when it ends. Idempotent, so a host that
     * asked for a lease first is not double-charged. */
    const esp_err_t ps_err = wifi_toolbox_radio_ps_lease_take(true);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "capture: power-save lease failed: %s (continuing, frames may be missed)",
                 esp_err_to_name(ps_err));
    }
    s_ps_owned = true;

    /* Set before the task exists, so the new task's exit cannot clear a flag that
     * has not been set yet. */
    s_sender_alive = true;

    if (xTaskCreate(capture_task, "tb_capture", CAP_TASK_STACK, NULL,
                    CAP_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create capture task");
        esp_wifi_set_promiscuous_rx_cb(NULL);
        s_task = NULL;
        s_sender_alive = false;
        return WIFI_TOOLBOX_ERR_INTERNAL;
    }

    lock();
    s_state = WIFI_TOOLBOX_STATE_CAPTURING;
    unlock();

    /* Enabling promiscuous mode last: the callback cannot fire before this, so
     * everything it touches is already initialised. */
    s_running = true;
    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_promiscuous(true): %s", esp_err_to_name(err));
        capture_stop_internal();
        return WIFI_TOOLBOX_ERR_INTERNAL;
    }

    ESP_LOGI(TAG, "capturing: ch=%u filter=0x%08lx",
             (unsigned)req->channel, (unsigned long)s_filter);

    return WIFI_TOOLBOX_OK;
}

bool wifi_toolbox_capture_is_running(void)
{
    return s_running;
}

/* Fire the handshake-forcing burst.
 *
 * This is a transmission, so it goes through the same arming gate as injection:
 * spec section 10.2 lists deauth elicitation alongside the injection modes for
 * exactly that reason. It is deliberately NOT a separate session — it borrows the
 * running one, because the radio is already on the target's channel and a second
 * session would have to fight it for the radio.
 *
 * The frame is built by the injection engine's own deauth builder, so the bytes
 * are the ones the golden-byte tests pin down rather than a second implementation
 * that could drift. */
wifi_toolbox_result_t wifi_toolbox_capture_elicit(const uint8_t bssid[6])
{
    if (!wifi_toolbox_is_armed(WIFI_TOOLBOX_ARM_CAPTURE)) {
        return WIFI_TOOLBOX_ERR_NOT_ARMED;
    }
    if (!s_running) {
        return WIFI_TOOLBOX_ERR_BAD_PARAM;
    }

    /* The target is the session's, not the caller's: a caller that could aim the
     * burst anywhere would make the arming dialog's promise ("...against the AP
     * you named") untrue. `bssid` may override it, but only when the session was
     * started without one. */
    uint8_t target[6];
    memcpy(target, (bssid != NULL) ? bssid : s_target_bssid, sizeof(target));

    uint8_t frame[WIFI_TOOLBOX_FRAME_MAX];
    const uint8_t client[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    const size_t len = wifi_toolbox_build_deauth(frame, client, target, false);
    if (len == 0) {
        return WIFI_TOOLBOX_ERR_INTERNAL;
    }

    const esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)len, true);

    count_inc(&s_elicits);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_last_error = (uint32_t)err;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "elicit: 80211_tx failed: %s", esp_err_to_name(err));
        return WIFI_TOOLBOX_ERR_TX_FAILED;
    }

    ESP_LOGI(TAG, "elicit: deauth burst sent to %02x:%02x:%02x:%02x:%02x:%02x",
             target[0], target[1], target[2], target[3], target[4], target[5]);

    return WIFI_TOOLBOX_OK;
}

void wifi_toolbox_capture_get_stats(wifi_toolbox_capture_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_mux);
    memset(out, 0, sizeof(*out));
    out->packets_seen = s_packets_seen;
    out->frames_matched = s_frames_matched;
    out->frames_sent = s_frames_sent;
    out->dropped_ring_full = s_dropped_ring_full;
    out->transport_failed = s_transport_failed;
    out->frames_clipped = s_frames_clipped;
    out->bytes_sent = s_bytes_sent;
    out->queued = s_count;
    out->max_queued = s_max_queued;
    out->elicits = s_elicits;
    out->last_error = s_last_error;
    out->frames_dropped = s_dropped_ring_full + s_transport_failed;
    portEXIT_CRITICAL(&s_mux);

    lock();
    out->state = (uint8_t)s_state;
    out->channel = s_channel;
    unlock();
}
