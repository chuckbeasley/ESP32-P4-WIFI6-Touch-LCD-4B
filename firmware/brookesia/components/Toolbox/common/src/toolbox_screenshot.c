/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screenshot web server — a development aid, not a product feature.
 *
 * *** Why this exists ***
 *
 * Three faults in a row were reported from the glass — a crash, then two empty lists —
 * and each time the only evidence available was a serial log and whatever the tester
 * described. Several rounds were spent fixing real faults that could not be confirmed as
 * the reported one, because "no devices on screen" is indistinguishable from the outside
 * across four very different causes.
 *
 * esp-brookesia 0.4.2 has no web server or screenshot facility (its src/ is core, systems,
 * widgets, assets), so this provides the missing half: the board is already on Wi-Fi with
 * an IP, and LVGL's snapshot module is already compiled in (CONFIG_LV_USE_SNAPSHOT=y).
 * Together those are enough to serve the actual framebuffer over HTTP.
 *
 * *** PNG only ***
 *
 * This served 24-bit BMP until the format itself became a problem: the browser had to be
 * pointed at a file, and a partial transfer left an unreadable file with no way to tell it
 * from a complete one. PNG is what every consumer of this actually wants, so BMP was removed
 * rather than kept alongside — one format, no negotiation, no /screen.bmp.
 *
 * There is no deflate compressor in this IDF (the ROM exports tinfl_decompress and mz_* for
 * decompression only) and no zlib component, so the PNG is written with STORED deflate blocks:
 * valid, universally readable, and larger than a compressed PNG would be. That is an accepted
 * cost. The alternative — hand-rolling LZ77 and fixed Huffman codes — is a great deal of code
 * whose failure mode is a subtly wrong image, which is worse than a large correct one.
 *
 * *** Deliberately not a feature ***
 *
 * This exposes the screen to anyone on the network with no authentication. It is a bench
 * aid, and CONFIG_TOOLBOX_SCREENSHOT_SERVER defaults to n so a shipped build does not
 * contain it at all. It is compiled out entirely when disabled, not merely switched off.
 */

#include "toolbox_screenshot.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"   /* the system LVGL lock: see the note in capture() */
#include "lvgl.h"

/* LVGL's include root is the component directory, not src/, so the snapshot header is
 * reached by its path within the tree rather than as "lv_snapshot.h". lvgl.h does pull it
 * in itself, but relying on that would depend on an unrelated header's internals and break
 * silently if LV_USE_SNAPSHOT were ever turned off. */
#include "src/others/snapshot/lv_snapshot.h"

#if CONFIG_TOOLBOX_SCREENSHOT_SERVER

#include "esp_http_server.h"

static const char *TAG = "screenshot";

/* One capture, guarded so the HTTP worker and the capture task cannot both hold it. */
static SemaphoreHandle_t s_lock;      /* guards the capture buffer */
static SemaphoreHandle_t s_tx_lock;   /* serialises HTTP responses: one transfer at a time */
static uint8_t          *s_png;
static size_t            s_png_len;
static size_t            s_png_cap;
static int               s_width;
static int               s_height;
static uint32_t          s_captures;

/* ---- PNG primitives ------------------------------------------------------ */

static uint32_t crc_table[256];

