# SPDX-License-Identifier: Apache-2.0
#
# Shared file picker: browses the SD card tree and returns a chosen file's path.
idf_component_register(
    SRCS "FilePicker.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        lvgl__lvgl
        fatfs
        # Refuse to browse a card a USB host currently owns.
        SdShare
)
