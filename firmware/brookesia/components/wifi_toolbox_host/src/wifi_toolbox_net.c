/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — net utilities. See wifi_toolbox_net.h.
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "wifi_toolbox_net.h"

static const char *TAG = "tb_net";

#define SCAN_TASK_STACK     (4096)
#define SCAN_TASK_PRIO      (4)
#define SCAN_DEFAULT_TIMEOUT_MS (400)
/* Upper bound on a sweep so a mistyped prefix cannot turn into an unbounded run. */
#define SCAN_MAX_HOSTS      (1024)

/* Preset tables. The ports are the ones people actually look for on a home or
 * small-office network; a full 1-65535 sweep is deliberately absent, because at
 * any sane timeout it would take hours and the UI would have to pretend to be
 * busy for all of them. */
static const wifi_toolbox_preset_t k_presets[WIFI_TOOLBOX_PRESET_COUNT] = {
    [WIFI_TOOLBOX_PRESET_COMMON]    = { .label = "Common",      .first = 20,    .last = 445   },
    [WIFI_TOOLBOX_PRESET_WEB]       = { .label = "Web",         .first = 80,    .last = 8443  },
    [WIFI_TOOLBOX_PRESET_IOT_SCADA] = { .label = "IoT/SCADA",   .first = 1883,  .last = 9100  },
    [WIFI_TOOLBOX_PRESET_WINDOWS_SMB] = { .label = "Windows/SMB", .first = 135, .last = 5985 },
};

/* A label for the ports a person recognises. Anything else is reported by number,
 * which is honest: inventing a service name would be worse than "-". */
static const char *service_for_port(uint16_t port)
{
    switch (port) {
    case 20:   return "ftp-data";
    case 21:   return "ftp";
    case 22:   return "ssh";
    case 23:   return "telnet";
    case 25:   return "smtp";
    case 53:   return "dns";
    case 80:   return "http";
    case 110:  return "pop3";
    case 135:  return "msrpc";
    case 139:  return "netbios";
    case 143:  return "imap";
    case 443:  return "https";
    case 445:  return "smb";
    case 554:  return "rtsp";
    case 1883: return "mqtt";
    case 1900: return "ssdp";
    case 3389: return "rdp";
    case 5000: return "upnp";
    case 5985: return "winrm";
    case 8000: return "http-alt";
    case 8080: return "http-proxy";
    case 8443: return "https-alt";
    case 8883: return "mqtts";
    case 9100: return "jetdirect";
    case 62078: return "iphone-sync";
    default:   return NULL;
    }
}

const wifi_toolbox_preset_t *wifi_toolbox_preset(wifi_toolbox_port_preset_t preset)
{
    if ((preset < 0) || (preset >= WIFI_TOOLBOX_PRESET_COUNT)) {
        return NULL;
    }

    return &k_presets[preset];
}

const char *wifi_toolbox_preset_name(wifi_toolbox_port_preset_t preset)
{
    const wifi_toolbox_preset_t *p = wifi_toolbox_preset(preset);

    return (p != NULL) ? p->label : "?";
}

const char *wifi_toolbox_scan_mode_name(wifi_toolbox_scan_mode_t mode)
{
    switch (mode) {
    case WIFI_TOOLBOX_SCAN_ARP:         return "host sweep";
    case WIFI_TOOLBOX_SCAN_SSH:         return "ssh";
    case WIFI_TOOLBOX_SCAN_TELNET:      return "telnet";
    case WIFI_TOOLBOX_SCAN_PORT_PRESET: return "port scan";
    default:                            return "?";
    }
}

bool wifi_toolbox_local_ipv4(char *out, size_t out_len, uint8_t *prefix_len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }

    if (ip.ip.addr == 0) {
        return false;
    }

    if (out != NULL) {
        snprintf(out, out_len, IPSTR, IP2STR(&ip.ip));
    }

    if (prefix_len != NULL) {
        *prefix_len = 24;                  /* lwIP's default for a DHCP lease */
    }

    return true;
}

