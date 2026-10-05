/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Vendor identification for scanned radios: Wi-Fi MAC OUI -> vendor, Bluetooth SIG
 * company ID -> company. Backed by the tables generated from the IEEE OUI registry and
 * the Bluetooth SIG assigned-numbers list (see scripts/gen_vendor_table.py).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vendor name for a Wi-Fi MAC's OUI (first three octets), or NULL if unknown. */
const char *vendor_lookup_oui(const uint8_t mac[6]);

/* Company name for a Bluetooth SIG company ID (from the manufacturer-specific AD
 * structure), or NULL if unknown. */
const char *vendor_lookup_company(uint16_t company_id);

/* Name for a 16-bit GATT service UUID (e.g. 0x180D -> "Heart Rate"), or NULL. */
const char *vendor_lookup_service(uint16_t uuid);

/* Name for a GAP appearance value (e.g. a watch, a phone), or NULL. */
const char *vendor_lookup_appearance(uint16_t appearance);

/* Name for a Google Fast Pair 24-bit model ID (e.g. 0x0A14 -> "Galaxy Buds"), or NULL. */
const char *vendor_lookup_fastpair(uint32_t model_id);

#ifdef __cplusplus
}
#endif
