/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "MusicPlayer.hpp"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:MusicPlayer"
#include "esp_lib_utils.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "gui_music/lv_demo_music.h"
#include "gui_music/lv_demo_music_main.h"
#include "FilePicker.hpp"

#include <string.h>

#define MUSIC_DIR BSP_SPIFFS_MOUNT_POINT "/music"

LV_IMG_DECLARE(img_app_musicplayer);

static const char *TAG = "MusicPlayer";

namespace esp_brookesia::apps
{

    MusicPlayer *MusicPlayer::_instance = nullptr;

    MusicPlayer *MusicPlayer::requestInstance(bool use_status_bar, bool use_navigation_bar)
    {
        if (_instance == nullptr)
        {
            _instance = new MusicPlayer(use_status_bar, use_navigation_bar);
        }
        return _instance;
    }

    MusicPlayer::MusicPlayer(bool use_status_bar, bool use_navigation_bar) : App("MusicPlayer", &img_app_musicplayer, true, use_status_bar, use_navigation_bar),
                                                                             _file_iterator(NULL)
    {
    }

    MusicPlayer::~MusicPlayer()
    {
    }

    bool MusicPlayer::run(void)
    {
        ESP_UTILS_LOGD("Run");

        if (bsp_extra_player_init() != ESP_OK)
        {
            ESP_LOGE(TAG, "Play init with SPIFFS failed");
            return false;
        }

        /* Let the user choose a track rather than always playing the fixed SPIFFS
         * folder. The picker starts where music usually lives and can walk the whole
         * card, so tracks kept on the SD card work too. */
        if (!file_picker::open(MUSIC_DIR, ".mp3", onFilePicked, onFilePickCancelled, this) &&
            !file_picker::open("/sdcard", ".mp3", onFilePicked, onFilePickCancelled, this))
        {
            ESP_LOGE(TAG, "no music folder and no file picker");
            return false;
        }

        return true;
    }

    void MusicPlayer::onFilePicked(const char *path, void *user)
    {
        MusicPlayer *self = static_cast<MusicPlayer *>(user);
        if (self == nullptr || path == nullptr)
        {
            return;
        }

        /* The demo's playlist is a directory, so list the picked file's folder and then
         * start on the track that was actually chosen. */
        char dir[160];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash == nullptr)
        {
            return;
        }
        *slash = '\0';

        if (bsp_extra_file_instance_init(dir, &self->_file_iterator) != ESP_OK)
        {
            ESP_LOGE(TAG, "cannot list %s", dir);
            return;
        }

        lv_demo_music(lv_scr_act(), self->_file_iterator);

        char full[160];
        const size_t count = file_iterator_get_count(self->_file_iterator);
        for (size_t i = 0; i < count; i++)
        {
            if (file_iterator_get_full_path_from_index(self->_file_iterator, i, full, sizeof(full)) &&
                strcmp(full, path) == 0)
            {
                file_iterator_set_index(self->_file_iterator, i);
                lv_demo_music_play((uint32_t)i);
                break;
            }
        }
    }

    void MusicPlayer::onFilePickCancelled(const char *path, void *user)
    {
        (void)path;
        MusicPlayer *self = static_cast<MusicPlayer *>(user);
        if (self != nullptr)
        {
            self->notifyCoreClosed();
        }
    }

    bool MusicPlayer::back(void)
    {
        ESP_UTILS_LOGD("Back");
        /* The picker overlays the app, so it takes the back gesture first. */
        if (file_picker::is_open())
        {
            file_picker::close();
            return true;
        }
        // If the app needs to exit, call notifyCoreClosed() to notify the core to close the app
        ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
        return true;
    }

    bool MusicPlayer::close(void)
    {
        ESP_UTILS_LOGD("Close");
        file_picker::close();
        if (audio_player_pause() != ESP_OK)
        {
            ESP_LOGE(TAG, "audio_player_pause failed");
            return false;
        }
        if (bsp_extra_player_del() != ESP_OK)
        {
            ESP_LOGE(TAG, "DEL Play init with SPIFFS failed");
            return false;
        }
        return true;
    }

    bool MusicPlayer::init()
    {
        ESP_UTILS_LOGD("Init");
        return true;
    }

    bool MusicPlayer::deinit()
    {
        ESP_UTILS_LOGD("Deinit");
        return true;
    }

    bool MusicPlayer::pause()
    {
        ESP_UTILS_LOGD("Pause");
        lv_demo_music_exit_pause();
        return true;
    }

    bool MusicPlayer::resume()
    {
        ESP_UTILS_LOGD("Resume");
        return true;
    }

} // namespace esp_brookesia::apps