#pragma once

#include <windows.h>
#include <array>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>
#include "chast/input.h"

namespace chast_viewer {

enum class RemoteShortcutAction : uint8_t {
    kToggleControl,
    kToggleFullscreen,
    kToggleRelativeMouse,
    kToggleStats,
    kLeaveSession,
};

struct RemoteShortcutBinding {
    RemoteShortcutAction action = RemoteShortcutAction::kToggleControl;
    uint16_t key_usage = 0;
    // Ctrl=1, Alt=2, Shift=4.
    uint8_t modifiers = 0;
};

constexpr bool valid_remote_shortcut(int64_t usage, int64_t modifiers) {
    // Keep this in sync with RemoteShortcutBinding in remote_shortcuts.dart.
    return usage >= 0x04 && usage < 0xe0 && usage != 0x29 && usage != 0x39 &&
        usage != 0x47 && usage != 0x53 && modifiers >= 1 && modifiers <= 7 &&
        !((usage == 0x2b || usage == 0x3d) && (modifiers & 2)) &&
        !(usage == 0x29 && modifiers == 5) &&
        !(usage == 0x2b && (modifiers & 3) == 3) &&
        usage != 0x2c && !(usage == 0x4c && modifiers == 3) &&
        !(usage == 0x15 && modifiers == 7);
}

inline constexpr int kNoShortcutMatched = -1;
inline constexpr int kShortcutKeySuppressed = -2;

// Pure event state, shared by the hook and regression tests. Synthetic AltGr
// Ctrl is discarded before a physical modifier bitmap is produced.
class KeyboardState {
public:
    void Reset(uint16_t modifiers = 0);
    bool Handle(const KBDLLHOOKSTRUCT& key, bool down, uint16_t locks,
                bool relative, chast::InputEvent& event, bool& escape,
                int& shortcut_action,
                bool translated = false, bool composing = false);
    void SetShortcuts(std::vector<RemoteShortcutBinding> shortcuts) {
        shortcuts_ = std::move(shortcuts);
    }
    // Legacy mode emits virtual-key events instead of HID usages. Switching
    // modes must be followed by a Reset; the bridge releases remote input and
    // resets through SetLegacy().
    void SetLegacy(bool enabled) { legacy_ = enabled; }
    uint16_t modifiers() const { return modifiers_; }
private:
    uint16_t modifiers_ = 0;
    std::array<bool, 256> held_{};
    std::array<bool, 256> legacy_held_{};
    std::array<bool, 256> forwarded_{};
    std::array<bool, 256> shortcut_suppressed_{};
    std::vector<RemoteShortcutBinding> shortcuts_;
    bool escape_held_ = false;
    bool legacy_ = false;
};

// A dedicated message thread keeps Windows' hook deadline independent of UI
// rendering. Remote input and shortcut-only capture have separate ownership.
class KeyboardCapture {
public:
    using LocalDispatch = std::function<bool(const KBDLLHOOKSTRUCT&, bool,
                                             const chast::InputEvent&, bool)>;
    using ShortcutDispatch = std::function<void(RemoteShortcutAction, uint32_t)>;
    KeyboardCapture(HWND window, std::function<bool(const chast::InputEvent&)> send,
                    std::function<void()> release, std::function<void()> escape,
                    LocalDispatch local_dispatch = {},
                    ShortcutDispatch shortcut_dispatch = {});
    ~KeyboardCapture();
    bool Enable(bool enabled);
    bool EnableShortcuts(bool enabled);
    bool available() const;
    bool enabled() const;
    bool shortcuts_enabled() const;
    uint16_t InputState() const;
    void Release();
    void set_relative(bool enabled);
    void SetTranslated(bool enabled);
    void SetLegacy(bool enabled);
    void SetComposing(bool composing);
    void SetShortcuts(std::vector<RemoteShortcutBinding> shortcuts, uint32_t revision);
private:
    void Run();
    void UpdateForeground(bool foreground);  // mutex_ must be held.
    static LRESULT CALLBACK Hook(int code, WPARAM message, LPARAM data);
    bool Handle(WPARAM message, const KBDLLHOOKSTRUCT& key);
    HWND window_;
    std::function<bool(const chast::InputEvent&)> send_;
    std::function<void()> release_;
    std::function<void()> escape_;
    LocalDispatch local_dispatch_;
    ShortcutDispatch shortcut_dispatch_;
    KeyboardState state_;
    mutable std::mutex mutex_;
    std::condition_variable ready_cv_;
    std::thread thread_;
    DWORD thread_id_ = 0;
    bool ready_ = false;
    bool hooked_ = false;
    uint16_t locks_ = 0;
    uint16_t physical_modifiers_ = 0;
    std::array<bool, 256> local_held_{};
    bool was_foreground_ = false;
    bool enabled_ = false;
    bool shortcut_capture_enabled_ = false;
    bool relative_ = false;
    bool translated_ = false;
    bool legacy_ = false;
    bool composing_ = false;
    uint32_t shortcut_revision_ = 0;
    // Keep the matching up/repeats local even if the action disables capture.
    std::array<bool, 256> shortcut_held_{};
};
}  // namespace chast_viewer