static struct {
    volatile bool                    running;
    volatile bool                    cancel;
    TaskHandle_t                     task;
    wifi_toolbox_scan_stats_t        stats;
    wifi_toolbox_scan_callbacks_t    cb;
    wifi_toolbox_scan_request_t      req;
} s_scan;

/* One TCP connect with a bounded wait.
 *
 * Ports the device itself is listening on would be a false positive if this only
 * tested "can I connect", so the caller is scanning a *target*, and the answer is
 * that target's. Nothing here binds a local port, so it cannot interfere with the
 * app's own listeners. */
static bool probe_tcp_port(uint32_t ip_be, uint16_t port, int timeout_ms)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip_be;

    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Non-blocking so the timeout is ours rather than the stack's: a blocking
     * connect to a filtered port can wait for the full SYN retry schedule, which
     * is tens of seconds. */
    const int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    bool open = false;
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    if (rc == 0) {
        open = true;
    } else if (errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);

        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        rc = select(sock + 1, NULL, &wset, NULL, &tv);
        if (rc > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);

            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0) {
                open = (so_error == 0);
            }
        }
    }

    close(sock);

    return open;
}

static void report_progress(void)
{
    if (s_scan.cb.on_progress != NULL) {
        s_scan.cb.on_progress(&s_scan.stats, s_scan.cb.ctx);
    }
}

static void scan_host_ports(uint32_t ip_be, const char *ip_text)
{
    uint16_t first = 0;
    uint16_t last = 0;

    switch (s_scan.req.mode) {
    case WIFI_TOOLBOX_SCAN_SSH:
        first = last = 22;
        break;
    case WIFI_TOOLBOX_SCAN_TELNET:
        first = last = 23;
        break;
    case WIFI_TOOLBOX_SCAN_PORT_PRESET: {
        const wifi_toolbox_preset_t *p = wifi_toolbox_preset(s_scan.req.preset);
        if (p == NULL) {
            return;
        }
        first = p->first;
        last = p->last;
        break;
    }
    default:
        return;
    }

    for (uint16_t port = first; (port <= last) && !s_scan.cancel; port++) {
        /* The presets are ranges, but the interesting ports inside them are
         * sparse — probing every number from 20 to 445 would be 426 attempts for
         * about a dozen answers, and at any usable timeout that is a scan nobody
         * will wait for. So a preset means "the named ports in this range"; the
         * bounds exist to make the set's intent readable, not to be enumerated. */
        const char *svc = service_for_port(port);
        if (svc == NULL) {
            continue;
        }

        s_scan.stats.scanned++;

        if (probe_tcp_port(ip_be, port, s_scan.req.timeout_ms)) {
            s_scan.stats.found++;
            if (s_scan.cb.on_port != NULL) {
                s_scan.cb.on_port(ip_be, port, svc, s_scan.cb.ctx);
            }
        } else {
            s_scan.stats.failed++;
        }
    }

    (void)ip_text;
}

