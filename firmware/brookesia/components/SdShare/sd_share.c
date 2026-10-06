/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SD card sharing with a USB host. See the header for the ownership rules.
 *
 * Ownership model: esp_tinyusb is the single owner of the card and of the /sdcard mount.
 * It mounts FATFS at /sdcard for the device (mount_point = APP), and on USB attach it
 * does its own soft unmount -- it flushes FatFs and unregisters the VFS but KEEPS the
 * card -- then exposes the raw sectors to the host; on detach it remounts /sdcard.
 *
 * The BSP must therefore NOT mount /sdcard. The card is brought up here the same way
 * bsp_sdcard_mount() does (Slot 0, 4-bit, VO4 LDO) but without any FATFS mount, because
 * the BSP's unmount (esp_vfs_fat_sdcard_unmount) frees the card -- which is fatal when
 * esp_tinyusb still holds a pointer to it.
 */
#include "sd_share.h"

#include <stdlib.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "vendor_lookup.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "SdShare";
static char s_base_path[] = "/sdcard";

static tinyusb_msc_storage_handle_t s_msc;
static volatile bool s_host_owns;
static bool s_started;
static esp_ldo_channel_handle_t s_vo4_chan;

bool sd_share_host_owns_card(void)
{
    return s_host_owns;
}

/* The SD card shares the VO4 LDO with the DSI PHY; the BSP's helper is static, so acquire
 * the channel here instead. Re-acquiring an already-acquired channel is a refcount, so
 * this is safe to call once per boot. */
static esp_err_t enable_vo4(void)
{
    if (s_vo4_chan != NULL) {
        return ESP_OK;
    }
    esp_ldo_channel_config_t cfg = {
        .chan_id = 4,
        .voltage_mv = 3300,
    };
    return esp_ldo_acquire_channel(&cfg, &s_vo4_chan);
}

/* Bring the card up exactly like bsp_sdcard_mount(), but do not mount a filesystem. */
static esp_err_t init_card(void)
{
    if (bsp_sdcard != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(enable_vo4(), TAG, "LDO VO4");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    sdmmc_card_t *card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    ESP_RETURN_ON_FALSE(card != NULL, ESP_ERR_NO_MEM, TAG, "card malloc");

    esp_err_t err = sdmmc_host_init();
    if (err == ESP_OK) {
        err = sdmmc_host_init_slot(host.slot, &slot_config);
    }
    if (err == ESP_OK) {
        err = sdmmc_card_init(&host, card);
    }
    if (err != ESP_OK) {
        free(card);
        return err;
    }

    bsp_sdcard = card;
    ESP_LOGI(TAG, "SDMMC card ready: %u sectors x %u bytes",
             (unsigned)card->csd.capacity, (unsigned)card->csd.sector_size);
    return ESP_OK;
}

/* esp_tinyusb tells us who owns the volume on each transition; drive the gate from it.
 * The gate must go up as soon as the host starts taking the card, and come down only
 * once the device's own mount is fully back. */
static void on_storage_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;

    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
            s_host_owns = true;
            ESP_LOGI(TAG, "SD card handed to the USB host");
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            s_host_owns = false;
            ESP_LOGI(TAG, "SD card back with the device");
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(TAG, "storage mount/unmount failed");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGE(TAG, "card has no filesystem; refusing to format it");
        break;
    default:
        break;
    }
}

esp_err_t sd_share_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_card(), TAG, "init_card");

    tinyusb_msc_driver_config_t driver_cfg = { 0 };
    driver_cfg.callback = on_storage_event;
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver_cfg), TAG, "msc_install_driver");

    tinyusb_msc_storage_config_t storage_cfg = { 0 };
    storage_cfg.medium.card = bsp_sdcard;
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;   /* esp_tinyusb mounts /sdcard */
    storage_cfg.fat_fs.base_path = s_base_path;
    storage_cfg.fat_fs.config.max_files = 5;
    storage_cfg.fat_fs.do_not_format = true;                   /* never erase the user's media */
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_msc), TAG,
                        "new_storage_sdmmc");

    /* /sdcard is mounted and still owned by the device here -- the USB host cannot take
     * it over until tinyusb_driver_install() below arms the port. Load the vendor
     * identification database now, before that race opens, so the read cannot be cut
     * short by a soft unmount. */
    (void)vendor_lookup_load("/sdcard/vendor_db.bin");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb_driver_install");

    s_started = true;
    ESP_LOGI(TAG, "armed; esp_tinyusb owns /sdcard and shares it on attach");
    return ESP_OK;
}
