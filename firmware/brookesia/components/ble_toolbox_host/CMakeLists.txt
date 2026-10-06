# SPDX-License-Identifier: Apache-2.0
#
# BLE Toolbox — host side.
#
# Owns the NimBLE host that runs against the co-processor's controller, and the
# advertisement decoders the UI is built on. The radio is on the ESP32-C6: this chip
# has none (spec section 36), so BLE arrives over esp-hosted's HCI transport rather
# than from a local controller.
#
# Deliberately does not own a transmit path yet. The Tactility app's BLE Spam screen
# is a transmitter, and it belongs behind the same arming gate as the Wi-Fi toolbox's
# injection rather than bolted on to a scanner.

idf_component_register(
    SRCS
        "src/ble_toolbox_host.c"
        "src/ble_toolbox_adv_decode.c"
        "src/ble_toolbox_spam.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        bt
        VendorLookup
    PRIV_REQUIRES
        esp_hosted
        esp_timer
)

# The address formatter appends a " (rnd)" suffix to a fixed 18-byte field, which
# snprintf truncates safely; the reference tree built without -Werror, so relax it.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)

# The BT stack glue (esp_hosted_bt_host_stack) is an internal static library of
# esp_hosted that attaches itself to the host umbrella, so requiring esp_hosted here
# is what makes it link. It is not a component that can be named in REQUIRES.
