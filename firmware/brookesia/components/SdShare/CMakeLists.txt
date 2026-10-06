# SPDX-License-Identifier: Apache-2.0
#
# Presents the SD card to a USB host as a removable drive, automatically on plug-in.
idf_component_register(
    SRCS "sd_share.c"
    INCLUDE_DIRS "include"
    REQUIRES
        # Vendored locally under components/esp_tinyusb (with the disk-name patch).
        esp_tinyusb
        esp32_p4_wifi6_touch_lcd_4b
        esp_driver_sdmmc
        esp_hw_support
        VendorLookup
)
