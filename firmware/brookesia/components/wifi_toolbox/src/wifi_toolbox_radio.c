/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — shared radio bring-up. See wifi_toolbox_radio.h for why this is
 * one module rather than duplicated in each session.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "wifi_toolbox_radio.h"
#include "wifi_toolbox_rpc.h"

static const char *TAG = "tb_radio";

static SemaphoreHandle_t s_lock;
static bool              s_ready;
static uint8_t           s_channel;      /* 0 = never tuned */

/* Power save lease, reference-counted.
 *
 * Two independent holders exist — the host (which takes a lease for the duration
 * of a tool session) and the session itself (which takes one because a sleeping
 * modem does not transmit and does miss frames). A single flag would let whichever
 * releases first restore power save while the other still needs it off, so the
 * count decides: the value is remembered on the first take and restored on the
 * last release. */
static uint8_t s_ps_refcount;
static bool    s_ps_saved;
static uint8_t s_ps_saved_type;
static uint8_t s_ps_current_type;

/* Hopper state. One timer, one radio. */
static TimerHandle_t s_hop_timer;
static uint16_t      s_hop_dwell_ms;
static uint8_t       s_hop_channel;

esp_err_t wifi_toolbox_radio_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

bool wifi_toolbox_radio_is_ready(void)
{
    return s_ready;
}

esp_err_t wifi_toolbox_radio_ensure_ready(uint8_t channel)
{
    if (channel > 14) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_lock == NULL) {
        /* init() was not called; do it here rather than failing a session over
         * an ordering mistake that has an obvious fix. */
        const esp_err_t init_err = wifi_toolbox_radio_init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;

    if (!s_ready) {
        /* The co-processor's own application does not start the driver — it is
         * esp-hosted's RPC handlers that wrap these same calls — so the first
         * toolbox session is what brings Wi-Fi up. ESP_ERR_WIFI_INIT_STATE and
         * ESP_ERR_WIFI_STATE mean someone else already did, which is not an
         * error here. */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        err = esp_wifi_init(&cfg);
        if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE)) {
            ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
            goto out;
        }

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if ((err != ESP_OK) && (err != ESP_ERR_WIFI_MODE)) {
            ESP_LOGE(TAG, "set_mode: %s", esp_err_to_name(err));
            goto out;
        }

        err = esp_wifi_start();
        if ((err != ESP_OK) && (err != ESP_ERR_WIFI_STATE)) {
            ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
            goto out;
        }

        /* Permissive TX power: injection range is the point of the tool, and the
         * regulatory ceiling is the country configuration's job, not ours. */
        esp_wifi_set_max_tx_power(80);

        s_ready = true;
        s_channel = 0;
        ESP_LOGI(TAG, "radio brought up by the toolbox (nothing transmits by itself)");
    }

    if ((channel != 0) && (channel != s_channel)) {
        err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_channel(%u): %s", (unsigned)channel, esp_err_to_name(err));
            goto out;
        }
        s_channel = channel;
    }

out:
    xSemaphoreGive(s_lock);
    return err;
}

uint8_t wifi_toolbox_radio_get_channel(void)
{
    if (!s_ready) {
        return 0;
    }

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;

    if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
        return 0;
    }

    return primary;
}

