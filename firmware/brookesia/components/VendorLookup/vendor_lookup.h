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

#ifdef __cplusplus
}
#endif
