/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared file picker. See the header for the API contract.
 *
 * FATFS in this build has long filenames disabled (CONFIG_FATFS_LFN_NONE), so every
 * name it returns is an uppercase 8.3 short name — SHOT0000.PNG. They are shown
 * lower-cased, which is what the on-disk intent was, but the paths handed back are the
 * real returned names so an fopen() on them always succeeds.
 */
#include "FilePicker.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "esp_log.h"

static const char *TAG = "FilePicker";

namespace file_picker {
namespace {

struct Entry {
    std::string name;   /* what the row shows */
    std::string path;   /* full path, as the filesystem returned it */
    bool is_dir;
    long size;
    time_t mtime;       /* 0 when the filesystem would not tell us */
};

struct State {
    bool        open;
    lv_obj_t   *overlay;
    lv_obj_t   *list;
    lv_obj_t   *path_label;
    lv_obj_t   *up_btn;
    std::string dir;
    std::string filter;     /* lower-cased, e.g. ".png"; empty means every file */
    Callback    on_pick;
    Callback    on_cancel;
    void       *user;
    std::vector<Entry> entries;
};

State s = {
    .open = false,
    .overlay = nullptr,
    .list = nullptr,
    .path_label = nullptr,
    .up_btn = nullptr,
    .dir = "",
    .filter = "",
    .on_pick = nullptr,
    .on_cancel = nullptr,
    .user = nullptr,
    .entries = {},
};

std::string lower(const std::string &in)
{
    std::string out = in;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

std::string parent_of(const std::string &path)
{
    if (path.size() <= 1) {
        return "/";
    }
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::string human_size(long bytes)
{
    char buf[24];
    if (bytes >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    } else if (bytes >= 1024) {
        snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%ld B", bytes);
    }
    return buf;
}

/* Case-insensitive suffix match against the filter. */
bool matches_filter(const std::string &name)
{
    if (s.filter.empty()) {
        return true;
    }
    const std::string tail = lower(name);
    if (tail.size() < s.filter.size()) {
        return false;
    }
    return tail.compare(tail.size() - s.filter.size(), s.filter.size(), s.filter) == 0;
}

void rebuild_list(void);

void navigate(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    if (d == nullptr) {
        ESP_LOGW(TAG, "cannot open %s", dir.c_str());
        return;
    }

    s.entries.clear();

    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }

        Entry ent;
        ent.name = e->d_name;
        ent.path = dir + "/" + e->d_name;
        ent.size = 0;
        ent.mtime = 0;

        if (e->d_type == DT_DIR) {
            ent.is_dir = true;
        } else if (e->d_type == DT_REG) {
            ent.is_dir = false;
        } else {
            /* Some VFS backends report DT_UNKNOWN; ask stat instead. */
            struct stat st;
            if (stat(ent.path.c_str(), &st) != 0) {
                continue;
            }
            ent.is_dir = S_ISDIR(st.st_mode);
        }

        if (!ent.is_dir && !matches_filter(ent.name)) {
            continue;
        }

        struct stat st;
        if (!ent.is_dir && stat(ent.path.c_str(), &st) == 0) {
            ent.size = (long)st.st_size;
            ent.mtime = st.st_mtime;
        }

        ent.name = lower(ent.name);
        s.entries.push_back(ent);
    }
    closedir(d);

    /* Directories first, then alphabetically — the conventional file-manager order. */
    std::sort(s.entries.begin(), s.entries.end(),
              [](const Entry &a, const Entry &b) {
                  if (a.is_dir != b.is_dir) {
                      return a.is_dir;
                  }
                  return a.name < b.name;
              });

    s.dir = dir;
    if (s.path_label != nullptr) {
        lv_label_set_text(s.path_label, s.dir.c_str());
    }
    if (s.up_btn != nullptr) {
        /* Nothing above "/": say so rather than offering a dead button. */
        if (s.dir == "/") {
            lv_obj_add_state(s.up_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s.up_btn, LV_STATE_DISABLED);
        }
    }

    rebuild_list();
}

/* Rows are keyed by index, not by a pointer into a string: the entry vector is stable
 * while a list is shown, and an index cannot dangle the way c_str() can. */
void on_row_click(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)s.entries.size()) {
        return;
    }

    if (s.entries[idx].is_dir) {
        navigate(s.entries[idx].path);
        return;
    }

    /* Copy first: close() drops the entry the copy came from. */
    const std::string picked = s.entries[idx].path;
    Callback cb = s.on_pick;
    void *user = s.user;
    close();
    if (cb != nullptr) {
        cb(picked.c_str(), user);
    }
}

void on_up_click(lv_event_t *e)
{
    (void)e;
    if (s.dir == "/" || s.dir.empty()) {
        return;
    }
    navigate(parent_of(s.dir));
}

void on_cancel_click(lv_event_t *e)
{
    (void)e;
    Callback cb = s.on_cancel;
    void *user = s.user;
    close();
    if (cb != nullptr) {
        cb(nullptr, user);
    }
}

/* Sized for finger input: this is a touch screen, and the theme's defaults are easy to
 * miss. Both the header controls and the list rows are tuned here so they stay in step. */
#define FILE_PICKER_HEADER_BTN 64
#define FILE_PICKER_ROW_H      60