static void scan_subnet(void)
{
    char local[32] = {0};
    uint8_t prefix = 24;

    if (!wifi_toolbox_local_ipv4(local, sizeof(local), &prefix)) {
        ESP_LOGW(TAG, "host sweep needs an IPv4 address; the station is not connected");
        if (s_scan.cb.on_done != NULL) {
            s_scan.cb.on_done(ESP_ERR_INVALID_STATE, &s_scan.stats, s_scan.cb.ctx);
        }
        return;
    }

    /* Sweep the /24 the device is on. A /24 is what a home network is; a wider
     * prefix would be SCAN_MAX_HOSTS hosts of waiting. */
    struct in_addr a;
    if (inet_aton(local, &a) != 1) {
        if (s_scan.cb.on_done != NULL) {
            s_scan.cb.on_done(ESP_ERR_INVALID_ARG, &s_scan.stats, s_scan.cb.ctx);
        }
        return;
    }

    const uint32_t base = ntohl(a.s_addr) & 0xFFFFFF00u;

    for (uint32_t host = 1; (host <= 254) && !s_scan.cancel; host++) {
        const uint32_t ip_host = base | host;
        const uint32_t ip_be = htonl(ip_host);

        s_scan.stats.scanned++;

        /* "Answers" means an open TCP port: ICMP echo needs raw sockets, and the
         * three ports below cover the overwhelming majority of devices that are
         * up. A host that answers none of them is not reported, which is the
         * honest outcome rather than a guess. */
        bool found = false;
        static const uint16_t probe_ports[] = { 80, 443, 22 };

        for (size_t i = 0; i < sizeof(probe_ports) / sizeof(probe_ports[0]) && !found; i++) {
            if (probe_tcp_port(ip_be, probe_ports[i], s_scan.req.timeout_ms)) {
                found = true;
            }
        }

        if (found) {
            char text[24];
            struct in_addr ia;
            ia.s_addr = ip_be;
            snprintf(text, sizeof(text), "%s", inet_ntoa(ia));

            s_scan.stats.found++;
            if (s_scan.cb.on_host != NULL) {
                s_scan.cb.on_host(ip_be, text, s_scan.cb.ctx);
            }
        } else {
            s_scan.stats.failed++;
        }

        if ((host % 16) == 0) {
            report_progress();
        }
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    if (s_scan.req.mode == WIFI_TOOLBOX_SCAN_ARP) {
        scan_subnet();
    } else {
        struct in_addr a;
        if (inet_aton(s_scan.req.target, &a) != 1) {
            ESP_LOGE(TAG, "target '%s' is not an IPv4 address", s_scan.req.target);
            if (s_scan.cb.on_done != NULL) {
                s_scan.cb.on_done(ESP_ERR_INVALID_ARG, &s_scan.stats, s_scan.cb.ctx);
            }
            s_scan.running = false;
            s_scan.task = NULL;
            vTaskDelete(NULL);
            return;
        }
        scan_host_ports(a.s_addr, s_scan.req.target);
    }

    const bool cancelled = s_scan.cancel;
    s_scan.running = false;

    if (s_scan.cb.on_done != NULL) {
        s_scan.cb.on_done(cancelled ? ESP_ERR_INVALID_STATE : ESP_OK, &s_scan.stats, s_scan.cb.ctx);
    }

    s_scan.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_toolbox_scan_start(const wifi_toolbox_scan_request_t *req,
                                  const wifi_toolbox_scan_callbacks_t *cb)
{
    if ((req == NULL) || (cb == NULL) || (cb->on_done == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_scan.running) {
        return ESP_ERR_INVALID_STATE;
    }

    if ((req->mode != WIFI_TOOLBOX_SCAN_ARP) && (req->target[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_scan.stats, 0, sizeof(s_scan.stats));
    s_scan.cb = *cb;
    s_scan.req = *req;
    if (s_scan.req.timeout_ms == 0) {
        s_scan.req.timeout_ms = SCAN_DEFAULT_TIMEOUT_MS;
    }
    s_scan.cancel = false;
    s_scan.running = true;

    if (xTaskCreate(scan_task, "tb_scan", SCAN_TASK_STACK, NULL, SCAN_TASK_PRIO, &s_scan.task) != pdPASS) {
        s_scan.running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "scan started: %s%s%s (timeout %ums)",
             wifi_toolbox_scan_mode_name(req->mode),
             (req->mode == WIFI_TOOLBOX_SCAN_PORT_PRESET) ? " " : "",
             (req->mode == WIFI_TOOLBOX_SCAN_PORT_PRESET) ? wifi_toolbox_preset_name(req->preset) : "",
             (unsigned)s_scan.req.timeout_ms);

    return ESP_OK;
}

void wifi_toolbox_scan_stop(void)
{
    if (!s_scan.running) {
        return;
    }

    s_scan.cancel = true;

    /* Bounded: the sweep checks the flag between attempts, so this returns within
     * one timeout. Not waited on to completion here because a caller cancelling
     * usually wants its thread back. */
    for (int i = 0; (i < 50) && s_scan.running; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool wifi_toolbox_scan_is_running(void)
{
    return s_scan.running;
}
