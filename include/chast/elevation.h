#pragma once
#include "chast/protocol.h"

namespace chast {
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
};
} // namespace chast
