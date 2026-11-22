#pragma once

#include <windows.h>

namespace chast_viewer {

inline bool view_owns_active_focus(HWND window, HWND foreground,
                                   HWND focused) {
    if (window == nullptr || !IsWindow(window) || !IsWindowVisible(window) ||
        focused == nullptr) return false;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (root == nullptr || foreground != root) return false;
    return focused == window || IsChild(window, focused);
}

inline bool thread_focus_is_within(HWND window, DWORD thread_id) {
    if (window == nullptr || thread_id == 0) return false;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!GetGUIThreadInfo(thread_id, &info)) return false;
    const HWND focused = info.hwndFocus;
    return focused == window || (focused != nullptr && IsChild(window, focused));
}

inline bool active_view_has_focus(HWND window) {
    if (window == nullptr || !IsWindow(window) || !IsWindowVisible(window))
        return false;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (root == nullptr) return false;
    const DWORD thread_id = GetWindowThreadProcessId(root, nullptr);
    if (thread_id == 0) return false;
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(thread_id, &info) &&
           view_owns_active_focus(window, GetForegroundWindow(), info.hwndFocus);
}

}  // namespace chast_viewer
