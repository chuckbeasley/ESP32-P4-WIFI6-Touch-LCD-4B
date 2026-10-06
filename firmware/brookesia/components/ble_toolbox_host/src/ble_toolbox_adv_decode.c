/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Toolbox — advertisement decoding.
 *
 * Pure functions over advertisement bytes: no radio, no state, no NimBLE. The
 * Observer and AirTag screens are built on these, and keeping them separable is what
 * makes them checkable by feeding them bytes rather than by watching a screen.
 *
 * The formats decoded are the ones the Tactility app decodes (BleToolbox.cpp:
 * decodeObserverCell, isOfflineFinding, isSmartGlasses):
 *
 *   iBeacon          Apple's format: 02 01 06 1A FF 4C 00 02 15 <uuid> <major> <minor> <tx>
 *   Eddystone        Google's: 0xAAFE, then a frame type byte (0x00 UID / 0x10 URL / 0x20 TLM)
 *   Apple FindMy     Continuity type 0x12 — what AirTags and third-party trackers emit
 *   Apple Nearby     Continuity types 0x10 and 0x0F, the "Nearby Info" adverts
 *
 * AD structures are length-prefixed: [len][type][len-1 bytes]. Length 0 terminates.
 */

#include "ble_toolbox_host.h"
#include "vendor_lookup.h"

#include <stdio.h>
#include <string.h>

/* Apple's manufacturer id, little-endian on the wire. */
#define MFR_APPLE_LOW     0x4C
#define MFR_APPLE_HIGH    0x00
#define APPLE_CONTINUITY_TYPE_NEARBY   0x10
#define APPLE_CONTINUITY_TYPE_FINDMY   0x12
#define APPLE_CONTINUITY_TYPE_NEARBY2  0x0F

#define MFR_GOOGLE_LOW    0xAA
#define MFR_GOOGLE_HIGH   0xFE
#define EDDYSTONE_UID     0x00
#define EDDYSTONE_URL     0x10
#define EDDYSTONE_TLM     0x20

#define ADV_TYPE_FLAGS            0x01
#define ADV_TYPE_NAME_SHORT       0x08
#define ADV_TYPE_NAME_COMPLETE    0x09
#define ADV_TYPE_MANUFACTURER     0xFF
#define ADV_TYPE_APPEARANCE       0x19
#define ADV_TYPE_SERVICE_DATA_16  0x16

/* 16-bit service-data UUIDs, as carried in a 0x16 field. */
#define SVC_FAST_PAIR             0xFE2C
#define SVC_EXPOSURE_NOTIF        0xFD6F

/* Tile's manufacturer company ID, little-endian on the wire. */
#define MFR_TILE_LOW              0x57
#define MFR_TILE_HIGH             0x01

/* Eddystone URL scheme prefixes, per the spec. Index is the encoded byte. */
static const char *const k_eddystone_schemes[] = {
    "http://www.", "https://www.", "http://", "https://",
};

/* Eddystone URL expansion codes. Index is (encoded byte - 0x0D). */
static const char *const k_eddystone_expansions[] = {
    ".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/",
    ".com", ".org", ".edu", ".net", ".info", ".biz", ".gov",
};

const char *ble_toolbox_adv_kind_name(ble_toolbox_adv_kind_t kind)
{
    switch (kind) {
    case BLE_ADV_IBEACON:        return "iBeacon";
    case BLE_ADV_EDDYSTONE_UID:  return "Eddystone UID";
    case BLE_ADV_EDDYSTONE_URL:  return "Eddystone URL";
    case BLE_ADV_EDDYSTONE_TLM:  return "Eddystone TLM";
    case BLE_ADV_APPLE_FINDMY:   return "Apple FindMy";
    case BLE_ADV_APPLE_NEARBY:   return "Nearby Info";
    case BLE_ADV_SMART_GLASSES:  return "Eye Glasses";
    case BLE_ADV_FAST_PAIR:      return "Fast Pair";
    case BLE_ADV_EXPOSURE_NOTIFICATION: return "Exposure Notification";
    case BLE_ADV_TILE:           return "Tile";
    default:                     return "advert";
    }
}

/* The iBeacon payload sits inside a manufacturer-specific field:
 *   [len][0xFF][0x4C 0x00][0x02][0x15][16-byte uuid][major:2][minor:2][tx:1]
 * The 0x02 0x15 pair is iBeacon's own type and length. */
static bool decode_ibeacon(const uint8_t *p, uint8_t n, ble_toolbox_adv_info_t *out)
{
    if (n < 2 + 2 + 2 + 16 + 2 + 2 + 1) {
        return false;
    }
    if (p[2] != 0x02 || p[3] != 0x15) {
        return false;
    }

    memcpy(out->uuid, &p[4], 16);
    out->major = (uint16_t)((p[20] << 8) | p[21]);
    out->minor = (uint16_t)((p[22] << 8) | p[23]);
    out->tx_power = (int8_t)p[24];
    out->kind = BLE_ADV_IBEACON;
    return true;
}

