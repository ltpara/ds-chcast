#include "shared_viewer_session.h"

#include <algorithm>

namespace chast_viewer {

SharedViewerSession::SharedViewerSession() : viewer(&primary, &camera) {}
SharedViewerSession::~SharedViewerSession() { viewer.Stop(); }

void SharedViewerSession::InvalidateWindows() {
    {
        std::lock_guard lock(input_mutex_);
        ++generation;
        viewer.ReleaseInput();
        input_owner_ = nullptr;
    }
    for (const auto& entry : windows) PostMessageW(entry.second, WM_CLOSE, 0, 0);
    windows.clear();
    for (const auto& entry : displays) entry.second->ClearOutputs();
    displays.clear();
    cursors.clear();
}

void SharedViewerSession::Prune(const ViewerStatus& status) {
    const auto contains = [&](uint32_t id) {
        return status.state == ViewerState::kStreaming && status.multiple_displays &&
            std::any_of(status.display_streams.begin(), status.display_streams.end(),
                        [id](const auto& item) { return item.stream.stream_id == id; });
    };
    for (auto it = displays.begin(); it != displays.end();) {
        if (contains(it->first)) { ++it; continue; }
        it->second->ClearOutputs();
        it = displays.erase(it);
    }
    for (auto it = windows.begin(); it != windows.end();) {
        if (contains(it->first)) { ++it; continue; }
        PostMessageW(it->second, WM_CLOSE, 0, 0);
        it = windows.erase(it);
    }
    for (auto it = cursors.begin(); it != cursors.end();) {
        if (contains(it->first)) ++it;
        else it = cursors.erase(it);
    }
}

void SharedViewerSession::Detach(const void* owner) {
    for (auto it = displays.begin(); it != displays.end();) {
        it->second->SetOutputForOwner(owner, nullptr);
        if (it->second->HasOutputs()) { ++it; continue; }
        viewer.BindDisplay(it->first, nullptr);
        it = displays.erase(it);
    }
}

bool SharedViewerSession::SendInput(const void* owner, uint64_t epoch,
    std::optional<uint32_t> stream, HWND window, const chast::InputEvent& event) {
    std::lock_guard lock(input_mutex_);
    if (epoch != generation.load() || GetForegroundWindow() != window) return false;
    if (stream) {
        // Pointer coordinates already select the remote display. The child's
        // local focus must not change the parent's screenshot/recording source.
        const auto status = viewer.Snapshot();
        if (status.state != ViewerState::kStreaming || !status.multiple_displays ||
            std::none_of(status.display_streams.begin(), status.display_streams.end(),
                [&](const auto& item) { return item.stream.stream_id == *stream; })) return false;
    }
    if (input_owner_ != owner) {
        if (input_owner_) viewer.ReleaseInput();
        input_owner_ = owner;
    }
    return viewer.SendInput(event);
}

bool SharedViewerSession::ReleaseInput(const void* owner, uint64_t epoch) {
    std::lock_guard lock(input_mutex_);
    // A delayed blur/close from an old window must not release the new owner's keys.
    if (epoch != generation.load() || input_owner_ != owner) return false;
    input_owner_ = nullptr;
    return viewer.ReleaseInput();
}

bool SharedViewerSession::SendText(const void* owner, uint64_t epoch,
    std::optional<uint32_t> stream, HWND window, const std::string& text) {
    std::lock_guard lock(input_mutex_);
    if (epoch != generation.load() || GetForegroundWindow() != window ||
        !chast::valid_text_input(text)) return false;
    const auto status = viewer.Snapshot();
    if (status.state != ViewerState::kStreaming || !status.input_enabled) return false;
    if (stream && (!status.multiple_displays ||
        std::none_of(status.display_streams.begin(), status.display_streams.end(),
            [&](const auto& item) { return item.stream.stream_id == *stream; }))) return false;
    // Text is a committed character sequence. Previously held shortcut or
    // pointer modifiers must not turn it into another remote command.
    viewer.ReleaseInput();
    input_owner_ = owner;
    return viewer.SendText(text);
}

void SharedViewerSession::CollectCursors() {
    chast::DisplayCursorUpdate update;
    while (viewer.TakeDisplayCursor(update)) {
        auto& cached = cursors[update.stream_id];
        // Movement packets may omit the sprite. Keep it for engines opened later.
        if (update.cursor.sprite.empty() &&
            update.cursor.shape_id == cached.value.shape_id) {
            update.cursor.sprite = std::move(cached.value.sprite);
        }
        cached.value = std::move(update.cursor);
        cached.revision = ++cursor_revision_;
    }
}

}  // namespace chast_viewer