static void crc_table_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    for (size_t n = 0; n < len; n++) {
        crc = crc_table[(crc ^ buf[n]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static uint32_t adler32(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    /* 5552 is the largest run that cannot overflow the 32-bit accumulator. */
    while (len > 0) {
        const size_t chunk = (len < 5552) ? len : 5552;
        for (size_t i = 0; i < chunk; i++) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        data += chunk;
        len -= chunk;
    }
    return (b << 16) | a;
}

/* Checks the checksum against published vectors before any PNG is built from it.
 *
 * The first version of this encoder produced a structurally perfect zlib stream - 24 stored
 * blocks, correct LEN/NLEN, exactly 1555920 payload bytes, first pixel the expected bezel colour -
 * and a file that every decoder rejected with "incorrect data check". The deflate half was right
 * and the Adler-32 was wrong, and that is not something visible from the outside: the PNG's chunk
 * CRCs were fine, the block structure was fine, and the only symptom was a decoder refusing the
 * image.
 *
 * With these two vectors the failure is caught here, where it can be read, instead of surfacing as
 * a corrupt file. */
static bool checksum_selfcheck(void)
{
    static const uint8_t v1[] = "Wikipedia";
    const uint32_t a1 = adler32(v1, sizeof(v1) - 1);
    if (a1 != 0x11E60398u) {
        ESP_LOGE(TAG, "adler32 self-check failed: %08lx != 11e60398", (unsigned long)a1);
        return false;
    }

    return true;
}

static size_t be32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
    return 4;
}

/* A PNG chunk: length, type, payload, CRC over type+payload. */
static size_t chunk(uint8_t *out, const char type[4], const uint8_t *data, size_t len)
{
    size_t n = be32(out, (uint32_t)len);
    memcpy(out + n, type, 4);
    n += 4;
    if (data != NULL && len > 0) {
        memcpy(out + n, data, len);
    }
    uint32_t crc = crc_update(0xFFFFFFFFu, out + 4, len + 4) ^ 0xFFFFFFFFu;
    n += len;
    n += be32(out + n, crc);
    return n;
}

/* ---- Capture ------------------------------------------------------------- */

/* Encodes the snapshot as PNG: 8-bit truecolour, one filter byte per row.
 *
 * Everything is allocated and encoded BEFORE the lock is taken wherever possible, but the
 * snapshot itself has to be taken under it. What is deliberately NOT done under the lock is
 * the transmission - see screen_get(). */
static esp_err_t capture(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_FAIL;

    /* The ADAPTER's lock, not bsp_display_lock.
     *
     * This is the system's actual LVGL lock — the framework's own display task takes it on
     * every tick. It is a RECURSIVE mutex (xSemaphoreTakeRecursive in esp_lv_adapter.c), so a
     * same-task re-entry is fine and cannot deadlock; what it can do is block, which is why
     * the timeout is short and why nothing slow happens while holding it.
     *
     * A capture is worth dropping rather than stalling the display task behind it. */
    if (esp_lv_adapter_lock(500) != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_image_dsc_t *shot = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);

    if (shot == NULL) {
        esp_lv_adapter_unlock();
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "snapshot failed (out of memory?)");
        return ESP_FAIL;
    }

    const int w = shot->header.w;
    const int h = shot->header.h;

    /* Downscaled before encoding, and this is a fix rather than a size optimisation.
     *
     * Full resolution means 1.5 MB of PNG plus a 1 MB snapshot plus 1.5 MB of scratch, and the
     * encode walks all of it through PSRAM. Measured, that starves the esp-hosted SDIO task: the
     * co-processor stops answering feature RPCs ("request: no response uid=67 msg_id=294 (5000 ms)"
     * every 7 s, msg 294 being WifiStaGetApInfo), a 1.5 MB transfer stalls part-way, and the server
     * then cannot accept any further TCP connection ("Failed to connect", 21 s) while ICMP still
     * answers in 0 ms. The radio is fine; the host is too busy to talk to it.
     *
     * SCALE 2 gives a 360x360 image - a quarter of the pixels and a sixteenth of the encode work -
     * which is enough to read the UI, and small enough that the transfer does not monopolise the
     * link. It is not a compression trade: the whole capture path shrinks with it. */
    #define SCREENSHOT_SCALE 2

    const int ow = (w + SCREENSHOT_SCALE - 1) / SCREENSHOT_SCALE;
    const int oh = (h + SCREENSHOT_SCALE - 1) / SCREENSHOT_SCALE;

    const size_t stride = 1 + ((size_t)ow * 3);
    const size_t raw_len = stride * (size_t)oh;

    /* Sized generously rather than derived. I mis-computed this four times - the Adler-32 trailer,
     * then the stored-deflate block count three ways (24, 26 and 32 predicted against 24 actual) -
     * and each wrong guess wrote past the buffer and corrupted the heap rather than failing
     * visibly. 1/16th plus 1 KB is far more than the measured overhead (under 1%: 5 bytes per
     * 65535-byte block, plus 2 zlib bytes and a 4 byte checksum), and the bounds checks below still
     * guard every write, so an error reports itself instead of overrunning. */
    const size_t zlib_cap = raw_len + (raw_len / 16) + 1024;

    /* 8 signature + 13 IHDR + 12 IDAT header + zlib stream + 12 IEND, plus slack. */
    const size_t need = 8 + 25 + (12 + zlib_cap) + 12 + 64;

    if (need > s_png_cap) {
        uint8_t *bigger = (uint8_t *)heap_caps_realloc(s_png, need, MALLOC_CAP_SPIRAM);
        if (bigger == NULL) {
            bigger = (uint8_t *)heap_caps_realloc(s_png, need, MALLOC_CAP_8BIT);
        }
        if (bigger == NULL) {
            lv_snapshot_free(shot);
            esp_lv_adapter_unlock();
            xSemaphoreGive(s_lock);
            ESP_LOGE(TAG, "no room for a %dx%d capture (%u bytes)", w, h, (unsigned)need);
            return ESP_ERR_NO_MEM;
        }
        s_png = bigger;
        s_png_cap = need;
    }

    /* --- build the zlib stream --- */
    uint8_t *z = (uint8_t *)heap_caps_malloc(zlib_cap, MALLOC_CAP_SPIRAM);
    if (z == NULL) {
        z = (uint8_t *)heap_caps_malloc(zlib_cap, MALLOC_CAP_8BIT);
    }
    if (z == NULL) {
        lv_snapshot_free(shot);
        esp_lv_adapter_unlock();
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "no room for the zlib stream (%u bytes)", (unsigned)zlib_cap);
        return ESP_ERR_NO_MEM;
    }

    size_t zi = 0;

    /* Adler-32 of the UNCOMPRESSED data, accumulated as it is written.
     *
     * Not computed afterwards over a contiguous range, because there is no such range: the
     * uncompressed bytes go into the buffer interleaved with block headers. The first version
     * checksummed the deflate stream instead - z[2 .. zi] - which the device reported as
     * 5664692b, and zlib confirms is exactly adler32 of the compressed bytes. Every decoder
     * rejected the file with "incorrect data check" while the PNG structure, the CRCs and the
     * block layout were all correct. */
    uint32_t ad_a = 1, ad_b = 0;
    size_t   ad_since = 0;              /* bytes since the last modular reduction */
    z[zi++] = 0x78;                     /* zlib: deflate, 32K window */
    z[zi++] = 0x01;                     /* no preset dictionary, fastest */

    {
        /* One deflate block per group of whole rows.
         *
         * Each row is exactly `stride` bytes: one filter byte then w pixels of three bytes. A row
         * is therefore an indivisible unit of work, and confining blocks to row boundaries removes
         * the case that produced four broken versions of this loop - a block ending mid-row, where
         * the "fill the block" loop could exit without having advanced a pixel and then spin
         * forever writing filter bytes until the buffer was full.
         *
         * 65535 / 2161 = 30 rows fit in a stored block, so this costs about 24 blocks for a 720px
         * frame - the same as packing them tightly, with no partial-row arithmetic at all. */
        const size_t rows_per_block = 65535 / stride;
        if (rows_per_block == 0) {
            ESP_LOGE(TAG, "row of %u bytes does not fit a stored deflate block", (unsigned)stride);
            err = ESP_FAIL;
            goto fail_z;
        }

        size_t y = 0;                  /* OUTPUT row; source row is y * SCALE */
        while (y < (size_t)oh) {
            size_t rows = (size_t)oh - y;
            if (rows > rows_per_block) {
                rows = rows_per_block;
            }
            const size_t take = rows * stride;
            const int final = (y + rows >= (size_t)oh) ? 1 : 0;

            if (zi + 5 > zlib_cap) {
                ESP_LOGE(TAG, "zlib overflow at block header: zi=%u cap=%u",
                         (unsigned)zi, (unsigned)zlib_cap);
                err = ESP_FAIL;
                goto fail_z;
            }

            z[zi++] = (uint8_t)final;   /* BFINAL, BTYPE=00 (stored) */
            z[zi++] = (uint8_t)(take & 0xFF);
            z[zi++] = (uint8_t)((take >> 8) & 0xFF);
            z[zi++] = (uint8_t)((~take & 0xFF));
            z[zi++] = (uint8_t)(((~take) >> 8) & 0xFF);

            for (size_t r = 0; r < rows; r++, y++) {
                if (zi + stride > zlib_cap) {
                    ESP_LOGE(TAG, "zlib overflow at row %u: zi=%u cap=%u need=%u",
                             (unsigned)y, (unsigned)zi, (unsigned)zlib_cap, (unsigned)stride);
                    err = ESP_FAIL;
                    goto fail_z;
                }

                z[zi++] = 0;    /* filter type 0: none */
                ad_b += ad_a;   /* the filter byte is part of the uncompressed data */
                if (++ad_since >= 256) { ad_a %= 65521; ad_b %= 65521; ad_since = 0; }

                /* Nearest-neighbour: one source pixel per output pixel, taken on a grid.
                 * Cheap on purpose - the point is to do a sixteenth of the memory work, so a
                 * filter here would defeat it. */
                const lv_color_t *line = (const lv_color_t *)shot->data
                                       + ((y * SCREENSHOT_SCALE) * (size_t)w);
                for (size_t px = 0; px < (size_t)ow; px++) {
                    const uint16_t c = lv_color_to16(line[px * SCREENSHOT_SCALE]);
                    const uint8_t r5 = (c >> 11) & 0x1F;
                    const uint8_t g6 = (c >> 5) & 0x3F;
                    const uint8_t b5 = c & 0x1F;

                    const uint8_t rb = (uint8_t)((r5 << 3) | (r5 >> 2));
                    const uint8_t gb = (uint8_t)((g6 << 2) | (g6 >> 4));
                    const uint8_t bb = (uint8_t)((b5 << 3) | (b5 >> 2));
                    z[zi++] = rb;
                    z[zi++] = gb;
                    z[zi++] = bb;
                    ad_b += (ad_a += rb);
                    ad_b += (ad_a += gb);
                    ad_b += (ad_a += bb);
                    ad_since += 3;
                    if (ad_since >= 256) { ad_a %= 65521; ad_b %= 65521; ad_since = 0; }
                }
            }
        }
    }

    /* What the encoder actually produced, against the budget it was given. The bounds checks
     * compare against `cap`, so this is also how a sizing mistake becomes visible as a number
     * rather than as a repeat of a fault that was already fixed. */
    /* Adler-32 covers the uncompressed payload: everything after the 2 byte zlib header, written
     * but not including the 4 byte checksum itself. */
    ad_a %= 65521;
    ad_b %= 65521;
    be32(z + zi, (ad_b << 16) | ad_a);
    zi += 4;

    /* --- assemble the file --- */
    size_t o = 0;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(s_png, sig, sizeof(sig));
    o += sizeof(sig);

    uint8_t ihdr[13];
    size_t ih = 0;
    ih += be32(ihdr + ih, (uint32_t)ow);   /* the OUTPUT size, not the screen size */
    ih += be32(ihdr + ih, (uint32_t)oh);
    ihdr[ih++] = 8;                     /* bit depth */
    ihdr[ih++] = 2;                     /* colour type 2: truecolour */
    ihdr[ih++] = 0;                     /* deflate */
    ihdr[ih++] = 0;                     /* adaptive filtering */
    ihdr[ih++] = 0;                     /* no interlace */
    o += chunk(s_png + o, "IHDR", ihdr, ih);

    o += chunk(s_png + o, "IDAT", z, zi);
    o += chunk(s_png + o, "IEND", NULL, 0);

    if (o > s_png_cap) {
        /* Cannot happen with the capacity computed above, which is the point of checking: a
         * silent overrun here is what corrupted the heap and took the board down on free(). */
        ESP_LOGE(TAG, "png overflow: wrote %u into %u", (unsigned)o, (unsigned)s_png_cap);
        err = ESP_FAIL;
        goto fail_z;
    }

    heap_caps_free(z);

    s_png_len = o;
    s_width = ow;
    s_height = oh;
    s_captures++;
    err = ESP_OK;

    lv_snapshot_free(shot);
    esp_lv_adapter_unlock();
    xSemaphoreGive(s_lock);

    return err;

fail_z:
    heap_caps_free(z);
    lv_snapshot_free(shot);
    esp_lv_adapter_unlock();
    xSemaphoreGive(s_lock);
    return err;
}

