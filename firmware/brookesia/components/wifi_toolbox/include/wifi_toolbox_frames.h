/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — raw 802.11 frame builders.
 *
 * Ported from the Tactility Wi-Fi Toolbox (Source/app/wifitoolbox/WifiToolbox.cpp)
 * so the on-air bytes are identical for the same inputs. That matters: the
 * BrookesiaSpec verification table requires golden-byte equality against the
 * original implementation, and these frames are the unit of both capture and
 * injection.
 *
 * This code runs on the ESP32-C6 co-processor. It cannot live on the ESP32-P4
 * host: esp_wifi_80211_tx() is not carried over esp-hosted's RPC (measured:
 * both it and esp_wifi_set_promiscuous* return ESP_ERR_NOT_SUPPORTED from the
 * host — see M0-FINDINGS.md).
 *
 * Deliberately plain C with a single IDF dependency (esp_random), so the
 * builders can also be compiled on a host for the golden-byte tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The injection-mode numbering is shared with the host, so it lives in the RPC
 * header rather than being declared here. */
#include "wifi_toolbox_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest frame any builder produces: a 24-byte header + 12 fixed bytes +
 * 2 + 32 SSID IE + 6 rate IE = 76, rounded up with headroom. */
#define WIFI_TOOLBOX_FRAME_MAX   (256)

/* Maximum SSID length in an 802.11 information element. */
#define WIFI_TOOLBOX_SSID_MAX    (32)

/* Parameters shared by the builders. Only the fields a given mode needs are
 * read; the rest may be NULL / 0. */
typedef struct {
    const char *ssid;        /* SSID for the modes that carry one           */
    size_t      ssid_len;    /* 0..32; ignored when ssid is NULL            */
    uint8_t     bssid[6];    /* BSSID to spoof / target AP                  */
    uint8_t     client[6];   /* client MAC for the targeted modes           */
    uint8_t     channel;     /* channel to advertise                        */
    uint16_t    listen_interval; /* assoc-sleep only: how long we "sleep"  */
    uint16_t    duration;    /* NAV only: medium-reservation microseconds   */
} wifi_toolbox_frame_params_t;

/*
 * Builders. Each writes into `out` (at least WIFI_TOOLBOX_FRAME_MAX bytes) and
 * returns the frame length in bytes, or 0 on invalid input.
 *
 * The two-address forms take addresses in 802.11 (addr1, addr2, addr3) order
 * implied by the frame type, matching the original call sites.
 */

/* Beacon: FC 0x80, DA broadcast, SA/BSSID = bssid, fixed params
 * 0x64/0x01/0x00, SSID IE, rate tail 82 84 8b 96. */
size_t wifi_toolbox_build_beacon(uint8_t *out, const char *ssid, size_t ssid_len, const uint8_t bssid[6]);

/* Probe request: FC 0x40, DA broadcast, SA = src, BSSID broadcast, SSID IE,
 * same rate tail. */
size_t wifi_toolbox_build_probe_request(uint8_t *out, const char *ssid, size_t ssid_len, const uint8_t src[6]);

/* Probe response: FC 0x50, DA broadcast, SA/BSSID = bssid. (The original sends
 * to broadcast from the inject timer; per-station Karma needs the receive
 * path.) */
size_t wifi_toolbox_build_probe_response(uint8_t *out, const uint8_t bssid[6], const char *ssid, size_t ssid_len);

/* Deauth: FC 0xC0, addr1 = dest (or broadcast), addr2 = addr3 = bssid,
 * reason 0x0007. */
size_t wifi_toolbox_build_deauth(uint8_t *out, const uint8_t dest[6], const uint8_t bssid[6], bool bcast);

/* Disassoc: the deauth frame with subtype changed to 0xA0. */
size_t wifi_toolbox_build_disassoc(uint8_t *out, const uint8_t dest[6], const uint8_t bssid[6], bool bcast);

/* Authentication: FC 0xB0, open system, sequence 1, status 0.
 * Address order is (AP, station, BSSID) — the reverse of the deauth layout. */
size_t wifi_toolbox_build_auth(uint8_t *out, const uint8_t client[6], const uint8_t bssid[6]);

/* Association request: FC 0x00, capability ESS, caller-controlled listen
 * interval, SSID IE, rate tail. */
size_t wifi_toolbox_build_assoc_request(uint8_t *out, const uint8_t client[6], const uint8_t bssid[6],
                                        const char *ssid, size_t ssid_len, uint16_t listen_interval);

/* Client -> AP deauth (FC 0xC0): addr1 = AP, addr2 = spoofed client,
 * addr3 = bssid, reason 0x0007. */
size_t wifi_toolbox_build_client_deauth(uint8_t *out, const uint8_t client[6], const uint8_t bssid[6]);

/* NAV / Duration (FC 0x48, null-data): the Duration field holds the channel for
 * `duration` microseconds without touching anyone's association, and being a
 * non-management frame it is not protected by 802.11w. */
size_t wifi_toolbox_build_nav(uint8_t *out, const uint8_t bssid[6], uint16_t duration);

/*
 * Locally-administered, unicast, random in the last five bytes: a flood presents
 * the AP with a new station every frame without colliding with real hardware
 * addresses. Same construction as the original.
 */
void wifi_toolbox_randomise_mac(uint8_t mac[6]);

/* Build one frame for `mode`, filling in randomised addresses where the mode
 * calls for them. Returns the frame length, or 0 if the mode cannot be built
 * from the given parameters (e.g. a targeted mode with no client MAC). */
size_t wifi_toolbox_build_for_mode(uint8_t *out, wifi_toolbox_inject_mode_t mode,
                                   wifi_toolbox_frame_params_t *params);

/* Human-readable mode name, for the UI and logs. */
const char *wifi_toolbox_inject_mode_name(wifi_toolbox_inject_mode_t mode);

/* True when `mode` needs a client MAC to build. */
bool wifi_toolbox_mode_needs_client(wifi_toolbox_inject_mode_t mode);

/* True when `mode` needs an SSID to build. */
bool wifi_toolbox_mode_needs_ssid(wifi_toolbox_inject_mode_t mode);

#ifdef __cplusplus
}
#endif
