/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device-wide screen capture engine.
 *
 * Screenshots are full-resolution lossless PNG. There is no deflate compressor in
 * this IDF, so the PNG uses STORED deflate blocks — valid everywhere, larger than a
 * compressed PNG. Recording uses the ESP32-P4 hardware JPEG encoder and wraps the
 * frames in a standard MJPEG AVI (MJPG fourcc), which the on-device VideoPlayer and
 * desktop players both read.
 *
 * The snapshot is taken under the system LVGL lock (the adapter lock), and only the
 * snapshot is taken under it — encoding and SD writes happen outside the lock so the
 * UI task is not stalled behind them.
 */

#include "screen_capture.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "src/others/snapshot/lv_snapshot.h"

#include "driver/jpeg_encode.h"

static const char *TAG = "screen_capture";

/* Directory and file names stay within FATFS 8.3 (this build has LFN disabled:
 * CONFIG_FATFS_LFN_NONE). "screenshots" and "recordings" are too long. */
#define SHOT_DIR    "/sdcard/shots"
#define REC_DIR     "/sdcard/recs"
#define REC_FPS     10
#define REC_QUALITY 70

static int s_shot_seq = 0;
static int s_rec_seq = 0;

/* Screenshot downscale. The PNG encoder is software (stored deflate) and a full
 * 720x720 pass walks ~1.5 MB through PSRAM, which starves the esp-hosted SDIO task
 * and can take the board down — the same fault the HTTP screenshot helper documents.
 * Scale 2 gives a 360x360 capture, a quarter of the pixels and a sixteenth of the work. */
#define SCREENSHOT_SCALE 2

static SemaphoreHandle_t s_shot_mutex;      /* serialises screenshot captures */
static SemaphoreHandle_t s_rec_mutex;      /* guards recording state transitions */
static SemaphoreHandle_t s_rec_done;       /* recording task signals it finished */
static TaskHandle_t s_rec_task = NULL;
static volatile bool s_recording = false;
static volatile int  s_frame_count = 0;

static jpeg_encoder_handle_t s_jpeg = NULL;

/* Recording file state, owned by the recording task. */
static FILE *s_rec_file = NULL;
static long  s_rec_riff_off = 0;
static long  s_rec_total_off = 0;
static long  s_rec_length_off = 0;
static long  s_rec_movi_off = 0;
static long  s_rec_movi_data = 0;
static lv_draw_buf_t *s_rec_buf = NULL;

/* idx1 index entries, accumulated by the recording task. */
typedef struct { uint32_t offset; uint32_t size; } idx_entry_t;
static idx_entry_t *s_idx = NULL;
static size_t s_idx_len = 0;
static size_t s_idx_cap = 0;

/* ---- little-endian write helpers (ESP32 is little-endian) ----------------- */

static void w32(FILE *f, uint32_t v) { fwrite(&v, 1, 4, f); }
static void w16(FILE *f, uint16_t v) { fwrite(&v, 1, 2, f); }
static void w4cc(FILE *f, const char c[4]) { fwrite(c, 1, 4, f); }

static void write_at(FILE *f, long off, uint32_t v)
{
    long here = ftell(f);
    fseek(f, off, SEEK_SET);
    w32(f, v);
    fseek(f, here, SEEK_SET);
}

/* ---- PNG primitives ------------------------------------------------------- */

static uint32_t png_crc_table[256];

static void png_crc_init(void)
{
    if (png_crc_table[1] != 0) {
        return;
    }
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        png_crc_table[n] = c;
    }
}

