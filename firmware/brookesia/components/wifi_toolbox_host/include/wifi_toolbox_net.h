/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — net utilities.
 *
 * Plain lwIP sockets on the host, no radio involvement: this is the one tool of
 * the three that does not care that the Wi-Fi lives on another chip (spec section
 * 1). It does need the station to be connected, which the caller checks.
 *
 * The scan runs in its own task and reports through a callback, because a sweep of
 * 192 addresses at a few hundred milliseconds each is seconds of work — doing it
 * inline would block whichever task asked, and on this target that task is the
 * UI's.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_TOOLBOX_SCAN_ARP = 0,      /* who answers on the subnet                 */
    WIFI_TOOLBOX_SCAN_SSH,          /* port 22 on a target                       */
    WIFI_TOOLBOX_SCAN_TELNET,       /* port 23 on a target                       */
    WIFI_TOOLBOX_SCAN_PORT_PRESET,  /* one of the presets below, on a target      */
} wifi_toolbox_scan_mode_t;

typedef enum {
    WIFI_TOOLBOX_PRESET_COMMON = 0,
    WIFI_TOOLBOX_PRESET_WEB,
    WIFI_TOOLBOX_PRESET_IOT_SCADA,
    WIFI_TOOLBOX_PRESET_WINDOWS_SMB,
    WIFI_TOOLBOX_PRESET_COUNT,
} wifi_toolbox_port_preset_t;

typedef struct {
    char     label[24];
    uint16_t first;
    uint16_t last;
} wifi_toolbox_preset_t;

/* The presets, as a table the UI can list without knowing the ports. */
const wifi_toolbox_preset_t *wifi_toolbox_preset(wifi_toolbox_port_preset_t preset);
const char *wifi_toolbox_preset_name(wifi_toolbox_port_preset_t preset);
const char *wifi_toolbox_scan_mode_name(wifi_toolbox_scan_mode_t mode);

typedef struct {
    uint32_t scanned;        /* hosts, or ports, depending on the mode     */
    uint32_t found;
    uint32_t failed;         /* timeouts / unreachable; not an error       */
    bool     running;
} wifi_toolbox_scan_stats_t;

typedef struct {
    /* A host answered (ARP mode): `port` is 0. */
    void (*on_host)(uint32_t ip, const char *ip_text, void *ctx);
    /* A port is open. */
    void (*on_port)(uint32_t ip, uint16_t port, const char *service, void *ctx);
    /* Progress, roughly once per host. */
    void (*on_progress)(const wifi_toolbox_scan_stats_t *stats, void *ctx);
    /* Finished, cancelled, or failed. `err` is ESP_OK for a completed sweep. */
    void (*on_done)(esp_err_t err, const wifi_toolbox_scan_stats_t *stats, void *ctx);
    void  *ctx;
} wifi_toolbox_scan_callbacks_t;

typedef struct {
    wifi_toolbox_scan_mode_t     mode;
    wifi_toolbox_port_preset_t   preset;
    char                         target[64];   /* "192.168.1.10" or "host"   */
    uint16_t                     timeout_ms;   /* per attempt; 0 = default   */
} wifi_toolbox_scan_request_t;

esp_err_t wifi_toolbox_scan_start(const wifi_toolbox_scan_request_t *req,
                                  const wifi_toolbox_scan_callbacks_t *cb);
void      wifi_toolbox_scan_stop(void);
bool      wifi_toolbox_scan_is_running(void);

/* The device's own IPv4 address and subnet, for the ARP mode's default range and
 * for the "scan myself" case. Returns false when not connected. */
bool wifi_toolbox_local_ipv4(char *out, size_t out_len, uint8_t *prefix_len);

#ifdef __cplusplus
}
#endif
