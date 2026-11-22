#pragma once

namespace chast_viewer {

enum class ViewerAwakeRequestAction { Noop, Acquire, Release, Reject };

constexpr ViewerAwakeRequestAction viewer_awake_request_action(
    bool enabled, bool held, bool session_active) {
    if (!enabled) return held ? ViewerAwakeRequestAction::Release
                              : ViewerAwakeRequestAction::Noop;
    if (held) return ViewerAwakeRequestAction::Noop;
    return session_active ? ViewerAwakeRequestAction::Acquire
                          : ViewerAwakeRequestAction::Reject;
}

bool AcquireViewerAwakeRequest();
bool ReleaseViewerAwakeRequest();

}  // namespace chast_viewer
