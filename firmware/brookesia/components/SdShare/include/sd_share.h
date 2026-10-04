/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Presents the SD card to a USB host as a removable drive, automatically on plug-in, so
 * media can be copied on and off without a card reader.
 *
 * The device and the host cannot both own the card: the host mounts its own FAT driver
 * over the raw sectors, so while it holds the card the device must not touch /sdcard.
 * sd_share_host_owns_card() is that gate, and the capture paths check it.
 *
 * The BSP stays the only thing that mounts the filesystem. esp_tinyusb is created with
 * auto_mount off, which stops it mounting /sdcard behind the BSP's back; every ownership
 * transition is instead driven from the USB attach/detach events.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the USB device stack and the MSC backing store. Call once the SD card has been
 * mounted by the BSP. Idempotent. */
esp_err_t sd_share_start(void);

/* True while the card belongs to the USB host. /sdcard is not mounted for this device
 * during that window and must not be opened, written, or even scanned. */
bool sd_share_host_owns_card(void);

#ifdef __cplusplus
}
#endif
