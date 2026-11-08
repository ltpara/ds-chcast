#pragma once

#include "chast/encoder.h"
#include <functional>
#include <memory>
#include <string>

namespace chast {

struct HostRecordingOptions {
    bool enabled = false;
    // Explicit absolute UTF-8 directory, chosen by the local operator.
    std::string directory;
    // Value-owned callback; must remain valid until the shared source stops.
    std::function<void(const std::string&)> log;
};

// One bounded disk worker per shared source. Only compressed bytes are copied;
// recording never retains capture textures or waits for disk on submit().
class StreamRecorder {
public:
    StreamRecorder(HostRecordingOptions options, const std::wstring& source);
    ~StreamRecorder();
    StreamRecorder(const StreamRecorder&) = delete;
    StreamRecorder& operator=(const StreamRecorder&) = delete;
    bool needs_keyframe(uint64_t pts_ms) const;
    // Called serially by the source thread. A discontinuity starts a new MP4
    // at the next IDR, including after queue overflow or a capture rebuild.
    void submit(VideoCodec codec, uint32_t width, uint32_t height, uint64_t generation,
                const std::vector<EncodedPacket>& packets, VideoChroma chroma = VideoChroma::Yuv420);
    // Drains and finalizes; only teardown waits for disk. pts uses the same
    // clock as packets, preserving the duration of an unchanged last image.
    void finish(uint64_t end_pts_ms);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chast
