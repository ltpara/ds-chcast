#include "keyboard_capture.h"

#include <utility>

namespace chast_viewer {
namespace {
thread_local KeyboardCapture* capture_on_thread = nullptr;

uint16_t lock_bits() {
    return ((GetKeyState(VK_CAPITAL) & 1) ? chast::kLockCaps : 0) |
           ((GetKeyState(VK_NUMLOCK) & 1) ? chast::kLockNum : 0) |
           ((GetKeyState(VK_SCROLL) & 1) ? chast::kLockScroll : 0);
}

uint16_t physical_modifiers() {
    constexpr int keys[] = {VK_LCONTROL, VK_LSHIFT, VK_LMENU, VK_LWIN,
                            VK_RCONTROL, VK_RSHIFT, VK_RMENU, VK_RWIN};
    uint16_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (GetAsyncKeyState(keys[i]) & 0x8000) bits |= static_cast<uint16_t>(1u << i);
    }
    return bits;
}
}  // namespace

void KeyboardState::Reset(uint16_t modifiers) {
    held_.fill(false);
    forwarded_.fill(false);
    modifiers_ = modifiers & chast::kModifierMask;
    for (unsigned i = 0; i < 8; ++i) held_[0xE0 + i] = (modifiers_ & (1u << i)) != 0;
    escape_held_ = false;
}

bool KeyboardState::Handle(const KBDLLHOOKSTRUCT& key, bool down, uint16_t locks,
                           bool relative, chast::InputEvent& event, bool& escape,
                           bool translated, bool composing) {
    escape = false;
    event = chast::InputEvent{};
    // The hook sees the special 0x21D provenance which Flutter HID events
    // discard. Do not infer AltGr from a real Ctrl+RightAlt combination.
    if (key.scanCode == 0x021D) return !translated;
    if (key.vkCode == VK_CAPITAL || key.vkCode == VK_NUMLOCK) return false;
    const uint16_t usage = chast::windows_key_to_hid(
        key.vkCode, key.scanCode, (key.flags & LLKHF_EXTENDED) != 0);
    if (usage == 0) return false;
    const bool repeat = down && held_[usage];
    held_[usage] = down;
    const uint16_t modifier = chast::modifier_bit_for_usage(usage);
    if (down) modifiers_ |= modifier;
    else modifiers_ &= static_cast<uint16_t>(~modifier);
    if (modifier != 0 && repeat) return true;
    if (usage == 0x15) {  // Ctrl+Alt+Shift+R releases relative mouse capture.
        if (escape_held_) {
            if (!down) escape_held_ = false;
            return true;
        }
        if (down && relative &&
            (modifiers_ & (chast::kModifierLeftCtrl | chast::kModifierRightCtrl)) &&
            (modifiers_ & (chast::kModifierLeftAlt | chast::kModifierRightAlt)) &&
            (modifiers_ & (chast::kModifierLeftShift | chast::kModifierRightShift))) {
            escape_held_ = true;
            escape = true;
            return true;
        }
    }
    if (translated) {
        if (modifier != 0) {
            // Modifiers must reach the local layout/IME. A remote shortcut
            // carries their bitmap; its modifier releases still reach the host.
            const bool win = usage == 0xe3 || usage == 0xe7;
            if (!down || win) {
                event.kind = chast::InputKind::Key;
                event.key = {down ? chast::KeyAction::Down : chast::KeyAction::Up, usage};
                event.modifiers = modifiers_ | (locks & chast::kLockStateMask);
            }
            return win;
        }
        if (!down) {
            if (!forwarded_[usage]) return false;
            forwarded_[usage] = false;
        } else if (!repeat) {
            const bool altgr = (modifiers_ & chast::kModifierRightAlt) != 0;
            const bool ctrl = (modifiers_ & (chast::kModifierLeftCtrl | chast::kModifierRightCtrl)) != 0;
            const bool alt = (modifiers_ & chast::kModifierLeftAlt) != 0;
            const bool win = (modifiers_ & (chast::kModifierLeftGui | chast::kModifierRightGui)) != 0;
            const bool shift = (modifiers_ & (chast::kModifierLeftShift | chast::kModifierRightShift)) != 0;
            const bool local_switch = key.vkCode == VK_SPACE && !win && (ctrl || shift);
            const bool shortcut = (ctrl && !altgr) || alt || win;
            // MAPVK_VK_TO_CHAR includes the high bit for dead keys. Do not
            // call ToUnicode here: it would consume the layout's dead-key state.
            const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
            const UINT character = MapVirtualKeyExW(key.vkCode, MAPVK_VK_TO_CHAR, layout) & 0x7fffffffu;
            const bool text = (character >= 0x20 && character != 0x7f) ||
                              key.vkCode == VK_PACKET;
            forwarded_[usage] = !composing && !local_switch && (shortcut || !text);
        }
        if (down && !forwarded_[usage]) return false;
    }
    event.kind = chast::InputKind::Key;
    event.key.hid_usage = usage;
    event.key.action = down ? chast::KeyAction::Down : chast::KeyAction::Up;
    event.modifiers = modifiers_ | (locks & chast::kLockStateMask);
    return true;
}