/* ---- HTTP ---------------------------------------------------------------- */

static esp_err_t root_get(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><meta charset=utf-8>"
        "<title>toolbox screen</title>"
        "<style>body{margin:0;background:#111;color:#ddd;font:14px system-ui}"
        "img{display:block;max-width:100vw;image-rendering:pixelated}"
        "p{margin:6px 10px}</style>"
        "<p>refreshes every 2 s &middot; <a style=color:#8cf href=/screen.png>/screen.png</a>"
        " &middot; <span id=s>?</span></p>"
        "<img id=i src=/screen.png>"
        "<script>"
        "i.onload=()=>{s.textContent=new Date().toLocaleTimeString()};"
        "setInterval(()=>{i.src='/screen.png?t='+Date.now()},2000);"
        "</script>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t screen_get(httpd_req_t *req)
{
    /* A fresh capture per request, and the lock is released BEFORE anything is transmitted.
     *
     * That ordering is the fix for the hang this replaces. Previously the handler held the
     * capture mutex across httpd_resp_send() of a 1.5 MB body, so a slow or half-open client
     * kept the mutex for as long as it liked and the next request waited on it — the endpoint
     * served one client and then appeared to hang. Sending from an immutable buffer with no
     * lock held means a stalled client costs nothing but its own socket. */
    const esp_err_t cap = capture();

    if (cap != ESP_OK) {
        /* Reported rather than left to time out: a capture that could not be taken must look
         * like a failure, not like a very slow success. */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            (cap == ESP_ERR_TIMEOUT) ? "capture busy" : "capture failed");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_FAIL;
    }

    /* Copied into a local buffer, and the capture mutex released before the first byte goes out.
     *
     * Two DIFFERENT locks, and the distinction is the whole point:
     *
     *   s_lock     guards the capture buffer - held for the milliseconds a snapshot and encode take.
     *   s_tx_lock  serialises responses    - held for the seconds a 1.5 MB body takes over Wi-Fi.
     *
     * The original server held one mutex for both, so a slow transfer also blocked capture; and
     * capture takes the LVGL lock, so a stalled client could freeze the display. Removing the lock
     * from the transfer entirely then caused the opposite fault: with nothing serialising them,
     * concurrent requests each copied 1.5 MB out of the capture buffer and exhausted PSRAM, leaving
     * the server unable to accept connections ("Failed to connect", 21 s) while ICMP still answered
     * in 0 ms.
     *
     * So the send is serialised, but against other SENDS rather than against capture. A queued
     * request waits for the previous response instead of piling another 1.5 MB copy on top of it,
     * and capture still runs whenever the display can give it a moment. */
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(20000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "a transfer is already running");
        return ESP_FAIL;
    }

    uint8_t *body = (uint8_t *)heap_caps_malloc(s_png_len, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        body = (uint8_t *)heap_caps_malloc(s_png_len, MALLOC_CAP_8BIT);
    }
    if (body == NULL) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_tx_lock);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no room to send");
        return ESP_FAIL;
    }

    const size_t len = s_png_len;
    memcpy(body, s_png, len);
    xSemaphoreGive(s_lock);

    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Chunked, 64 KB at a time, and the result of every chunk is checked.
     *
     * One httpd_resp_send() of the whole body reports failure only at the end, after the server has
     * spent the entire send timeout on a client that may already be gone. Chunking lets a dead
     * client be abandoned at the first chunk that fails, and leaves the connection in a state the
     * server can reuse rather than one it times out of. */
    esp_err_t err = ESP_OK;
    const size_t kChunk = 65536;

    for (size_t sent = 0; sent < len; sent += kChunk) {
        const size_t n = ((len - sent) < kChunk) ? (len - sent) : kChunk;
        err = httpd_resp_send_chunk(req, (const char *)(body + sent), (ssize_t)n);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "client went away after %u of %u bytes: %s",
                     (unsigned)sent, (unsigned)len, esp_err_to_name(err));
            break;
        }
    }

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);      /* terminate the chunked body */
    }

    heap_caps_free(body);
    xSemaphoreGive(s_tx_lock);
    return err;
}

