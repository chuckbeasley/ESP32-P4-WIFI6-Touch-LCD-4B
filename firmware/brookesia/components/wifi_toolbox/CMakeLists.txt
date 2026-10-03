# SPDX-License-Identifier: Apache-2.0
#
# Wi-Fi Toolbox — co-processor (ESP32-C6) side.
#
# This is the radio half: frame builders, the injection engine, the promiscuous
# capture engine, the shared radio bring-up and the custom-RPC handler. It runs on
# the C6, so it is deliberately INERT in the application build — the application
# builds for esp32p4, which has no radio.
#
# The co-processor project at ../c6_firmware/ consumes this directory through a
# junction named `wifi_toolbox` in its own components/ tree, the same mechanism that
# points it at this tree's managed_components/espressif__esp_hosted. So there is one
# copy of this code, in the application's tree, and the C6 firmware cannot drift
# from it.
#
# The guard below is what keeps this component inert in the application build. It
# keys on IDF_TARGET rather than on CONFIG_ESP_HOSTED_CP, and that is a build-system
# constraint rather than a preference: a component's CMakeLists.txt is processed
# before sdkconfig is loaded, so CONFIG_* symbols do not exist yet at this point.
# IDF_TARGET does.
#
# The radio code is esp32c6-only, and this component is only ever reached from the
# esp32c6 project, so the target is the accurate statement of what this is: on any
# other target the component registers nothing, none of its headers reach the
# build, and the co-processor half stays out of the application image by
# construction.
#
# Two build shapes, both real (spec section 10.5):
#
#   CONFIG_WIFI_TOOLBOX_ALLOW_INJECTION=y   the tool image: builders, injection
#                                           engine, capture engine, custom RPC
#   CONFIG_WIFI_TOOLBOX_ALLOW_INJECTION=n   the shipped image: capture only, no
#                                           frame builders and no way to transmit
#
# The frame builders have no IDF dependency beyond esp_random(), so they can also
# be compiled and byte-compared on a host for the golden-byte tests required by
# BrookesiaSpec section 11.

if(NOT IDF_TARGET STREQUAL "esp32c6")
    # Application build: nothing to register. Said once at configure time so an
    # empty component is not mistaken for a broken one.
    message(STATUS
        "wifi_toolbox: esp32c6 co-processor component, skipped on target '${IDF_TARGET}' — "
        "it is built by the C6 project at c6_firmware/ through a junction to this directory")
    return()
endif()

set(WIFI_TOOLBOX_SRCS
    "src/wifi_toolbox_capture.c"
    "src/wifi_toolbox_capture_engine.c"
    "src/wifi_toolbox_radio.c"
    "src/wifi_toolbox_rpc.c"
)

if(CONFIG_WIFI_TOOLBOX_ALLOW_INJECTION)
    list(APPEND WIFI_TOOLBOX_SRCS
        "src/wifi_toolbox_frames.c"
        "src/wifi_toolbox_inject.c"
    )
endif()

idf_component_register(
    SRCS
        ${WIFI_TOOLBOX_SRCS}
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_wifi
    PRIV_REQUIRES
        esp_hosted
)

# The co-processor peer-data feature is an internal target of the esp_hosted
# component (not part of its exported API), so link it explicitly. Its header is
# the only route to the custom-RPC channel that carries this toolbox's commands.
#
# Its callback table is fixed-size and defaults to 8 (eh_cp_feat_peer_data.c),
# because CONFIG_ESP_HOSTED_CP_MAX_PEER_DATA_HANDLERS is not defined by any Kconfig
# in the shipped component. Registrations past the end fail at runtime and the only
# evidence is a log line on the co-processor's console, which is not wired to the
# host — from there the symptom is a command that never answers.
#
# wifi_toolbox_rpc.c asserts the toolbox's command count against that size at
# compile time; if esp_hosted ever defines the Kconfig symbol, that assert is the
# one place to revisit.
if(TARGET eh_cp_feat_peer_data)
    target_link_libraries(${COMPONENT_LIB} PRIVATE eh_cp_feat_peer_data)
else()
    message(WARNING
        "eh_cp_feat_peer_data target not found — the Wi-Fi Toolbox custom-RPC "
        "channel will not be available. Ensure CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA=y.")
endif()
