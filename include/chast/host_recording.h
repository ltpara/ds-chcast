#pragma once

#include <cstdint>
#include <string>

namespace chast {

// Recording stops before the volume fills: the floor leaves room for the final
// MP4 trailer and for the rest of the machine to keep working.
inline constexpr uint64_t kMinRecordingFreeBytes = 512ull * 1024ull * 1024ull;

// Reports whether the volume holding `utf8_directory`, or its nearest existing
// parent, has at least `min_free_bytes` available. A volume that cannot be
// queried is treated as usable: this guard must not disable recording on
// shares or filesystems that do not report free space.
bool recording_disk_available(const std::string& utf8_directory, uint64_t min_free_bytes,
                              std::string* reason);

}  // namespace chast