/* Eddystone frames share a header: [len][0xFF][0xAA 0xFE][frame type][...]. */
static bool decode_eddystone(const uint8_t *p, uint8_t n, ble_toolbox_adv_info_t *out)
{
    if (n < 4) {
        return false;
    }

    switch (p[4]) {
    case EDDYSTONE_URL: {
        /* [frame type][tx power][scheme][encoded url...] */
        if (n < 6) {
            return false;
        }
        const uint8_t scheme = p[6];
        size_t used = 0;

        if (scheme < sizeof(k_eddystone_schemes) / sizeof(k_eddystone_schemes[0])) {
            used = (size_t)snprintf(out->url, sizeof(out->url), "%s", k_eddystone_schemes[scheme]);
        }

        for (uint8_t i = 7; i < n && used + 1 < sizeof(out->url); i++) {
            const uint8_t c = p[i];
            if (c >= 0x0D && c < 0x0D + sizeof(k_eddystone_expansions) / sizeof(k_eddystone_expansions[0])) {
                used += (size_t)snprintf(out->url + used, sizeof(out->url) - used, "%s",
                                         k_eddystone_expansions[c - 0x0D]);
            } else if (c >= 0x20 && c < 0x7F) {
                out->url[used++] = (char)c;
                out->url[used] = '\0';
            }
        }

        out->kind = BLE_ADV_EDDYSTONE_URL;
        return true;
    }

    case EDDYSTONE_TLM:
        /* [frame type][battery:2 big-endian][temperature:2 signed, 8.8 fixed] */
        if (n < 9) {
            return false;
        }
        out->battery_mv = (uint16_t)((p[5] << 8) | p[6]);
        out->temperature_c_x256 = (int16_t)((p[7] << 8) | p[8]);
        out->kind = BLE_ADV_EDDYSTONE_TLM;
        return true;

    case EDDYSTONE_UID:
        out->kind = BLE_ADV_EDDYSTONE_UID;
        return true;

    default:
        return false;
    }
}

void ble_toolbox_adv_decode(const uint8_t *data, uint8_t len, ble_toolbox_adv_info_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->kind = BLE_ADV_UNKNOWN;

    if (data == NULL) {
        return;
    }

    uint32_t flags = 0;

    for (uint8_t i = 0; i + 1 < len; ) {
        const uint8_t field_len = data[i];
        if (field_len == 0) {
            break;                                  /* end of the AD structures */
        }
        if ((uint16_t)i + 1 + field_len > len) {
            break;                                  /* truncated: stop, keep what we have */
        }

        const uint8_t type = data[i + 1];
        const uint8_t *body = &data[i + 2];
        const uint8_t body_len = (uint8_t)(field_len - 1);

        /* Classification rules are runtime data (vendor_db.bin section 5), not code.
         * A rule either assigns an immediate kind (returned true) or sets a deferred
         * flag, resolved after the walk below. */
        uint8_t kind = 0;
        if (vendor_rule_eval(type, body, body_len, &flags, &kind)) {
            switch ((ble_toolbox_adv_kind_t)kind) {
            case BLE_ADV_IBEACON:
                if (decode_ibeacon(body, body_len, out)) {
                    return;
                }
                break;
            case BLE_ADV_EDDYSTONE_UID:
            case BLE_ADV_EDDYSTONE_URL:
            case BLE_ADV_EDDYSTONE_TLM:
                if (decode_eddystone(body, body_len, out)) {
                    return;
                }
                break;
            case BLE_ADV_FAST_PAIR:
                if (body_len >= 5) {
                    out->fastpair_model_id = ((uint32_t)body[2] << 16) |
                                             ((uint32_t)body[3] << 8) |
                                             (uint32_t)body[4];
                    out->kind = BLE_ADV_FAST_PAIR;
                    return;
                }
                break;
            case BLE_ADV_EXPOSURE_NOTIFICATION:
                out->kind = BLE_ADV_EXPOSURE_NOTIFICATION;
                return;
            case BLE_ADV_TILE:
                out->kind = BLE_ADV_TILE;
                return;
            default:
                break;
            }
        }

        i = (uint8_t)(i + 1 + field_len);
    }

    /* Deferred flags: FindMy wins over the glasses hint, glasses wins over plain Nearby. */
    if (flags & (1u << 0)) {
        out->kind = BLE_ADV_APPLE_FINDMY;
        out->findmy_status = 0;
        out->findmy_separated = false;
    } else if (flags & (1u << 1)) {
        if (flags & (1u << 2)) {
            out->kind = BLE_ADV_SMART_GLASSES;
            out->reason = "Nearby Info advert with a device model";
        } else {
            out->kind = BLE_ADV_APPLE_NEARBY;
        }
    } else if (flags & (1u << 2)) {
        out->kind = BLE_ADV_SMART_GLASSES;
        out->reason = "eyewear appearance";
    }
}