lv_obj_t *make_row(lv_obj_t *parent, const char *icon, const std::string &label,
                   const char *date, const char *size, int index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, FILE_PICKER_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2a3138), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x7d8a97), 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label.c_str());
    lv_obj_set_flex_grow(lbl, 1);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_DOTS);

    /* Timestamp and size sit in fixed-width columns so they line up down the list
     * whatever the values are. Directories carry neither. */
    if (date != nullptr && date[0] != '\0') {
        lv_obj_t *dt = lv_label_create(row);
        lv_label_set_text(dt, date);
        lv_obj_set_width(dt, 150);      /* fits YYYY-MM-DD HH:MM */
        lv_label_set_long_mode(dt, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_color(dt, lv_color_hex(0x7d8a97), 0);
    }

    if (size != nullptr) {
        lv_obj_t *sz = lv_label_create(row);
        lv_label_set_text(sz, size);
        lv_obj_set_width(sz, 80);
        lv_label_set_long_mode(sz, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_color(sz, lv_color_hex(0x7d8a97), 0);
    }

    return row;
}

lv_obj_t *make_header_button(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, FILE_PICKER_HEADER_BTN, FILE_PICKER_HEADER_BTN);
    lv_obj_set_style_radius(btn, FILE_PICKER_HEADER_BTN / 2, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void rebuild_list(void)
{    if (s.list == nullptr) {
        return;
    }

    lv_obj_clean(s.list);

    if (s.entries.empty()) {
        lv_obj_t *none = lv_label_create(s.list);
        lv_label_set_text(none, "empty");
        lv_obj_set_style_text_color(none, lv_color_hex(0x7d8a97), 0);
        return;
    }

    for (size_t i = 0; i < s.entries.size(); i++) {
        const Entry &ent = s.entries[i];
        if (ent.is_dir) {
            make_row(s.list, LV_SYMBOL_DIRECTORY, ent.name, nullptr, nullptr, (int)i);
            continue;
        }

        /* The stamp shown is the file's own, read straight out of the FAT directory
         * entry via stat() — nothing here is the device's current clock.
         *
         * The year is always printed. A file written before SNTP set the clock cannot
         * have a usable stamp: FAT has no representation before 1980, so FatFs clamps
         * it and the entry lands on 1980-01-01. Without the year that reads as a
         * plausible recent date rather than as the unset clock it actually is. */
        char datebuf[28] = "";
        if (ent.mtime != 0) {
            struct tm tmv;
            localtime_r(&ent.mtime, &tmv);
            strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M", &tmv);
        }

        const std::string size = human_size(ent.size);
        make_row(s.list, LV_SYMBOL_FILE, ent.name, datebuf, size.c_str(), (int)i);
    }
}

} // namespace

bool open(const char *start_dir, const char *ext_filter,
          Callback on_pick, Callback on_cancel, void *user)
{
    if (start_dir == nullptr || on_pick == nullptr) {
        return false;
    }
    if (s.open) {
        close();
    }

    DIR *probe = opendir(start_dir);
    if (probe == nullptr) {
        ESP_LOGW(TAG, "start directory %s is not readable", start_dir);
        return false;
    }
    closedir(probe);

    s.filter = (ext_filter != nullptr) ? lower(ext_filter) : std::string();
    s.on_pick = on_pick;
    s.on_cancel = on_cancel;
    s.user = user;

    /* The system layer is above every app, so the picker covers the whole screen
     * without needing a screen of its own. Clickable so taps do not reach the app. */
    s.overlay = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s.overlay);
    lv_obj_set_size(s.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s.overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s.overlay, lv_color_hex(0x11161b), 0);
    lv_obj_set_style_bg_opa(s.overlay, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s.overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s.overlay, 16, 0);
    lv_obj_set_style_pad_row(s.overlay, 12, 0);
    lv_obj_clear_flag(s.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *header = lv_obj_create(s.overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 12, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s.up_btn = make_header_button(header, LV_SYMBOL_UP, on_up_click);

    s.path_label = lv_label_create(header);
    lv_obj_set_flex_grow(s.path_label, 1);
    lv_label_set_long_mode(s.path_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s.path_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s.path_label, start_dir);

    make_header_button(header, LV_SYMBOL_CLOSE, on_cancel_click);

    s.list = lv_obj_create(s.overlay);
    lv_obj_set_width(s.list, LV_PCT(100));
    lv_obj_set_flex_grow(s.list, 1);
    lv_obj_set_flex_flow(s.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s.list, 0, 0);
    lv_obj_set_style_pad_row(s.list, 8, 0);
    lv_obj_set_scroll_dir(s.list, LV_DIR_VER);

    s.open = true;
    navigate(start_dir);

    ESP_LOGI(TAG, "opened at %s (filter \"%s\")", start_dir,
             s.filter.empty() ? "*" : s.filter.c_str());
    return true;
}

void close(void)
{
    if (s.overlay != nullptr) {
        lv_obj_delete(s.overlay);
        s.overlay = nullptr;
    }
    s.list = nullptr;
    s.path_label = nullptr;
    s.up_btn = nullptr;
    s.entries.clear();
    s.open = false;
    s.on_pick = nullptr;
    s.on_cancel = nullptr;
    s.user = nullptr;
}

bool is_open(void)
{
    return s.open;
}

} // namespace file_picker
