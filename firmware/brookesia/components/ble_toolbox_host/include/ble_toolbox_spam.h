/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Toolbox — the transmit path (the Tactility app's "BLE Spam" screen).
 *
 * *** This transmits. Read the gate before changing anything here. ***
 *
 * The Wi-Fi toolbox's injection runs on the co-processor, and its arming gate is
 * enforced there — on the chip that radiates, out of reach of the UI. This module is
 * different and the difference matters: the advertisements go out through THIS chip's
 * NimBLE host, so the gate is enforced here, in software the same party controls. That
 * is a weaker guarantee and is stated rather than glossed. What it still buys:
 *
 *   - arm() requires an explicit acknowledgement and a duration; neither can be
 *     defaulted into
 *   - the duration is capped by BLE_TOOLBOX_SPAM_MAX_HOLD_US and the cap is re-checked
 *     on every tick, so a closed UI, a stalled task or a forgotten Stop cannot leave a
 *     transmitter running
 *   - the whole transmit path is compiled out unless CONFIG_BLE_TOOLBOX_ALLOW_SPAM is
 *     set, so a shipped product omits the capability rather than hiding it
 *
 * The payloads are imitations of other vendors' pairing advertisements. They are
 * non-connectable and non-scannable, so this cannot accept a connection or act as a
 * bridge; it can still prompt pairing dialogs on nearby devices, which is precisely
 * what makes it a tool that needs authorisation rather than a feature.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ble_toolbox_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hard ceiling on how long one arming can last. Ten minutes, the same figure the Wi-Fi
 * toolbox's spec uses (section 8.5), so the two toolboxes cannot disagree about what
 * "too long" means. */
#define BLE_TOOLBOX_SPAM_MAX_HOLD_US    (10u * 60u * 1000000u)

typedef struct {
    uint8_t        family;          /* index into the family names */
    const char    *label;           /* the device being imitated */
    const uint8_t *data;            /* complete AD structure bytes */
    size_t         length;
} ble_toolbox_spam_payload_t;

typedef struct {
    uint32_t max_hold_us;           /* 0 means the default cap; never unlimited */
    bool     randomize_address;     /* advertise from a random address */
    bool     acknowledged;          /* MUST be true; false is refused */
} ble_toolbox_spam_params_t;

/* The four families, in payload order. */
extern const char *const ble_toolbox_spam_family_names[4];

size_t ble_toolbox_spam_payload_count(void);
const ble_toolbox_spam_payload_t *ble_toolbox_spam_payload(size_t index);

/* The nth payload of a family, or NULL when that family has fewer. */
const ble_toolbox_spam_payload_t *ble_toolbox_spam_payload_for_family(uint8_t family, size_t nth);

/* Open the gate. Refused unless params->acknowledged is set and the BLE service is up.
 * Returns ESP_ERR_NOT_SUPPORTED when the build has no transmit path. */
esp_err_t ble_toolbox_spam_arm(const ble_toolbox_spam_params_t *params);

/* Close the gate and stop advertising. Safe to call when not armed. */
esp_err_t ble_toolbox_spam_disarm(void);

bool ble_toolbox_spam_is_armed(void);

/* Seconds left before the cap stops it, for the UI's countdown. */
uint32_t ble_toolbox_spam_seconds_left(void);

/* Advertisements sent since arming, and the payload about to go out. */
uint32_t ble_toolbox_spam_sent(void);
uint8_t  ble_toolbox_spam_current_index(void);

/* One transmitting step: expiring check, set the payload, cycle to the next. Called from
 * a timer while armed. Public so the timer can live with the caller that owns the UI
 * cadence; it enforces the expiry itself rather than trusting the caller's interval. */
esp_err_t ble_toolbox_spam_tick(void);

#ifdef __cplusplus
}
#endif
