/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fieldwatch signature ingestion. Loads Fieldwatch's own fieldwatch-signatures-v2.json
 * at runtime and matches BLE advertisements against its fleet catalog, so the BLE
 * Toolbox can label radios with Fieldwatch's device names -- no rebuild, no conversion.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the signature catalog from a JSON file (Fieldwatch export / dist). Replaces any
 * previously loaded catalog. */
esp_err_t vendor_signature_load(const char *path);

/* Whether a catalog is currently loaded. */
bool vendor_signature_is_loaded(void);

/* One decoded BLE advertisement, as the matcher consumes it. */
typedef struct {
    const uint8_t *mac;        /* 6 bytes, BLE address (required) */
    const char *name;          /* advertised name, NULL/empty = none */
    uint16_t company_id;       /* manufacturer company ID, 0 = none */
    const char *mfg_hex;       /* manufacturer data as uppercase hex, NULL/empty = none */
    uint16_t svc_uuid;         /* 16-bit service UUID, 0 = none */
    const char *svc_hex;       /* service data as uppercase hex, NULL/empty = none */
} ble_signature_input_t;

/* Fieldwatch SignatureClass (the subset we label with). */
typedef enum {
    SIG_CLASS_FINDER = 0, SIG_CLASS_BEACON, SIG_CLASS_SIGNAGE, SIG_CLASS_WEARABLE,
    SIG_CLASS_SURVEILLANCE, SIG_CLASS_DRONE, SIG_CLASS_HACKING, SIG_CLASS_BODYWORN,
    SIG_CLASS_LAW_ENFORCEMENT, SIG_CLASS_VEHICLE, SIG_CLASS_GLASSES, SIG_CLASS_AUDIO,
    SIG_CLASS_CAMERA, SIG_CLASS_THERMOSTAT, SIG_CLASS_LOCK, SIG_CLASS_HEALTH,
    SIG_CLASS_HOME, SIG_CLASS_ISP, SIG_CLASS_MESH, SIG_CLASS_PHONE, SIG_CLASS_OTHER,
    SIG_CLASS_UNKNOWN,
} ble_signature_class_t;

/* Match one advertisement. Returns the best fleet name (a pointer into the catalog, so
 * valid until the next load), or NULL when nothing matches. If class_out is non-NULL and
 * a fleet matched, *class_out receives its SignatureClass. */
const char *vendor_signature_match(const ble_signature_input_t *in,
                                   ble_signature_class_t *class_out);

/* Human label for a SignatureClass, or NULL. */
const char *vendor_signature_class_label(ble_signature_class_t cls);

#ifdef __cplusplus
}
#endif
