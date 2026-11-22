#pragma once

#include <cstdint>
#include <string>

namespace chast {

struct HostRecordingSegment {
    std::string path;
    uint64_t started_at_ms = 0;
    uint64_t duration_ms = 0;
};

// Publish one successfully sealed MP4 as an immutable task for chast-tunnel.
// The manifest becomes visible only after its contents have been flushed.
bool enqueue_host_recording_upload(const std::string& queue_directory,
                                   const HostRecordingSegment& segment,
                                   std::string* error);

}  // namespace chast
