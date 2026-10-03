/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — PCAP writer. See wifi_toolbox_pcap.h for why the link type is
 * the part that matters.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>

/* ESP-IDF has no statvfs(); the FAT layer's own query is esp_vfs_fat_info(). */
#include "esp_vfs_fat.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "wifi_toolbox_pcap.h"

static const char *TAG = "tb_pcap";

#define PCAP_DIR                "/sdcard/captures"
#define PCAP_ROTATE_BYTES       (64u * 1024u * 1024u)
/* Stop before the card is full rather than after a write fails: a capture that
 * dies mid-file leaves a file that opens and is short, which is worse than one
 * that ends cleanly. */
#define PCAP_FREE_FLOOR_BYTES   (2u * 1024u * 1024u)

/* Classic PCAP. All fields little-endian, which the magic number declares. */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0xa1b2c3d4 = little-endian, microsecond resolution */
    uint16_t version_major;
    uint16_t version_minor;
    int32_t  thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;       /* link type */
} pcap_file_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
} pcap_rec_hdr_t;

static struct {
    FILE                           *fp;
    SemaphoreHandle_t               lock;
    int                             index;
    uint32_t                        packets;
    uint64_t                        bytes;
    uint32_t                        rotations;
    bool                            stopped_for_space;
    char                            path[96];
    char                            name_hint[32];
} s_pcap;

static void pcap_lock(void)
{
    if (s_pcap.lock != NULL) {
        xSemaphoreTake(s_pcap.lock, portMAX_DELAY);
    }
}

static void pcap_unlock(void)
{
    if (s_pcap.lock != NULL) {
        xSemaphoreGive(s_pcap.lock);
    }
}

/* Free space on the volume holding `path`, via the FAT layer.
 *
 * When the query fails we treat space as unknown and keep capturing rather than
 * refusing the session: a filesystem that cannot answer is not evidence that the
 * card is full, and stopping a capture on a failed diagnostic would be the
 * diagnostic causing the outage. */
static bool space_available(const char *path, uint64_t *free_bytes)
{
    uint64_t total = 0;
    uint64_t free_b = 0;

    if (esp_vfs_fat_info(path, &total, &free_b) != ESP_OK) {
        return true;
    }

    *free_bytes = free_b;

    return free_b > PCAP_FREE_FLOOR_BYTES;
}

static esp_err_t open_current(void)
{
    char path[sizeof(s_pcap.path)];

    /* Index in the name so rotation never overwrites an earlier file, and a
     * timestamp so a session is identifiable in a file listing. */
    struct timeval tv;
    gettimeofday(&tv, NULL);

    snprintf(path, sizeof(path), PCAP_DIR "/%s-%ld-%03d.pcap",
             (s_pcap.name_hint[0] != '\0') ? s_pcap.name_hint : "capture",
             (long)tv.tv_sec, s_pcap.index);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_FAIL;
    }

    pcap_file_hdr_t hdr = {
        .magic = 0xa1b2c3d4u,
        .version_major = 2,
        .version_minor = 4,
        .thiszone = 0,
        .sigfigs = 0,
        .snaplen = WIFI_TOOLBOX_CAP_FRAME_MAX,
        .network = WIFI_TOOLBOX_PCAP_LINKTYPE,
    };

    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1) {
        ESP_LOGE(TAG, "cannot write the PCAP header to %s", path);
        fclose(fp);
        return ESP_FAIL;
    }

    s_pcap.fp = fp;
    snprintf(s_pcap.path, sizeof(s_pcap.path), "%s", path);

    ESP_LOGI(TAG, "capturing to %s (link type %u = DLT_IEEE802_11, bare 802.11 frames)",
             path, (unsigned)WIFI_TOOLBOX_PCAP_LINKTYPE);

    return ESP_OK;
}

