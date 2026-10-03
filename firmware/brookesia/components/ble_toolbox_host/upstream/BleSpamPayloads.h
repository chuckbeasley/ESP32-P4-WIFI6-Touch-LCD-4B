#pragma once

#include <cstddef>
#include <cstdint>

namespace tt::app::bletoolbox {

/** Number of packet families (Apple, Android, Windows, Samsung). */
static constexpr size_t kFamilyCount = 4;

/** Human-readable label for one packet family. */
struct Family {
    const char* label;
};

/** One raw BLE advertisement payload (full AD structure bytes). */
struct Payload {
    uint8_t family; // index into kFamilies
    const char* label;
    const uint8_t* data;
    size_t length;
};

extern const Family kFamilies[kFamilyCount];
extern const Payload kPayloads[];
extern const size_t kPayloadCount;

} // namespace tt::app::bletoolbox
