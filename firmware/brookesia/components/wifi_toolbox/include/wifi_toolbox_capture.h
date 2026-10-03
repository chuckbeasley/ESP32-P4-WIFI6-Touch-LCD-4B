/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — captured-frame classification.
 *
 * The filter decides which frames are worth sending across SDIO. Sending every
 * captured frame would waste the transport and the host's cycles on frames nobody
 * asked for (spec section 3.3), so this runs on the co-processor, in the
 * promiscuous callback, and must stay allocation-free and cheap.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_toolbox_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The classification bits live in the protocol header (wifi_toolbox_rpc.h),
 * because the host has to name them too: a capture filter is a filter_mask sent
 * over the wire, and the bits in it are the classifier's answer for one frame.
 * Declaring them on the co-processor only would have forced the host to
 * duplicate the numbering, which is exactly the drift this port keeps guarding
 * against. */

/* LLC/SNAP EAPOL (AA AA 03 00 00 00 88 8E), scanned over a small window of
 * offsets because the MAC header length varies with QoS/address-4. */
bool wifi_toolbox_is_eapol(const uint8_t *payload, size_t length);

/* Vendor-specific IE 0xDD length 20 with a mostly-non-zero 16-byte body. A
 * heuristic — see the implementation note; report it as "candidates". */
bool wifi_toolbox_looks_like_pmkid(const uint8_t *payload, size_t length);

/* Classify one captured frame. Returns WIFI_TOOLBOX_FRAME_OTHER when nothing
 * matches, so a caller can `if (cls == OTHER) return;` and drop it immediately. */
wifi_toolbox_frame_class_t wifi_toolbox_classify(const uint8_t *payload, size_t length);

#ifdef __cplusplus
}
#endif
