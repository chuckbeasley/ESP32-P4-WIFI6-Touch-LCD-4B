/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — shared radio bring-up.
 *
 * Injection and capture are two sessions that both need the same Wi-Fi driver
 * started and tuned to a channel, and both run in their own task. Bringing the
 * driver up lazily is what keeps boot free of radio activity (spec section 9),
 * but "lazily" from two tasks at once is a race: esp_wifi_init() twice, or a
 * set_channel() from the capture session landing between an injection session's
 * ensure and its first TX.
 *
 * So the bring-up lives here, behind one lock, and both sessions go through it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the lock. Must be called before any other function here. */
esp_err_t wifi_toolbox_radio_init(void);

/* Start the driver on first use, then tune to `channel`. `channel` 0 means "leave
 * the channel alone" — used by a session that wants the radio up without moving
 * it. Idempotent for a channel already set.
 *
 * Serialised across callers: a session that asks for a different channel moves
 * the radio, which by construction also re-tunes any session that assumed the
 * old one. That is the honest behaviour — one radio, one channel — and the host
 * is responsible for not running two fixed-channel sessions at once. */
esp_err_t wifi_toolbox_radio_ensure_ready(uint8_t channel);

/* True once the driver has been started. */
bool wifi_toolbox_radio_is_ready(void);

/* The channel the radio is really on, or 0 when it is not up or the driver will
 * not say. This is the driver's answer, not a cached request: the two can differ,
 * and the difference is exactly what makes a capture come back empty. */
uint8_t wifi_toolbox_radio_get_channel(void);

/* Tune to `channel`, bringing the driver up first.
 *
 * A no-op when the radio is already there, which matters more than it looks:
 * esp_wifi_set_channel() is documented as "should not be called when STA is
 * scanning or connecting", and this co-processor associates and re-associates on
 * its own schedule because it is esp-hosted's station. Calling it redundantly
 * therefore fails intermittently for reasons that have nothing to do with the
 * request — measured: a beacon session asking for the channel it was already on
 * was refused, which is a failure introduced by the check rather than by the
 * radio.
 *
 * When the channel really is different the call is made and its answer returned,
 * because whether the radio may move is exactly what the caller needs to know. */
esp_err_t wifi_toolbox_radio_ensure_channel(uint8_t channel);

/* ---- Power save lease ---------------------------------------------------
 *
 * Why this is here and not on the host: every esp_wifi_remote_* entry point in
 * this tree is a weak UNSUPPORTED stub, esp_wifi_get_ps / esp_wifi_set_ps
 * included, so the host cannot read the current value or restore it. The policy
 * belongs where it can be observed.
 */

typedef struct {
    bool held;               /* at least one holder                             */
    uint8_t holders;         /* reference count                                 */
    bool saved;              /* `saved_type` is meaningful                      */
    uint8_t saved_type;      /* wifi_ps_type_t remembered when first taken      */
    uint8_t current_type;    /* what the radio is set to now                    */
} wifi_toolbox_ps_state_t;

/* Take the lease: on the FIRST take, remember the current value; then force power
 * save off. Reference-counted, because two independent holders exist — the host,
 * for the duration of a tool session, and the session itself. A single flag would
 * let whichever releases first restore power save while the other still needs it
 * off. */
esp_err_t wifi_toolbox_radio_ps_lease_take(bool disable_power_save);

/* Drop one reference. The remembered value is restored only when the LAST holder
 * releases, so an inner holder cannot undo an outer one. Idempotent when nothing
 * is held. Returns the driver's error if the restore itself fails, which the
 * caller must surface: "we left power save off" is the failure mode this whole
 * mechanism exists to prevent. */
esp_err_t wifi_toolbox_radio_ps_lease_release(void);

void wifi_toolbox_radio_ps_get_state(wifi_toolbox_ps_state_t *out);

/* ---- Channel hopping ----------------------------------------------------
 *
 * One hopper for the radio, not one per capture session: there is a single radio,
 * and two hoppers would fight over it. Frames carry the channel they arrived on,
 * so the host reconstructs the sequence from the stream rather than assuming it.
 */
esp_err_t wifi_toolbox_radio_hop_start(uint16_t dwell_ms);
esp_err_t wifi_toolbox_radio_hop_stop(void);
bool wifi_toolbox_radio_hop_is_running(void);

#ifdef __cplusplus
}
#endif
