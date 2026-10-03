/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Toolbox — host-side view of the shared protocol.
 *
 * The protocol header itself lives with the co-processor component, at
 * components/wifi_toolbox/include/wifi_toolbox_rpc.h, and is included here rather
 * than copied.
 *
 * It used to be a copy, kept honest by a hash comparison in this component's
 * CMakeLists.txt that warned when the two drifted. That was a mitigation, not a
 * fix: the co-processor is flashed separately from the app, so a drifted header is
 * a wire-format mismatch, and the version handshake only catches it when somebody
 * remembers to bump the version. One file, reachable from both sides, removes the
 * failure mode instead of reporting it.
 */
#pragma once

#include "../../wifi_toolbox/include/wifi_toolbox_rpc.h"