static esp_err_t stats_get(httpd_req_t *req)
{
    char body[160];
    snprintf(body, sizeof(body), "%dx%d captures=%lu bytes=%u\n",
             s_width, s_height, (unsigned long)s_captures, (unsigned)s_png_len);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* ---- Lifetime ------------------------------------------------------------ */

static void log_url(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};

    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        ESP_LOGW(TAG, "screenshots: http://" IPSTR "/", IP2STR(&ip.ip));
    } else {
        ESP_LOGW(TAG, "screenshots: server up, no IP yet - check the address once Wi-Fi is up");
    }
}

esp_err_t toolbox_screenshot_start(void)
{
    if (s_lock != NULL) {
        return ESP_OK;              /* already running */
    }

    crc_table_init();

    if (!checksum_selfcheck()) {
        ESP_LOGE(TAG, "refusing to serve PNG: the checksum implementation is wrong");
        return ESP_FAIL;
    }

    s_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_tx_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* One capture before the server opens, so the first request has something to return
     * rather than a 404 that looks like a failure. */
    (void)capture();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get,
    };
    const httpd_uri_t uri_png = {
        .uri = "/screen.png", .method = HTTP_GET, .handler = screen_get,
    };
    const httpd_uri_t uri_stats = {
        .uri = "/stats", .method = HTTP_GET, .handler = stats_get,
    };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_png);
    httpd_register_uri_handler(server, &uri_stats);

    log_url();
    ESP_LOGW(TAG, "screenshot server started (3 handlers, PNG only)");
    return ESP_OK;
}

void toolbox_screenshot_capture(void)
{
    (void)capture();
}

#else  /* !CONFIG_TOOLBOX_SCREENSHOT_SERVER */

esp_err_t toolbox_screenshot_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void toolbox_screenshot_capture(void)
{
}

#endif
























