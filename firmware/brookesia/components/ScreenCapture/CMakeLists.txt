# SPDX-License-Identifier: Apache-2.0
#
# Device-wide screen capture: PNG screenshots + MJPEG (AVI) screen recording.
idf_component_register(
    SRCS "src/screen_capture.c" "ScreenCaptureApp.cpp"
    INCLUDE_DIRS "include" "."
    REQUIRES
        brookesia_core
        lvgl__lvgl
        esp_lvgl_adapter
        esp_driver_jpeg
        esp_timer
        fatfs
        esp32_p4_wifi6_touch_lcd_4b
)
