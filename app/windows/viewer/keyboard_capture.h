#pragma once

#include <windows.h>
#include <array>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "chast/input.h"

namespace chast_viewer {

// Pure event state, shared by the hook and regression tests. Synthetic AltGr
// Ctrl is discarded before a physical modifier bitmap is produced.
class KeyboardState {
public:
    void Reset(uint16_t modifiers = 0);
    bool Handle(const KBDLLHOOKSTRUCT& key, bool down, uint16_t locks,
                bool relative, chast::InputEvent& event, bool& escape,
                bool translated = false, bool composing = false);
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
    bool escape_held_ = false;
    bool legacy_ = false;
};

// A dedicated message thread keeps Windows' hook deadline independent of UI
// rendering. Only the foreground window with remote focus owns keyboard input.
class KeyboardCapture {
public:
    using LocalDispatch = std::function<bool(const KBDLLHOOKSTRUCT&, bool,
                                             const chast::InputEvent&, bool)>;
    KeyboardCapture(HWND window, std::function<bool(const chast::InputEvent&)> send,
                    std::function<void()> release, std::function<void()> escape,
                    LocalDispatch local_dispatch = {});
    ~KeyboardCapture();
    bool Enable(bool enabled);
    bool available() const;
    bool enabled() const;
    uint16_t InputState() const;
    void Release();
    void set_relative(bool enabled);
    void SetTranslated(bool enabled);
    void SetLegacy(bool enabled);
    void SetComposing(bool composing);
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
    bool relative_ = false;
    bool translated_ = false;
    bool legacy_ = false;
    bool composing_ = false;
};
}  // namespace chast_viewer
