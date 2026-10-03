# SPDX-License-Identifier: Apache-2.0
#
# Wi-Fi Toolbox + BLE Toolbox phone apps, ported from the reference tree
# (ESP-IDF/11_esp_brookesia_phone/components/apps/{toolbox,bletoolbox,common}).
# Kept as one component because the two apps share common/toolbox_ui.hpp,
# common/toolbox_icons.hpp and the screenshot helper.

set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC} ${PROJ_SRC}/common ${PROJ_SRC}/common/include
    REQUIRES
        brookesia_core
        lvgl__lvgl
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_lvgl_adapter
        wifi_toolbox_host
        ble_toolbox_host
        lwip
        bsp_extra
        esp32_p4_wifi6_touch_lcd_4b
)

# The ported app code was written against the esp-brookesia 0.4.2 C++ API; the
# local brookesia_core keeps those names as deprecated typedefs, so the warnings
# are expected and silenced. The C++ warning relaxations mirror the reference
# apps/CMakeLists.txt.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-format
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wno-format
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
        -Wno-deprecated-declarations
    >
    -Wno-error=format-truncation
    -DLV_LVGL_H_INCLUDE_SIMPLE
)