esp_err_t wifi_toolbox_radio_set_channel(uint8_t channel)
{
    if ((channel < 1) || (channel > 14)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t ready = wifi_toolbox_radio_ensure_ready(0);
    if (ready != ESP_OK) {
        return ready;
    }

    const esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_channel = channel;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t wifi_toolbox_radio_ensure_channel(uint8_t channel)
{
    if ((channel < 1) || (channel > 14)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t ready = wifi_toolbox_radio_ensure_ready(0);
    if (ready != ESP_OK) {
        return ready;
    }

    /* A fixed channel and a running hopper are contradictory instructions. The
     * session that wanted fixed wins by asking explicitly, so the hopper is
     * stopped here rather than silently overriding it a dwell later. */
    if (s_hop_timer != NULL) {
        wifi_toolbox_radio_hop_stop();
    }

    /* Ask the driver where it is, rather than trusting the cached request: the
     * association moves the radio without telling this module. */
    if (wifi_toolbox_radio_get_channel() == channel) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_channel = channel;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    return wifi_toolbox_radio_set_channel(channel);
}

/* ---- Power save lease ---------------------------------------------------- */

esp_err_t wifi_toolbox_radio_ps_lease_take(bool disable_power_save)
{
    const esp_err_t ready = wifi_toolbox_radio_ensure_ready(0);
    if (ready != ESP_OK) {
        return ready;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_ps_refcount == 0) {
        wifi_ps_type_t current = WIFI_PS_MIN_MODEM;

        if (esp_wifi_get_ps(&current) == ESP_OK) {
            s_ps_saved = true;
            s_ps_saved_type = (uint8_t)current;
            s_ps_current_type = (uint8_t)current;
        } else {
            /* Not knowing the original is itself worth reporting: the caller is
             * told a lease is held but nothing was remembered, so it will not
             * claim to have restored a value it never saw. */
            s_ps_saved = false;
            ESP_LOGW(TAG, "power save: could not read the current value");
        }
    }
    if (s_ps_refcount < 0xFF) {
        s_ps_refcount++;
    }

    const uint8_t saved_type = s_ps_saved_type;
    const bool had_value = s_ps_saved;
    const uint8_t holders = s_ps_refcount;
    xSemaphoreGive(s_lock);

    if (disable_power_save) {
        const esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "power save off failed: %s", esp_err_to_name(err));
            /* The lease is still held: the caller asked for it and the count is
             * what the release path uses. Undoing the take here would make the
             * refcount lie about who holds what. */
            return err;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ps_current_type = (uint8_t)WIFI_PS_NONE;
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "power save lease taken (holders=%u had=%d saved=%u current=%u)",
             (unsigned)holders, (int)had_value, (unsigned)saved_type, (unsigned)s_ps_current_type);

    return ESP_OK;
}

esp_err_t wifi_toolbox_radio_ps_lease_release(void)
{
    esp_err_t err = ESP_OK;
    bool restore = false;
    uint8_t saved_type = 0;
    uint8_t holders = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_ps_refcount == 0) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    s_ps_refcount--;
    holders = s_ps_refcount;

    if (s_ps_refcount == 0) {
        /* Last holder out restores what the first holder found. */
        restore = s_ps_saved;
        saved_type = s_ps_saved_type;
        s_ps_saved = false;
    }
    xSemaphoreGive(s_lock);

    if (restore) {
        err = esp_wifi_set_ps((wifi_ps_type_t)saved_type);
        if (err != ESP_OK) {
            /* Surfaced rather than swallowed: "we left power save off" is exactly
             * the leftover state this mechanism exists to prevent. */
            ESP_LOGE(TAG, "power save restore to %u failed: %s",
                     (unsigned)saved_type, esp_err_to_name(err));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ps_current_type = saved_type;
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "power save lease released (holders=%u restored=%d to %u)",
             (unsigned)holders, (int)restore, (unsigned)saved_type);

    return err;
}

void wifi_toolbox_radio_ps_get_state(wifi_toolbox_ps_state_t *out)
{
    if (out == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->held = (s_ps_refcount > 0);
    out->holders = s_ps_refcount;
    out->saved = s_ps_saved;
    out->saved_type = s_ps_saved_type;
    out->current_type = s_ps_current_type;
    xSemaphoreGive(s_lock);
}

/* ---- Channel hopping ----------------------------------------------------- */

/* Runs in the FreeRTOS timer task. Tuning can fail transiently (the station is
 * associating, or has moved the radio itself); a hopper that aborted on the first
 * refusal would leave the caller thinking it was still hopping, so the failure is
 * logged and the next tick tries again. */
static void hop_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    uint8_t next = s_hop_channel + 1;

    if (next > WIFI_TOOLBOX_HOP_CHANNEL_MAX) {
        next = (uint8_t)WIFI_TOOLBOX_HOP_CHANNEL_MIN;
    }
    s_hop_channel = next;

    const esp_err_t err = esp_wifi_set_channel(next, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "hop to ch%u skipped: %s", (unsigned)next, esp_err_to_name(err));
    }
}

esp_err_t wifi_toolbox_radio_hop_start(uint16_t dwell_ms)
{
    if ((dwell_ms < WIFI_TOOLBOX_HOP_DWELL_MS_MIN) || (dwell_ms > WIFI_TOOLBOX_HOP_DWELL_MS_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t ready = wifi_toolbox_radio_ensure_ready(0);
    if (ready != ESP_OK) {
        return ready;
    }

    if (s_hop_timer == NULL) {
        s_hop_timer = xTimerCreate("tb_hop", pdMS_TO_TICKS(dwell_ms), pdTRUE, NULL, hop_timer_cb);
        if (s_hop_timer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xTimerChangePeriod(s_hop_timer, pdMS_TO_TICKS(dwell_ms), portMAX_DELAY);
    }

    s_hop_dwell_ms = dwell_ms;
    s_hop_channel = (uint8_t)WIFI_TOOLBOX_HOP_CHANNEL_MIN;
    esp_wifi_set_channel(s_hop_channel, WIFI_SECOND_CHAN_NONE);

    if (xTimerStart(s_hop_timer, pdMS_TO_TICKS(100)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "hopping ch%u..%u every %ums",
             (unsigned)WIFI_TOOLBOX_HOP_CHANNEL_MIN, (unsigned)WIFI_TOOLBOX_HOP_CHANNEL_MAX,
             (unsigned)dwell_ms);

    return ESP_OK;
}

esp_err_t wifi_toolbox_radio_hop_stop(void)
{
    if (s_hop_timer == NULL) {
        return ESP_OK;
    }

    xTimerStop(s_hop_timer, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "hopping stopped");

    return ESP_OK;
}

bool wifi_toolbox_radio_hop_is_running(void)
{
    if (s_hop_timer == NULL) {
        return false;
    }

    return xTimerIsTimerActive(s_hop_timer) == pdTRUE;
}