KeyboardCapture::KeyboardCapture(HWND window,
    std::function<bool(const chast::InputEvent&)> send,
    std::function<void()> release, std::function<void()> escape, LocalDispatch local_dispatch)
    : window_(window), send_(std::move(send)), release_(std::move(release)),
      escape_(std::move(escape)), local_dispatch_(std::move(local_dispatch)) {
    locks_ = lock_bits();
    physical_modifiers_ = physical_modifiers();
    for (const int key : {VK_CAPITAL, VK_NUMLOCK, VK_SCROLL}) {
        local_held_[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    thread_ = std::thread(&KeyboardCapture::Run, this);
    std::unique_lock<std::mutex> lock(mutex_);
    ready_cv_.wait(lock, [this] { return ready_; });
}

KeyboardCapture::~KeyboardCapture() {
    Enable(false);
    if (thread_id_ != 0) PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
}

bool KeyboardCapture::Enable(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled == enabled_) return enabled_;
    if (!enabled) {
        enabled_ = false;
        was_foreground_ = false;
        state_.Reset();
        release_();
        return false;
    }
    if (!hooked_) return false;
    locks_ = lock_bits();  // Called on the window's input thread.
    // The hook observes keys even while capture is off. Resampling async
    // state here would lose consumed downs and resurrect AltGr's fake Ctrl.
    state_.Reset(physical_modifiers_);
    was_foreground_ = GetForegroundWindow() == GetAncestor(window_, GA_ROOT);
    enabled_ = true;
    return true;
}

void KeyboardCapture::Run() {
    capture_on_thread = this;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, Hook, GetModuleHandleW(nullptr), 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        thread_id_ = GetCurrentThreadId();
        hooked_ = hook != nullptr;
        ready_ = true;
    }
    ready_cv_.notify_one();
    const UINT_PTR timer = SetTimer(nullptr, 0, 50, nullptr);
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_TIMER) {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool foreground = GetForegroundWindow() == GetAncestor(window_, GA_ROOT);
            UpdateForeground(foreground);
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (timer != 0) KillTimer(nullptr, timer);
    if (hook != nullptr) UnhookWindowsHookEx(hook);
    capture_on_thread = nullptr;
}

void KeyboardCapture::UpdateForeground(bool foreground) {
    if (enabled_ && foreground != was_foreground_) {
        if (foreground) {
            state_.Reset(physical_modifiers_);
        } else {
            state_.Reset();
            release_();
        }
    }
    was_foreground_ = foreground;
}

bool KeyboardCapture::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hooked_;
}

bool KeyboardCapture::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void KeyboardCapture::set_relative(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    relative_ = enabled;
}

void KeyboardCapture::SetTranslated(bool enabled) {
    std::lock_guard lock(mutex_);
    translated_ = enabled;
    composing_ = false;
    state_.Reset(physical_modifiers_);
}

