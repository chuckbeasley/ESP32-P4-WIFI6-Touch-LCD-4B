/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A shared file picker: walks the SD card's directory tree and hands back the path of
 * a chosen file. Modal — it draws on the system layer above whatever app is running,
 * so any app can borrow it without owning a screen.
 *
 * It is deliberately a plain C++ API rather than an app: the caller keeps its own
 * screen and just gets a path back.
 */
#pragma once

#include "lvgl.h"

namespace file_picker {

/* Fired with the picked full path, or with nullptr on cancel. */
typedef void (*Callback)(const char *path, void *user);

/* Show the picker over the current screen.
 *
 *   start_dir   directory to open, e.g. "/sdcard"
 *   ext_filter  one extension to list, e.g. ".png" (case-insensitive). nullptr lists
 *               every file. Directories are always listed so the user can descend.
 *   on_pick     called with the chosen file's full path
 *   on_cancel   called when the user backs out; may be nullptr
 *   user        passed through to both callbacks
 *
 * Returns false if start_dir cannot be opened, or if a picker is already up. */
bool open(const char *start_dir, const char *ext_filter,
          Callback on_pick, Callback on_cancel, void *user);

/* Dismiss without firing a callback. Safe to call when not open. */
void close(void);

bool is_open(void);

} // namespace file_picker
