#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "gpu_frame_sink.h"

namespace chast_viewer {

// Window registration, outputs and cursor caches live on the runner thread.
// The keyboard hooks use SendInput/ReleaseInput from their own threads.
class SharedViewerSession {
public:
    SharedViewerSession();
    ~SharedViewerSession();
    void InvalidateWindows();
    void Prune(const ViewerStatus& status);
    void Detach(const void* owner);
    bool SendInput(const void* owner, uint64_t epoch, std::optional<uint32_t> stream,
                   HWND window, const chast::InputEvent& event);
    bool ReleaseInput(const void* owner, uint64_t epoch);
    bool SendText(const void* owner, uint64_t epoch, std::optional<uint32_t> stream,
                  HWND window, const std::string& text);
    void CollectCursors();

    struct Cursor {
        chast::CursorUpdate value;
        uint64_t revision = 0;
    };
    std::atomic<uint64_t> generation{1};
    std::map<uint32_t, HWND> windows;
    std::map<uint32_t, std::shared_ptr<GpuFrameSink>> displays;
    std::map<uint32_t, Cursor> cursors;
    GpuFrameSink primary;
    GpuFrameSink camera;
    ViewerEngine viewer;

private:
    std::mutex input_mutex_;
    const void* input_owner_ = nullptr;
    uint64_t cursor_revision_ = 0;
};

}  // namespace chast_viewer
