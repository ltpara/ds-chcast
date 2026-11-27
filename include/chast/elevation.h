#pragma once
#include "chast/protocol.h"

#include <string>
#include <vector>

namespace chast {
// A decoded frame returned by a per-viewer elevated desktop capture source.
// Pixels are tightly packed, top-down BGRA and are bounded by the caller before
// they reach an encoder.
struct ElevationCaptureFrame final {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_number = 0;
    std::vector<uint8_t> bgra;
};

// A local UAC launch runs independently of video/network service threads.
// Restarting means the elevated child is authenticated and ready for handoff.
// The host sends that result before commit(); cancelling before commit must
// leave the original host serving and prevent the new child from taking over.
class IElevationTask {
public:
    virtual ~IElevationTask() = default;
    virtual ElevationState poll() = 0;
    virtual void commit() = 0;
    virtual void cancel() = 0;

    // Legacy whole-host elevation tasks do not expose capture. Portable helper
    // tasks opt in after their per-viewer WorkerReady proof has been verified.
    virtual bool capture_ready() const { return false; }
    virtual bool capture_frame(ElevationCaptureFrame*, std::string* error) {
        if (error) *error = "this elevation task has no per-viewer capture source";
        return false;
    }
};
} // namespace chast
