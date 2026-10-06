# SPDX-License-Identifier: Apache-2.0
#
# Vendor identification for scanned Wi-Fi/BLE devices. The ~40k-entry OUI table, the
# BLE company/service/appearance tables, the Fast Pair list and the classification
# rules are all parsed at runtime from vendor_db.json (see scripts/gen_vendor_table.py).
idf_component_register(
    SRCS
        "vendor_lookup.c"
        "vendor_signature.c"
    INCLUDE_DIRS
        "."
)
