/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Vendor identification for scanned radios. The data comes from a runtime-loadable
 * database file (scripts/gen_vendor_table.py -> vendor_db.bin), so it can be updated on
 * the SD card without rebuilding the firmware. Until vendor_lookup_load() succeeds the
 * lookups return NULL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the database from a file (e.g. "/sdcard/vendor_db.bin"). Call after the
 * filesystem is mounted. Replaces any previously loaded database. */
esp_err_t vendor_lookup_load(const char *path);

/* Whether a database is currently loaded. */
bool vendor_lookup_is_loaded(void);

/* Vendor name for a Wi-Fi MAC's OUI (first three octets), or NULL. */
const char *vendor_lookup_oui(const uint8_t mac[6]);

/* Company name for a Bluetooth SIG company ID (manufacturer-specific AD), or NULL. */
const char *vendor_lookup_company(uint16_t company_id);

/* Name for a 16-bit GATT service UUID, or NULL. */
const char *vendor_lookup_service(uint16_t uuid);

/* Name for a GAP appearance value, or NULL. */
const char *vendor_lookup_appearance(uint16_t appearance);

/* Name for a Google Fast Pair 24-bit model ID, or NULL. */
const char *vendor_lookup_fastpair(uint32_t model_id);

/* Evaluate the classification rules for one AD field. Returns true and fills *kind (a
 * ble_toolbox_adv_kind_t value) when a rule assigns an immediate kind; otherwise
 * accumulates any matching flag bits into *flags and returns false. */
bool vendor_rule_eval(uint8_t ad_type, const uint8_t *body, uint8_t body_len,
                      uint32_t *flags, uint8_t *kind);

#ifdef __cplusplus
}
#endif