static uint32_t png_crc(uint32_t crc, const uint8_t *buf, size_t len)
{
    for (size_t n = 0; n < len; n++) {
        crc = png_crc_table[(crc ^ buf[n]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static size_t png_be32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
    return 4;
}

static size_t png_chunk(uint8_t *out, const char type[4], const uint8_t *data, size_t len)
{
    size_t n = png_be32(out, (uint32_t)len);
    memcpy(out + n, type, 4);
    n += 4;
    if (data != NULL && len > 0) {
        memcpy(out + n, data, len);
    }
    uint32_t crc = png_crc(0xFFFFFFFFu, out + 4, len + 4) ^ 0xFFFFFFFFu;
    n += len;
    n += png_be32(out + n, crc);
    return n;
}

/* Encode an RGB565 buffer as a PNG (8-bit truecolour, stored deflate), optionally
 * downscaled by `scale` (nearest neighbour) during encode. `out` receives a freshly
 * allocated buffer the caller frees. */
static esp_err_t png_encode_rgb565(const uint8_t *rgb565, int w, int h, int scale,
                                   uint8_t **out, size_t *out_len)
{
    png_crc_init();

    const int ow = (w + scale - 1) / scale;
    const int oh = (h + scale - 1) / scale;

    const size_t stride = 1 + (size_t)ow * 3;           /* filter byte + RGB888 row */
    const size_t raw_len = stride * (size_t)oh;
    const size_t zlib_cap = raw_len + (raw_len / 16) + 1024;
    const size_t need = 8 + 25 + (12 + zlib_cap) + 12 + 64;

    uint8_t *png = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (png == NULL) {
        png = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_8BIT);
    }
    if (png == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *z = (uint8_t *)heap_caps_malloc(zlib_cap, MALLOC_CAP_SPIRAM);
    if (z == NULL) {
        z = (uint8_t *)heap_caps_malloc(zlib_cap, MALLOC_CAP_8BIT);
    }
    if (z == NULL) {
        free(png);
        return ESP_ERR_NO_MEM;
    }

    /* Build the zlib stream (one stored deflate block per group of whole rows). */
    size_t zi = 0;
    uint32_t ad_a = 1, ad_b = 0;
    size_t ad_since = 0;
    z[zi++] = 0x78;
    z[zi++] = 0x01;

    const size_t rows_per_block = 65535 / stride;
    if (rows_per_block == 0) {
        free(z);
        free(png);
        return ESP_FAIL;
    }

    int y = 0;
    while (y < oh) {
        size_t rows = (size_t)(oh - y);
        if (rows > rows_per_block) {
            rows = rows_per_block;
        }
        const size_t take = rows * stride;
        const int final = (y + (int)rows >= oh) ? 1 : 0;

        z[zi++] = (uint8_t)final;
        z[zi++] = (uint8_t)(take & 0xFF);
        z[zi++] = (uint8_t)((take >> 8) & 0xFF);
        z[zi++] = (uint8_t)((~take) & 0xFF);
        z[zi++] = (uint8_t)(((~take) >> 8) & 0xFF);

        for (size_t r = 0; r < rows; r++, y++) {
            z[zi++] = 0;    /* filter type 0 */
            ad_b += ad_a;
            if (++ad_since >= 256) { ad_a %= 65521; ad_b %= 65521; ad_since = 0; }

            const uint16_t *line = (const uint16_t *)rgb565 + ((size_t)(y * scale) * (size_t)w);
            for (int px = 0; px < ow; px++) {
                const uint16_t c = line[px * scale];
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

    ad_a %= 65521;
    ad_b %= 65521;
    png_be32(z + zi, (ad_b << 16) | ad_a);
    zi += 4;

    /* Assemble the file. */
    size_t o = 0;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(png, sig, sizeof(sig));
    o += sizeof(sig);

    uint8_t ihdr[13];
    size_t ih = 0;
    ih += png_be32(ihdr + ih, (uint32_t)ow);
    ih += png_be32(ihdr + ih, (uint32_t)oh);
    ihdr[ih++] = 8;     /* bit depth */
    ihdr[ih++] = 2;     /* truecolour */
    ihdr[ih++] = 0;     /* deflate */
    ihdr[ih++] = 0;     /* filtering */
    ihdr[ih++] = 0;     /* no interlace */
    o += png_chunk(png + o, "IHDR", ihdr, ih);
    o += png_chunk(png + o, "IDAT", z, zi);
    o += png_chunk(png + o, "IEND", NULL, 0);

    free(z);

    *out = png;
    *out_len = o;
    return ESP_OK;
}

static uint32_t png_be32_read(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Decode the exact stored-deflate truecolour PNG that png_encode_rgb565 writes,
 * back into RGB565. */
bool screen_capture_png_to_rgb565(const uint8_t *png, size_t png_len,
                                  int *out_w, int *out_h, uint8_t **out_rgb565)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (png == NULL || png_len < 8 + 25 || memcmp(png, sig, 8) != 0) {
        return false;
    }

    uint32_t w = 0, h = 0;
    int bit_depth = 0, color_type = 0;
    const uint8_t *idat = NULL;
    size_t idat_len = 0;

    size_t off = 8;
    while (off + 8 <= png_len) {
        uint32_t clen = png_be32_read(png + off);
        const uint8_t *type = png + off + 4;
        const uint8_t *data = png + off + 8;
        off += 8 + clen + 4;
        if (off > png_len) {
            return false;
        }
        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            w = png_be32_read(data);
            h = png_be32_read(data + 4);
            bit_depth = data[8];
            color_type = data[9];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat = data;
            idat_len = clen;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
    }

    if (bit_depth != 8 || color_type != 2 || w == 0 || h == 0 || idat == NULL || idat_len < 6) {
        return false;
    }

    /* zlib header + stored deflate blocks. */
    if (idat[0] != 0x78) {
        return false;
    }
    size_t zi = 2;
    const size_t row_bytes = (size_t)w * 3;
    const size_t raw_len = (1 + row_bytes) * (size_t)h;

    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (raw == NULL) {
        return false;
    }
    size_t o = 0;
    while (zi < idat_len && o < raw_len) {
        if (zi + 5 > idat_len) {
            free(raw);
            return false;
        }
        const uint8_t final = idat[zi++];
        const uint16_t blen = (uint16_t)(idat[zi] | (idat[zi + 1] << 8));
        zi += 4;    /* LEN + NLEN */
        if (zi + blen > idat_len || o + blen > raw_len) {
            free(raw);
            return false;
        }
        memcpy(raw + o, idat + zi, blen);
        zi += blen;
        o += blen;
        if (final) {
            break;
        }
    }
    if (o != raw_len) {
        free(raw);
        return false;
    }

    uint8_t *rgb565 = (uint8_t *)malloc((size_t)w * h * 2);
    if (rgb565 == NULL) {
        free(raw);
        return false;
    }

    size_t p = 0;
    for (uint32_t y = 0; y < h; y++) {
        if (raw[p] != 0) {   /* filter type must be none */
            free(raw);
            free(rgb565);
            return false;
        }
        p++;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t r = raw[p], g = raw[p + 1], b = raw[p + 2];
            p += 3;
            const uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            rgb565[((size_t)y * w + x) * 2] = (uint8_t)(c & 0xFF);
            rgb565[((size_t)y * w + x) * 2 + 1] = (uint8_t)((c >> 8) & 0xFF);
        }
    }
    free(raw);

    *out_w = (int)w;
    *out_h = (int)h;
    *out_rgb565 = rgb565;
    return true;
}

/* ---- AVI writer ----------------------------------------------------------- */

static void avi_write_header(FILE *f, int w, int h, int fps)
{
    w4cc(f, "RIFF");
    s_rec_riff_off = ftell(f); w32(f, 0);
    w4cc(f, "AVI ");

    w4cc(f, "LIST"); w32(f, 192); w4cc(f, "hdrl");

    w4cc(f, "avih"); w32(f, 56);
    w32(f, (uint32_t)(1000000 / fps));  /* dwMicroSecPerFrame */
    w32(f, 0);                          /* dwMaxBytesPerSec */
    w32(f, 0);                          /* dwPaddingGranularity */
    w32(f, 0x10);                       /* AVIF_HASINDEX */
    s_rec_total_off = ftell(f); w32(f, 0);   /* dwTotalFrames */
    w32(f, 0);                          /* dwInitialFrames */
    w32(f, 1);                          /* dwStreams */
    w32(f, (uint32_t)(w * h * 3));      /* dwSuggestedBufferSize */
    w32(f, (uint32_t)w);
    w32(f, (uint32_t)h);
    w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);

    w4cc(f, "LIST"); w32(f, 116); w4cc(f, "strl");

    w4cc(f, "strh"); w32(f, 56);
    w4cc(f, "vids"); w4cc(f, "MJPG");
    w32(f, 0);                          /* dwFlags */
    w16(f, 0); w16(f, 0);               /* priority + language */
    w32(f, 0);                          /* dwInitialFrames */
    w32(f, 1); w32(f, (uint32_t)fps);   /* dwScale / dwRate */
    w32(f, 0);                          /* dwStart */
    s_rec_length_off = ftell(f); w32(f, 0);   /* dwLength */
    w32(f, (uint32_t)(w * h * 3));      /* dwSuggestedBufferSize */
    w32(f, 0xFFFFFFFF);                 /* dwQuality = -1 */
    w32(f, 0);                          /* dwSampleSize */
    w16(f, 0); w16(f, 0); w16(f, (uint16_t)w); w16(f, (uint16_t)h);

    w4cc(f, "strf"); w32(f, 40);
    w32(f, (uint32_t)w); w32(f, (uint32_t)h);
    w16(f, 1); w16(f, 24);
    w4cc(f, "MJPG");
    w32(f, (uint32_t)(w * h * 3));      /* biSizeImage */
    w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);

    w4cc(f, "LIST");
    s_rec_movi_off = ftell(f); w32(f, 0);
    w4cc(f, "movi");
}

static void avi_write_frame(FILE *f, const uint8_t *jpeg, uint32_t jpeg_len)
{
    /* idx1 offset is from the start of the movi list DATA (after "movi"). */
    if (s_idx_len == s_idx_cap) {
        size_t ncap = (s_idx_cap == 0) ? 256 : s_idx_cap * 2;
        idx_entry_t *bigger = (idx_entry_t *)realloc(s_idx, ncap * sizeof(idx_entry_t));
        if (bigger == NULL) {
            return;                     /* index is best-effort; frames still land */
        }
        s_idx = bigger;
        s_idx_cap = ncap;
    }
    s_idx[s_idx_len].offset = (uint32_t)s_rec_movi_data;
    s_idx[s_idx_len].size = jpeg_len;
    s_idx_len++;

    w4cc(f, "00dc");
    w32(f, jpeg_len);
    fwrite(jpeg, 1, jpeg_len, f);
    if (jpeg_len & 1) {
        fputc(0, f);
    }
    s_rec_movi_data += 8 + jpeg_len + (jpeg_len & 1);
}

static void avi_finalize(FILE *f)
{
    /* idx1 index */
    w4cc(f, "idx1");
    w32(f, (uint32_t)(s_idx_len * 16));
    for (size_t i = 0; i < s_idx_len; i++) {
        w4cc(f, "00dc");
        w32(f, 0x10);                   /* AVIIF_KEYFRAME */
        w32(f, s_idx[i].offset);
        w32(f, s_idx[i].size);
    }

    /* Fix the dynamic fields. */
    const long end = ftell(f);
    write_at(f, s_rec_riff_off, (uint32_t)(end - 8));
    write_at(f, s_rec_total_off, (uint32_t)s_frame_count);
    write_at(f, s_rec_length_off, (uint32_t)s_frame_count);
    write_at(f, s_rec_movi_off, (uint32_t)(4 + s_rec_movi_data));
}

/* ---- snapshot ------------------------------------------------------------- */

/* Take a full-resolution RGB565 snapshot of the active screen. Returns a draw
 * buffer (caller frees with lv_draw_buf_destroy) or NULL. Must hold the LVGL lock. */
static lv_draw_buf_t *snapshot_take(void)
{
    lv_obj_t *scr = lv_screen_active();
    return lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
}

/* ---- on-screen capture indication ---------------------------------------- */

static void screenshot_flash_delete_cb(lv_timer_t *t)
{
    lv_obj_t *flash = (lv_obj_t *)lv_timer_get_user_data(t);
    if (flash != NULL) {
        lv_obj_delete(flash);
    }
}

/* Solid white overlay on the system layer for ~400 ms, shown on whatever app is on
 * screen so a capture is visible even after the user has navigated away. */
static void screenshot_flash_cb(void *user_data)
{
    (void)user_data;

    lv_obj_t *flash = lv_obj_create(lv_layer_sys());
    if (flash == NULL) {
        ESP_LOGW(TAG, "flash: system layer unavailable");
        return;
    }
    lv_obj_remove_style_all(flash);
    lv_obj_set_size(flash, lv_pct(100), lv_pct(100));
    lv_obj_align(flash, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(flash, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(flash, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(flash, 0, 0);
    lv_obj_clear_flag(flash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(flash, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_timer_t *t = lv_timer_create(screenshot_flash_delete_cb, 400, flash);
    if (t != NULL) {
        lv_timer_set_repeat_count(t, 1);
    }
    ESP_LOGI(TAG, "flash shown");
}

/* ---- init / teardown ------------------------------------------------------ */

esp_err_t screen_capture_init(void)
{
    struct stat st;
    if (stat("/sdcard", &st) != 0) {
        esp_err_t e = bsp_sdcard_mount();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(e));
            return e;
        }
        ESP_LOGI(TAG, "SD card mounted");
    }

    if (mkdir(SHOT_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %s", SHOT_DIR, strerror(errno));
    }
    if (mkdir(REC_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: %s", REC_DIR, strerror(errno));
    }

    if (s_jpeg == NULL) {
        jpeg_encode_engine_cfg_t eng = {
            .intr_priority = 0,
            .timeout_ms = 500,
        };
        esp_err_t e = jpeg_new_encoder_engine(&eng, &s_jpeg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "jpeg encoder create failed: %s", esp_err_to_name(e));
            return e;
        }
    }

    if (s_shot_mutex == NULL) {
        s_shot_mutex = xSemaphoreCreateMutex();
    }
    if (s_rec_mutex == NULL) {
        s_rec_mutex = xSemaphoreCreateMutex();
        s_rec_done = xSemaphoreCreateBinary();
    }

    return ESP_OK;
}

/* ---- screenshot ----------------------------------------------------------- */

/* Pick an 8.3 name that does not collide with an existing file, advancing `seq`. This
 * matters because the counter resets on reboot, and without it a fresh boot would
 * overwrite earlier captures instead of numbering past them. */
static void pick_unique_name(char *path, size_t len, const char *dir,
                             const char *prefix, const char *ext, int *seq)
{
    for (int i = 0; i < 10000; i++) {
        int n = (*seq + i) % 10000;
        snprintf(path, len, "%s/%s%04d%s", dir, prefix, n, ext);
        if (access(path, F_OK) != 0) {
            *seq = (n + 1) % 10000;
            return;
        }
    }
    snprintf(path, len, "%s/%s%04d%s", dir, prefix, *seq, ext);
    *seq = (*seq + 1) % 10000;
}

esp_err_t screen_capture_screenshot(void)
{
    /* Ensure the SD card is mounted (idempotent) before writing, so a capture fired
     * from the delayed task works even if the app's own init failed earlier. */
    (void)screen_capture_init();

    if (s_shot_mutex != NULL) {
        xSemaphoreTake(s_shot_mutex, portMAX_DELAY);
    }

    if (esp_lv_adapter_lock(500) != ESP_OK) {
        if (s_shot_mutex != NULL) {
            xSemaphoreGive(s_shot_mutex);
        }
        return ESP_ERR_TIMEOUT;
    }
    lv_draw_buf_t *buf = snapshot_take();
    esp_lv_adapter_unlock();

    if (buf == NULL) {
        if (s_shot_mutex != NULL) {
            xSemaphoreGive(s_shot_mutex);
        }
        ESP_LOGW(TAG, "snapshot failed (out of memory?)");
        return ESP_ERR_NO_MEM;
    }

    uint8_t *png = NULL;
    size_t png_len = 0;
    esp_err_t err = png_encode_rgb565((const uint8_t *)buf->data,
                                      (int)buf->header.w, (int)buf->header.h,
                                      SCREENSHOT_SCALE, &png, &png_len);
    lv_draw_buf_destroy(buf);

    if (err != ESP_OK) {
        if (s_shot_mutex != NULL) {
            xSemaphoreGive(s_shot_mutex);
        }
        return err;
    }

    char path[64];
    pick_unique_name(path, sizeof(path), SHOT_DIR, "shot", ".png", &s_shot_seq);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        free(png);
        if (s_shot_mutex != NULL) {
            xSemaphoreGive(s_shot_mutex);
        }
        ESP_LOGE(TAG, "cannot open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = fwrite(png, 1, png_len, f);
    fclose(f);
    free(png);

    if (s_shot_mutex != NULL) {
        xSemaphoreGive(s_shot_mutex);
    }

    if (written != png_len) {
        ESP_LOGE(TAG, "short write to %s", path);
        return ESP_FAIL;
    }

    /* Flash the screen to confirm the capture. Object creation and the animation start
     * run under the LVGL lock because this is the capture task, not the LVGL task. */
    if (esp_lv_adapter_lock(500) == ESP_OK) {
        screenshot_flash_cb(NULL);
        esp_lv_adapter_unlock();
    }

    ESP_LOGI(TAG, "screenshot saved: %s (%u bytes)", path, (unsigned)png_len);
    return ESP_OK;
}

static void screenshot_delayed_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_err_t err = screen_capture_screenshot();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "delayed screenshot failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

esp_err_t screen_capture_screenshot_delayed(uint32_t delay_ms)
{
    TaskHandle_t t = NULL;
    if (xTaskCreate(screenshot_delayed_task, "shot", 8192, (void *)(uintptr_t)delay_ms, 5, &t) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- recording ------------------------------------------------------------ */

static void record_task(void *arg)
{
    const int w = (int)s_rec_buf->header.w;
    const int h = (int)s_rec_buf->header.h;

    const size_t in_size = (size_t)w * h * 2;
    uint8_t *jpeg = (uint8_t *)heap_caps_malloc(in_size, MALLOC_CAP_SPIRAM);
    if (jpeg == NULL) {
        jpeg = (uint8_t *)heap_caps_malloc(in_size, MALLOC_CAP_8BIT);
    }
    if (jpeg == NULL) {
        ESP_LOGE(TAG, "no JPEG output buffer");
        xSemaphoreGive(s_rec_done);
        vTaskDelete(NULL);
        return;
    }

    jpeg_encode_cfg_t cfg = {
        .width = (uint32_t)w,
        .height = (uint32_t)h,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = REC_QUALITY,
        .pixel_reverse = false,
    };

    const TickType_t period = pdMS_TO_TICKS(1000 / REC_FPS);
    TickType_t last = xTaskGetTickCount();

    while (s_recording) {
        if (esp_lv_adapter_lock(500) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        lv_result_t r = lv_snapshot_take_to_draw_buf(lv_screen_active(),
                                                     LV_COLOR_FORMAT_RGB565, s_rec_buf);
        esp_lv_adapter_unlock();

        if (r != LV_RESULT_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t jpeg_len = 0;
        esp_err_t e = jpeg_encoder_process(s_jpeg, &cfg,
                                           (const uint8_t *)s_rec_buf->data,
                                           (uint32_t)in_size,
                                           jpeg, (uint32_t)in_size, &jpeg_len);
        if (e != ESP_OK || jpeg_len == 0) {
            ESP_LOGW(TAG, "jpeg encode failed: %s", esp_err_to_name(e));
        } else {
            avi_write_frame(s_rec_file, jpeg, jpeg_len);
            s_frame_count++;
        }

        vTaskDelayUntil(&last, period);
    }

    free(jpeg);

    avi_finalize(s_rec_file);
    fclose(s_rec_file);
    s_rec_file = NULL;

    ESP_LOGI(TAG, "recording stopped: %d frames", (int)s_frame_count);

    xSemaphoreGive(s_rec_done);
    vTaskDelete(NULL);
}

esp_err_t screen_capture_record_start(void)
{
    if (s_rec_mutex != NULL) {
        xSemaphoreTake(s_rec_mutex, portMAX_DELAY);
    }
    if (s_recording) {
        if (s_rec_mutex != NULL) {
            xSemaphoreGive(s_rec_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (screen_capture_init() != ESP_OK) {
        if (s_rec_mutex != NULL) {
            xSemaphoreGive(s_rec_mutex);
        }
        return ESP_FAIL;
    }

    /* Size the reusable snapshot buffer against the active screen. */
    if (esp_lv_adapter_lock(500) != ESP_OK) {
        if (s_rec_mutex != NULL) {
            xSemaphoreGive(s_rec_mutex);
        }
        return ESP_ERR_TIMEOUT;
    }
    s_rec_buf = lv_snapshot_create_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    esp_lv_adapter_unlock();
    if (s_rec_buf == NULL) {
        if (s_rec_mutex != NULL) {
            xSemaphoreGive(s_rec_mutex);
        }
        return ESP_ERR_NO_MEM;
    }

    char path[64];
    pick_unique_name(path, sizeof(path), REC_DIR, "rec", ".avi", &s_rec_seq);

    s_rec_file = fopen(path, "wb");
    if (s_rec_file == NULL) {
        lv_draw_buf_destroy(s_rec_buf);
        s_rec_buf = NULL;
        if (s_rec_mutex != NULL) {
            xSemaphoreGive(s_rec_mutex);
        }
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_FAIL;
    }

    s_idx_len = 0;
    s_frame_count = 0;
    s_rec_movi_data = 0;
    avi_write_header(s_rec_file, (int)s_rec_buf->header.w, (int)s_rec_buf->header.h, REC_FPS);

    s_recording = true;
    if (xTaskCreate(record_task, "rec", 16384, NULL, 5, &s_rec_task) != pdPASS) {
        s_recording = false;
        fclose(s_rec_file);
        s_rec_file = NULL;
        lv_draw_buf_destroy(s_rec_buf);
        s_rec_buf = NULL;
        if (s_rec_mutex != NULL) {
            xSemaphoreGive(s_rec_mutex);
        }
        return ESP_ERR_NO_MEM;
    }

    if (s_rec_mutex != NULL) {
        xSemaphoreGive(s_rec_mutex);
    }
    ESP_LOGI(TAG, "recording started: %s", path);
    return ESP_OK;
}

esp_err_t screen_capture_record_stop(void)
{
    if (s_rec_mutex != NULL) {
        xSemaphoreTake(s_rec_mutex, portMAX_DELAY);
    }
    if (!s_recording) {
        if (s_rec_mutex != NULL) {
            xSemaphoreGive(s_rec_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }
    s_recording = false;
    if (s_rec_mutex != NULL) {
        xSemaphoreGive(s_rec_mutex);
    }

    /* Wait for the task to finish the final frame, index, and close. */
    xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(5000));

    if (s_rec_buf != NULL) {
        lv_draw_buf_destroy(s_rec_buf);
        s_rec_buf = NULL;
    }
    s_rec_task = NULL;
    return ESP_OK;
}

bool screen_capture_is_recording(void)
{
    return s_recording;
}

int screen_capture_recorded_frames(void)
{
    return (int)s_frame_count;
}
