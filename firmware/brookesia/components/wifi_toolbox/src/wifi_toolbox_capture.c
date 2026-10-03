/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — captured-frame classification.
 *
 * Ported from the Tactility Wi-Fi Toolbox (isEapolFrame / looksLikePmkid in
 * WifiToolbox.cpp) so the same frames are counted as interesting.
 *
 * These two predicates are the capture filter's whole decision. On this target
 * they must run on the ESP32-C6, not the host: the filter exists so that only
 * frames worth writing cross SDIO (spec section 3.3), which means the decision
 * has to happen where the frames arrive.
 *
 * Plain C with no IDF dependency, so they can be unit-tested on a host.
 */

#include <stddef.h>
#include <stdint.h>

#include "wifi_toolbox_capture.h"

/* LLC/SNAP header for EAPOL: AA AA 03 00 00 00 88 8E.
 *
 * Scanned from offset 24 (the end of the 802.11 MAC header) up to 40, because the
 * header length varies with QoS control / address-4 presence. The original does
 * the same, and the bound matters: a fixed offset misses QoS data frames, which
 * is most of what a modern AP sends. */
bool wifi_toolbox_is_eapol(const uint8_t *payload, size_t length)
{
    if ((payload == NULL) || (length < 34)) {
        return false;
    }

    for (size_t off = 24; (off + 8 <= length) && (off <= 40); ++off) {
        if ((payload[off] == 0xaa) && (payload[off + 1] == 0xaa) && (payload[off + 2] == 0x03) &&
            (payload[off + 3] == 0x00) && (payload[off + 4] == 0x00) && (payload[off + 5] == 0x00) &&
            (payload[off + 6] == 0x88) && (payload[off + 7] == 0x8e)) {
            return true;
        }
    }

    return false;
}

/* PMKID in an RSN key-data element: vendor-specific IE (0xDD) of length 20 whose
 * 16-byte body is the PMKID, and a real PMKID is not mostly zeros.
 *
 * The "at least 12 of 16 bytes non-zero" test is the original's heuristic, kept
 * verbatim: it is cheap and rejects padding without needing the full RSN parse.
 * It can in principle accept a random non-PMKID 0xDD-20 element, which is why the
 * counter is reported as "PMKID candidates" in the UI and not as fact. */
bool wifi_toolbox_looks_like_pmkid(const uint8_t *payload, size_t length)
{
    if (payload == NULL) {
        return false;
    }

    for (size_t off = 24; (off + 32 <= length) && (off <= 48); ++off) {
        if ((payload[off] == 0xdd) && (payload[off + 1] == 20)) {
            int nonzero = 0;
            for (int k = 0; k < 16; ++k) {
                if (payload[off + 4 + k] != 0) {
                    nonzero++;
                }
            }
            if (nonzero >= 12) {
                return true;
            }
        }
    }

    return false;
}

wifi_toolbox_frame_class_t wifi_toolbox_classify(const uint8_t *payload, size_t length)
{
    wifi_toolbox_frame_class_t cls = WIFI_TOOLBOX_FRAME_OTHER;

    if ((payload == NULL) || (length < 24)) {
        return WIFI_TOOLBOX_FRAME_OTHER;
    }

    if (wifi_toolbox_is_eapol(payload, length)) {
        cls |= WIFI_TOOLBOX_FRAME_EAPOL;
    }
    if (wifi_toolbox_looks_like_pmkid(payload, length)) {
        cls |= WIFI_TOOLBOX_FRAME_PMKID;
    }

    /* Frame control: type is bits 2-3 of byte 0. Deauth is subtype 0xC0, disassoc
     * 0xA0, beacon 0x80, probe request 0x40. Reading the subtype from the wrong
     * byte here is a classic off-by-one, so the mask is spelled out. */
    const uint8_t fc0 = payload[0];
    const uint8_t type = (uint8_t)((fc0 >> 2) & 0x3);
    const uint8_t subtype = (uint8_t)((fc0 >> 4) & 0xF);

    if (type == 0) {                                   /* management */
        switch (subtype) {
        case 0x0C: cls |= WIFI_TOOLBOX_FRAME_DEAUTH;   break;
        case 0x0A: cls |= WIFI_TOOLBOX_FRAME_DISASSOC; break;
        case 0x08: cls |= WIFI_TOOLBOX_FRAME_BEACON;   break;
        case 0x04: cls |= WIFI_TOOLBOX_FRAME_PROBE_REQ;break;
        default: break;
        }
    }

    return cls;
}