esp_err_t wifi_toolbox_pcap_start(const char *name_hint)
{
    if (s_pcap.lock == NULL) {
        s_pcap.lock = xSemaphoreCreateMutex();
        if (s_pcap.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    pcap_lock();

    if (s_pcap.fp != NULL) {
        pcap_unlock();
        return ESP_OK;                     /* already capturing */
    }

    /* mkdir is not recursive and the mount point exists; failures here are
     * reported because "no such directory" would otherwise look like a write
     * failure on every frame. */
    struct stat st;
    if ((stat("/sdcard", &st) != 0)) {
        pcap_unlock();
        ESP_LOGE(TAG, "no SD card mounted at /sdcard — cannot write PCAPs");
        return ESP_ERR_NOT_FOUND;
    }

    if ((stat(PCAP_DIR, &st) != 0) && (mkdir(PCAP_DIR, 0777) != 0)) {
        pcap_unlock();
        ESP_LOGE(TAG, "cannot create %s", PCAP_DIR);
        return ESP_FAIL;
    }

    s_pcap.packets = 0;
    s_pcap.bytes = 0;
    s_pcap.rotations = 0;
    s_pcap.index = 0;
    s_pcap.stopped_for_space = false;
    s_pcap.name_hint[0] = '\0';

    if ((name_hint != NULL) && (name_hint[0] != '\0')) {
        /* Keep it filename-safe: this string reaches a filesystem. */
        size_t j = 0;
        for (size_t i = 0; (name_hint[i] != '\0') && (j + 1 < sizeof(s_pcap.name_hint)); i++) {
            const char c = name_hint[i];
            s_pcap.name_hint[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                     (c >= '0' && c <= '9') || (c == '-') || (c == '_')) ? c : '_';
        }
        s_pcap.name_hint[j] = '\0';
    }

    const esp_err_t err = open_current();

    pcap_unlock();
    return err;
}

esp_err_t wifi_toolbox_pcap_write(const wifi_toolbox_frame_evt_t *hdr, const uint8_t *payload)
{
    if ((hdr == NULL) || (payload == NULL) || (hdr->payload_len == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);

    pcap_lock();

    if (s_pcap.fp == NULL) {
        pcap_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if (s_pcap.stopped_for_space) {
        pcap_unlock();
        return ESP_ERR_NO_MEM;
    }

    /* Rotate on size before writing, so no single file is unbounded. */
    if (s_pcap.bytes >= PCAP_ROTATE_BYTES) {
        fclose(s_pcap.fp);
        s_pcap.fp = NULL;
        s_pcap.index++;
        s_pcap.bytes = 0;
        s_pcap.rotations++;

        if (open_current() != ESP_OK) {
            pcap_unlock();
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "rotated to %s after %u MB", s_pcap.path, (unsigned)(PCAP_ROTATE_BYTES / (1024u * 1024u)));
    }

    uint64_t free_bytes = 0;
    if (!space_available("/sdcard", &free_bytes)) {
        ESP_LOGE(TAG, "stopping capture: only %llu bytes free on the card",
                 (unsigned long long)free_bytes);
        s_pcap.stopped_for_space = true;
        pcap_unlock();
        return ESP_ERR_NO_MEM;
    }

    pcap_rec_hdr_t rec = {
        .ts_sec = (uint32_t)tv.tv_sec,
        .ts_usec = (uint32_t)tv.tv_usec,
        .incl_len = hdr->payload_len,
        /* orig_len is the frame's real length as the radio reported it, so a
         * clipped record still says how much was lost rather than pretending the
         * frame was short. */
        .orig_len = (hdr->sig_len > hdr->payload_len) ? hdr->sig_len : hdr->payload_len,
    };

    const bool ok = (fwrite(&rec, sizeof(rec), 1, s_pcap.fp) == 1) &&
                    (fwrite(payload, hdr->payload_len, 1, s_pcap.fp) == 1);

    if (!ok) {
        ESP_LOGE(TAG, "write failed at packet %u — stopping capture", (unsigned)s_pcap.packets);
        pcap_unlock();
        return ESP_FAIL;
    }

    s_pcap.packets++;
    s_pcap.bytes += sizeof(rec) + hdr->payload_len;

    pcap_unlock();
    return ESP_OK;
}

void wifi_toolbox_pcap_stop(void)
{
    pcap_lock();

    if (s_pcap.fp != NULL) {
        fflush(s_pcap.fp);
        fclose(s_pcap.fp);
        s_pcap.fp = NULL;
        ESP_LOGI(TAG, "closed %s (%u packets, %llu bytes, %u rotations)",
                 s_pcap.path, (unsigned)s_pcap.packets,
                 (unsigned long long)s_pcap.bytes, (unsigned)s_pcap.rotations);
    }

    pcap_unlock();
}

bool wifi_toolbox_pcap_is_open(void)
{
    pcap_lock();
    const bool open = (s_pcap.fp != NULL);
    pcap_unlock();

    return open;
}

void wifi_toolbox_pcap_get_stats(wifi_toolbox_pcap_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    pcap_lock();
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", s_pcap.path);
    out->packets = s_pcap.packets;
    out->bytes = s_pcap.bytes;
    out->rotations = s_pcap.rotations;
    out->stopped_for_space = s_pcap.stopped_for_space;
    pcap_unlock();
}

int wifi_toolbox_pcap_list(char *out, size_t out_len, int max_names)
{
    if ((out == NULL) || (out_len == 0) || (max_names <= 0)) {
        return -1;
    }

    DIR *dir = opendir(PCAP_DIR);
    if (dir == NULL) {
        return 0;                          /* no directory yet = no captures */
    }

    size_t used = 0;
    int count = 0;
    struct dirent *ent;

    while (((ent = readdir(dir)) != NULL) && (count < max_names)) {
        const size_t len = strlen(ent->d_name);

        if ((len < 5) || (strcmp(ent->d_name + len - 5, ".pcap") != 0)) {
            continue;
        }
        if (used + len + 1 >= out_len) {
            break;
        }

        memcpy(out + used, ent->d_name, len + 1);
        used += len + 1;
        count++;
    }

    closedir(dir);

    return count;
}
