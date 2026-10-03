/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — PCAP writer, on the P4's SD card.
 *
 * Frames are captured by the co-processor and streamed up already filtered (spec
 * section 3.3); this writes what arrives to `/sdcard/captures` with size rotation
 * and a free-space floor.
 *
 * **The link type is the thing that makes or breaks every file here.** A PCAP with
 * the wrong link type opens fine and shows garbage, which reads as "the capture was
 * corrupt" rather than "the header was wrong". DLT_IEEE802_11 (105) means the
 * records are bare 802.11 MAC frames, which is exactly what the co-processor
 * streams: no preamble, no FCS record, no radiotap. Writing 127 would be the
 * tempting mistake — it is the DLT for radiotap, and this stream has no radiotap
 * header, so every frame would be parsed from the wrong offset.
 *
 * The per-frame timestamp comes from the host's own clock, not from the
 * co-processor's modem timestamp: the modem counter is a 32-bit microsecond value
 * that wraps about every 71 minutes and is only precise while modem sleep is off,
 * so presenting it as wall time would be wrong in a way that is hard to notice.
 * The modem timestamp is kept in the file's *packet* metadata only insofar as it
 * orders the stream; the PCAP header gets gettimeofday().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "wifi_toolbox_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DLT_IEEE802_11 — bare 802.11 MAC frames, which is what the slave streams. */
#define WIFI_TOOLBOX_PCAP_LINKTYPE   (105u)

typedef struct {
    char     path[96];
    uint32_t packets;
    uint64_t bytes;
    uint32_t rotations;
    bool     stopped_for_space;
} wifi_toolbox_pcap_stats_t;

/* Open a capture file. `name_hint` is a short label (the tool appends a counter
 * and a timestamp); pass NULL for a default. Creates the directory if needed. */
esp_err_t wifi_toolbox_pcap_start(const char *name_hint);

/* Append one frame. Returns ESP_OK, or an error when the file could not be
 * written — the caller should stop the session rather than pretend the capture
 * continues (a capture that silently stops writing is the failure mode the spec's
 * transport warning exists for). */
esp_err_t wifi_toolbox_pcap_write(const wifi_toolbox_frame_evt_t *hdr, const uint8_t *payload);

/* Flush and close. Safe to call when nothing is open. */
void wifi_toolbox_pcap_stop(void);

bool wifi_toolbox_pcap_is_open(void);
void wifi_toolbox_pcap_get_stats(wifi_toolbox_pcap_stats_t *out);

/* List the capture files on the card, newest first.
 *
 * `out` is a flat NUL-separated buffer of names. Returns the number written, or
 * -1 on error. Kept as a C-friendly shape because the UI builds an LVGL list from
 * it directly. */
int wifi_toolbox_pcap_list(char *out, size_t out_len, int max_names);

#ifdef __cplusplus
}
#endif