void KeyboardCapture::SetComposing(bool composing) {
    std::lock_guard lock(mutex_);
    composing_ = composing;
}

uint16_t KeyboardCapture::InputState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (enabled_ ? state_.modifiers() : physical_modifiers_) | locks_;
}

void KeyboardCapture::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.Reset();
    was_foreground_ = false;
    release_();
}

LRESULT CALLBACK KeyboardCapture::Hook(int code, WPARAM message, LPARAM data) {
    bool consume = false;
    if (code == HC_ACTION) {
        const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        // A local loopback host's injected events must not get sent back.
        if (key.dwExtraInfo != chast::kInputInjectionTag) {
            consume = capture_on_thread != nullptr && capture_on_thread->Handle(message, key);
        }
    }
    // Other session windows need to observe physical state even when this
    // window owns the key. Blocking delivery to Windows happens after all
    // hooks have observed it, without holding this capture's mutex.
    const LRESULT next = CallNextHookEx(nullptr, code, message, data);
    return consume ? 1 : next;
}

bool KeyboardCapture::Handle(WPARAM message, const KBDLLHOOKSTRUCT& key) {
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    if (!down && message != WM_KEYUP && message != WM_SYSKEYUP) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const bool foreground = GetForegroundWindow() == GetAncestor(window_, GA_ROOT);
    UpdateForeground(foreground);
    const uint16_t usage = chast::windows_key_to_hid(
        key.vkCode, key.scanCode, (key.flags & LLKHF_EXTENDED) != 0);
    const uint16_t modifier = chast::modifier_bit_for_usage(usage);
    if (down) physical_modifiers_ |= modifier;
    else physical_modifiers_ &= static_cast<uint16_t>(~modifier);
    if (key.vkCode < local_held_.size()) {
        if (down && !local_held_[key.vkCode]) {
            if (key.vkCode == VK_CAPITAL) locks_ ^= chast::kLockCaps;
            if (key.vkCode == VK_NUMLOCK) locks_ ^= chast::kLockNum;
            if (key.vkCode == VK_SCROLL) locks_ ^= chast::kLockScroll;
        }
        local_held_[key.vkCode] = down;
    }
    const bool owns = enabled_ && foreground;
    if (!owns) {
        // Another session's hook must see its releases. An inactive window
        // must never consume input intended for the foreground owner.
        return false;
    }
    chast::InputEvent event;
    bool escape = false;
    bool consume = state_.Handle(key, down, locks_, relative_, event, escape,
                                 translated_, composing_);
    if (translated_ && local_dispatch_ && consume && event.kind == chast::InputKind::Key) {
        // Ordinary keys go through IMM before the window decides whether they
        // belong to composition. A fast Enter can precede STARTCOMPOSITION on
        // this hook thread. Only OS shortcuts need to be suppressed here.
        const auto mods = state_.modifiers();
        const bool win = usage == 0xe3 || usage == 0xe7 ||
            (mods & (chast::kModifierLeftGui | chast::kModifierRightGui)) != 0;
        const bool alt = (mods & (chast::kModifierLeftAlt | chast::kModifierRightAlt)) != 0;
        const bool ctrl = (mods & (chast::kModifierLeftCtrl | chast::kModifierRightCtrl)) != 0;
        consume = win || (key.vkCode == VK_TAB && alt) ||
            (key.vkCode == VK_ESCAPE && (alt || ctrl));
    }
    if (escape) escape_();
    const bool accepted = translated_ && local_dispatch_
        ? local_dispatch_(key, down, event, consume && down)
        : event.kind != chast::InputKind::Key || send_(event);
    if (!accepted) {
        // Queue saturation cannot leave an earlier down without its up.
        release_();
    }
    // RustDesk lets releases reach Windows, including keys pressed before
    // capture started. Otherwise local modifiers can remain logically down.
    return consume && down;
}
}  // namespace chast_viewer
