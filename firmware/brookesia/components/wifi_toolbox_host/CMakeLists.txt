# SPDX-License-Identifier: Apache-2.0
#
# Wi-Fi Toolbox — host side.
#
# Owns the custom-RPC channel to the co-processor's toolbox, the PCAP writer that
# consumes the frames it streams, and the net utilities. Nothing here talks to a
# radio: on this target the radio is on the ESP32-C6, and the host's esp_wifi_remote
# entry points for the operations that matter are weak UNSUPPORTED stubs.

idf_component_register(
    SRCS
        "src/wifi_toolbox_host.c"
        "src/wifi_toolbox_pcap.c"
        "src/wifi_toolbox_net.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        esp_netif
        lwip
    PRIV_REQUIRES
        esp_hosted
        esp_wifi
        fatfs
        vfs
)

# The protocol header is not duplicated: include/wifi_toolbox_rpc.h includes the one
# that lives with the co-processor component (../wifi_toolbox/include/). It is
# included rather than linked, on purpose — this side must not depend on the
# co-processor component, which is inert in this build.
#
# There used to be a copy here with a SHA256 comparison against the master. That
# reported drift; sharing one file removes it.

