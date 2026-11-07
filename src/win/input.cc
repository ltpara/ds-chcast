// Windows input injection: USB HID usage -> scan code -> SendInput.
//
// The table below is page 0x07 (keyboard/keypad) of the HID Usage Tables
// mapped to PS/2 set 1 scan codes. Scan codes are what a remote desktop
// should send: SendInput with KEYEVENTF_SCANCODE feeds the same code to the
// same keyboard driver a physical key would, so the host's layout, IME and
// dead keys behave exactly as if the key were pressed locally.

#include "chast/input.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "chast/desktop.h"

namespace chast {

namespace {

struct ScanMapping {
    uint16_t usage;
    uint16_t scan;
    bool extended;
};

// A switch keeps the table readable and lets the compiler see the common
// cases; it also avoids a binary search over an array whose order would then
// have to be maintained by hand.
bool lookup(uint16_t usage, uint16_t& scan, bool& extended) {
    extended = false;
    switch (usage) {
        // Letters.
        case 0x04: scan = 0x1E; return true;  // A
        case 0x05: scan = 0x30; return true;  // B
        case 0x06: scan = 0x2E; return true;  // C
        case 0x07: scan = 0x20; return true;  // D
        case 0x08: scan = 0x12; return true;  // E
        case 0x09: scan = 0x21; return true;  // F
        case 0x0A: scan = 0x22; return true;  // G
        case 0x0B: scan = 0x23; return true;  // H
        case 0x0C: scan = 0x17; return true;  // I
        case 0x0D: scan = 0x24; return true;  // J
        case 0x0E: scan = 0x25; return true;  // K
        case 0x0F: scan = 0x26; return true;  // L
        case 0x10: scan = 0x32; return true;  // M
        case 0x11: scan = 0x31; return true;  // N
        case 0x12: scan = 0x18; return true;  // O
        case 0x13: scan = 0x19; return true;  // P
        case 0x14: scan = 0x10; return true;  // Q
        case 0x15: scan = 0x13; return true;  // R
        case 0x16: scan = 0x1F; return true;  // S
        case 0x17: scan = 0x14; return true;  // T
        case 0x18: scan = 0x16; return true;  // U
        case 0x19: scan = 0x2F; return true;  // V
        case 0x1A: scan = 0x11; return true;  // W
        case 0x1B: scan = 0x2D; return true;  // X
        case 0x1C: scan = 0x15; return true;  // Y
        case 0x1D: scan = 0x2C; return true;  // Z

        // Digit row and the punctuation around it.
        case 0x1E: scan = 0x02; return true;  // 1
        case 0x1F: scan = 0x03; return true;  // 2
        case 0x20: scan = 0x04; return true;  // 3
        case 0x21: scan = 0x05; return true;  // 4
        case 0x22: scan = 0x06; return true;  // 5
        case 0x23: scan = 0x07; return true;  // 6
        case 0x24: scan = 0x08; return true;  // 7
        case 0x25: scan = 0x09; return true;  // 8
        case 0x26: scan = 0x0A; return true;  // 9
        case 0x27: scan = 0x0B; return true;  // 0
        case 0x28: scan = 0x1C; return true;  // Enter
        case 0x29: scan = 0x01; return true;  // Escape
        case 0x2A: scan = 0x0E; return true;  // Backspace
        case 0x2B: scan = 0x0F; return true;  // Tab
        case 0x2C: scan = 0x39; return true;  // Space
        case 0x2D: scan = 0x0C; return true;  // -
        case 0x2E: scan = 0x0D; return true;  // =
        case 0x2F: scan = 0x1A; return true;  // [
        case 0x30: scan = 0x1B; return true;  // ]
        case 0x31: scan = 0x2B; return true;  // backslash
        case 0x32: scan = 0x2B; return true;  // Non-US #
        case 0x33: scan = 0x27; return true;  // ;
        case 0x34: scan = 0x28; return true;  // '
        case 0x35: scan = 0x29; return true;  // grave
        case 0x36: scan = 0x33; return true;  // ,
        case 0x37: scan = 0x34; return true;  // .
        case 0x38: scan = 0x35; return true;  // /
        case 0x39: scan = 0x3A; return true;  // Caps Lock

        // Function keys.
        case 0x3A: scan = 0x3B; return true;  // F1
        case 0x3B: scan = 0x3C; return true;  // F2
        case 0x3C: scan = 0x3D; return true;  // F3
        case 0x3D: scan = 0x3E; return true;  // F4
        case 0x3E: scan = 0x3F; return true;  // F5
        case 0x3F: scan = 0x40; return true;  // F6
        case 0x40: scan = 0x41; return true;  // F7
        case 0x41: scan = 0x42; return true;  // F8
        case 0x42: scan = 0x43; return true;  // F9
        case 0x43: scan = 0x44; return true;  // F10
        case 0x44: scan = 0x57; return true;  // F11
        case 0x45: scan = 0x58; return true;  // F12

        case 0x46: scan = 0x37; extended = true; return true;  // Print Screen
        case 0x47: scan = 0x46; return true;                   // Scroll Lock
        // 0x48 Pause: needs the E1 prefix, which SendInput cannot emit as a
        // scan code. make_keyboard_input() uses VK_PAUSE instead.

        // Navigation block (all extended).
        case 0x49: scan = 0x52; extended = true; return true;  // Insert
        case 0x4A: scan = 0x47; extended = true; return true;  // Home
        case 0x4B: scan = 0x49; extended = true; return true;  // Page Up
        case 0x4C: scan = 0x53; extended = true; return true;  // Delete
        case 0x4D: scan = 0x4F; extended = true; return true;  // End
        case 0x4E: scan = 0x51; extended = true; return true;  // Page Down
        case 0x4F: scan = 0x4D; extended = true; return true;  // Right
        case 0x50: scan = 0x4B; extended = true; return true;  // Left
        case 0x51: scan = 0x50; extended = true; return true;  // Down
        case 0x52: scan = 0x48; extended = true; return true;  // Up

        // Keypad.
        case 0x53: scan = 0x45; extended = true; return true;  // Num Lock
        case 0x54: scan = 0x35; extended = true; return true;  // KP /
        case 0x55: scan = 0x37; return true;                   // KP *
        case 0x56: scan = 0x4A; return true;                   // KP -
        case 0x57: scan = 0x4E; return true;                   // KP +
        case 0x58: scan = 0x1C; extended = true; return true;  // KP Enter
        case 0x59: scan = 0x4F; return true;                   // KP 1
        case 0x5A: scan = 0x50; return true;                   // KP 2
        case 0x5B: scan = 0x51; return true;                   // KP 3
        case 0x5C: scan = 0x4B; return true;                   // KP 4
        case 0x5D: scan = 0x4C; return true;                   // KP 5
        case 0x5E: scan = 0x4D; return true;                   // KP 6
        case 0x5F: scan = 0x47; return true;                   // KP 7
        case 0x60: scan = 0x48; return true;                   // KP 8
        case 0x61: scan = 0x49; return true;                   // KP 9
        case 0x62: scan = 0x52; return true;                   // KP 0
        case 0x63: scan = 0x53; return true;                   // KP .
        case 0x64: scan = 0x56; return true;                   // Non-US backslash
        case 0x65: scan = 0x5D; extended = true; return true;  // Application
        case 0x67: scan = 0x59; return true;                   // KP =
        case 0x68: scan = 0x64; return true;  // F13
        case 0x69: scan = 0x65; return true;
        case 0x6A: scan = 0x66; return true;
        case 0x6B: scan = 0x67; return true;
        case 0x6C: scan = 0x68; return true;
        case 0x6D: scan = 0x69; return true;
        case 0x6E: scan = 0x6A; return true;
        case 0x6F: scan = 0x6B; return true;
        case 0x70: scan = 0x6C; return true;
        case 0x71: scan = 0x6D; return true;
        case 0x72: scan = 0x6E; return true;
        case 0x73: scan = 0x76; return true;  // F24
        case 0x85: scan = 0x7E; return true;  // Keypad comma
        case 0x87: scan = 0x73; return true;  // International 1 (Ro)
        case 0x89: scan = 0x7D; return true;  // International 3 (Yen)
        case 0x8A: scan = 0x79; return true;  // Convert
        case 0x8B: scan = 0x7B; return true;  // Non-convert

        // Modifiers.
        case 0xE0: scan = 0x1D; return true;                   // Left Ctrl
        case 0xE1: scan = 0x2A; return true;                   // Left Shift
        case 0xE2: scan = 0x38; return true;                   // Left Alt
        case 0xE3: scan = 0x5B; extended = true; return true;  // Left GUI
        case 0xE4: scan = 0x1D; extended = true; return true;  // Right Ctrl
        case 0xE5: scan = 0x36; return true;                   // Right Shift
        case 0xE6: scan = 0x38; extended = true; return true;  // Right Alt
        case 0xE7: scan = 0x5C; extended = true; return true;  // Right GUI

        default:
            return false;
    }
}

// `v` comes from the peer unchecked: scale in i64 so the multiplication cannot
// overflow, and clamp to the 0..=65535 range MOUSEEVENTF_ABSOLUTE takes.
// `extent` is 0 when the virtual screen metrics are unavailable.
bool to_absolute(int32_t v, int32_t origin, int32_t extent, int32_t& out) {
    if (extent <= 0) {
        return false;
    }
    const int64_t value =
        (static_cast<int64_t>(v) - origin) * 65535 / static_cast<int64_t>(extent);
    out = static_cast<int32_t>(value < 0 ? 0 : (value > 65535 ? 65535 : value));
    return true;
}

DWORD mouse_flag(MouseAction action, uint16_t buttons) {
    switch (action) {
        case MouseAction::ButtonDown:
            if (buttons & kMouseLeft) return MOUSEEVENTF_LEFTDOWN;
            if (buttons & kMouseRight) return MOUSEEVENTF_RIGHTDOWN;
            if (buttons & kMouseMiddle) return MOUSEEVENTF_MIDDLEDOWN;
            return MOUSEEVENTF_XDOWN;
        case MouseAction::ButtonUp:
            if (buttons & kMouseLeft) return MOUSEEVENTF_LEFTUP;
            if (buttons & kMouseRight) return MOUSEEVENTF_RIGHTUP;
            if (buttons & kMouseMiddle) return MOUSEEVENTF_MIDDLEUP;
            return MOUSEEVENTF_XUP;
        default:
            return 0;
    }
}

uint16_t modifier_usage(uint16_t bit) {
    switch (bit) {
        case kModifierLeftCtrl: return 0xE0;
        case kModifierLeftShift: return 0xE1;
        case kModifierLeftAlt: return 0xE2;
        case kModifierLeftGui: return 0xE3;
        case kModifierRightCtrl: return 0xE4;
        case kModifierRightShift: return 0xE5;
        case kModifierRightAlt: return 0xE6;
        case kModifierRightGui: return 0xE7;
        default: return 0;
    }
}

uint16_t lock_usage(uint16_t bit) {
    switch (bit) {
        case kLockCaps: return 0x39;
        case kLockNum: return 0x53;
        case kLockScroll: return 0x47;
        default: return 0;
    }
}

uint16_t lock_bit_for_usage(uint16_t usage) {
    switch (usage) {
        case 0x39: return kLockCaps;
        case 0x53: return kLockNum;
        case 0x47: return kLockScroll;
        default: return 0;
    }
}

bool system_lock_state(uint16_t bit) {
    // GetKeyState's low bit is the toggle state. GetAsyncKeyState's low bit
    // means "pressed since a previous query", which is not a lock state.
    switch (bit) {
        case kLockCaps: return (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        case kLockNum: return (GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
        case kLockScroll: return (GetKeyState(VK_SCROLL) & 0x0001) != 0;
        default: return false;
    }
}

bool read_system_locks(uint16_t& locks) {
    // This worker has no window receiving keyboard messages. Read the state
    // of the foreground input queue rather than its stale thread snapshot.
    const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD current = GetCurrentThreadId();
    if (foreground == 0) return false;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const bool attach = foreground != current;
    if (attach && !AttachThreadInput(current, foreground, TRUE)) return false;
    locks = (system_lock_state(kLockCaps) ? kLockCaps : 0) |
            (system_lock_state(kLockNum) ? kLockNum : 0) |
            (system_lock_state(kLockScroll) ? kLockScroll : 0);
    if (attach) AttachThreadInput(current, foreground, FALSE);
    return true;
}

// Touch contacts (and synthetic pointers for the pen) live in screen
// coordinates; the virtual screen is the union of the monitors and can have
// a negative origin when one sits left of or above the primary.
bool virtual_screen_rect(int32_t& left, int32_t& top, int32_t& width, int32_t& height) {
    left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return width > 0 && height > 0;
}

}  // namespace

bool hid_to_scancode(uint16_t hid_usage, uint16_t& scan_code, bool& extended) {
    return lookup(hid_usage, scan_code, extended);
}

uint16_t windows_key_to_hid(uint32_t vk, uint32_t scan, bool extended) {
    if (vk == VK_PAUSE) return 0x48;
    if (vk == VK_NUMLOCK) return 0x53;
    // Windows' AltGr companion is not a real left Ctrl.
    if (scan == 0x021D) return 0;
    for (uint16_t usage = 1; usage <= 0xE7; ++usage) {
        uint16_t candidate = 0;
        bool candidate_extended = false;
        if (lookup(usage, candidate, candidate_extended) && candidate == scan &&
            candidate_extended == extended) return usage;
    }
    return 0;
}

bool make_keyboard_input(uint16_t usage, bool down, INPUT& input) {
    input = INPUT{};
    input.type = INPUT_KEYBOARD;
    input.ki.dwExtraInfo = kInputInjectionTag;
    if (usage == 0x48) {
        input.ki.wVk = VK_PAUSE;
    } else {
        uint16_t scan = 0;
        bool extended = false;
        if (!lookup(usage, scan, extended)) return false;
        input.ki.wScan = scan;
        input.ki.dwFlags = KEYEVENTF_SCANCODE |
                              (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    return true;
}

uint16_t modifier_bit_for_usage(uint16_t hid_usage) {
    if (hid_usage >= 0xE0 && hid_usage <= 0xE7) {
        return static_cast<uint16_t>(1u << (hid_usage - 0xE0));
    }
    return 0;
}

std::unique_ptr<InputInjector> InputInjector::create(bool dry_run, std::string* error,
    std::function<void(const INPUT&)> dry_run_observer) {
    std::unique_ptr<InputInjector> injector(new InputInjector());
    injector->dry_run_ = dry_run;
    injector->dry_run_observer_ = std::move(dry_run_observer);
    try {
        injector->thread_ = std::thread(&InputInjector::run, injector.get());
    } catch (const std::exception& e) {
        *error = std::string("input thread: ") + e.what();
        return nullptr;
    }
    return injector;
}

UINT InputInjector::send(const INPUT& input) {
    INPUT tagged = input;
    if (tagged.type == INPUT_KEYBOARD) tagged.ki.dwExtraInfo = kInputInjectionTag;
    // A dry run stops at the OS boundary: everything above it (mapping,
    // coordinate conversion, held-key tracking) runs exactly as it does for a
    // real event, so a loopback check covers all of our code and none of the
    // desktop's.
    if (dry_run_) {
        if (dry_run_observer_) dry_run_observer_(tagged);
        return 1u;
    }
    const UINT inserted = SendInput(1, &tagged, sizeof(tagged));
    if (inserted == 0) {
        // The desktop can change between the check in run() and this call;
        // SendInput then drops the event. Look again before the next one.
        desktop_check_due_ = true;
    }
    return inserted;
}

void InputInjector::follow_input_desktop_if_due() {
    const auto now = std::chrono::steady_clock::now();
    if (!desktop_check_due_ &&
        now - last_desktop_follow_ < std::chrono::milliseconds(250)) {
        return;
    }
    desktop_check_due_ = false;
    last_desktop_follow_ = now;
    std::string error;
    bool switched = false;
    if (!follow_input_desktop(&error, &switched)) {
        // A tray host cannot open Winlogon; events keep going to the old
        // desktop (and are dropped there) until the user comes back.
        return;
    }
    if (switched) {
        // Keys and buttons belong to the desktop they were pressed on. The
        // new one starts with everything up, so forget the old state and let
        // the peer's next modifier bitmap press what is still held.
        held_keys_.clear();
        held_buttons_ = 0;
        locks_seeded_ = false;
        // The OS cancels injected contacts with the old desktop; attempting
        // to lift them on the new one would create phantom touches there.
        for (TouchSlot& slot : touch_slots_) {
            slot = TouchSlot{};
        }
        pen_down_ = false;
        fallback_pointer_ = FallbackPointer::None;
    }
}

InputInjector::~InputInjector() {
    stop();
    if (destroy_pointer_device_ != nullptr && pen_device_ != nullptr) {
        destroy_pointer_device_(pen_device_);
        pen_device_ = nullptr;
    }
}

void InputInjector::submit(const InputEvent& event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        if (queue_.size() >= 1024) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.dropped;
            return;
        }
        Pending pending;
        pending.event = event;
        queue_.push_back(pending);
    }
    cv_.notify_one();
}

void InputInjector::submit_text(const std::string& text) {
    if (!valid_text_input(text)) {
        std::lock_guard<std::mutex> stats_lock(stats_mutex_);
        ++stats_.rejected;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        if (queue_.size() >= 1024) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            ++stats_.dropped;
            return;
        }
        Pending pending;
        pending.has_text = true;
        pending.text = text;
        queue_.push_back(pending);
    }
    cv_.notify_one();
}

void InputInjector::release_held() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        // A release barrier must survive saturation; discard unsent work,
        // then release everything that actually reached the desktop.
        if (queue_.size() >= 1024) {
            std::lock_guard<std::mutex> stats_lock(stats_mutex_);
            stats_.dropped += queue_.size();
            queue_.clear();
        }
        Pending pending;
        pending.release = true;
        queue_.push_back(pending);
    }
    cv_.notify_one();
}

void InputInjector::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        // Events already queued still get applied: an event that arrived just
        // before the disconnect is not stale, and dropping a ButtonUp here
        // would leave the click half-done.
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

InputStats InputInjector::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void InputInjector::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait_for(lock, std::chrono::seconds(10),
                     [this] { return stopping_ || !queue_.empty(); });
        std::deque<Pending> batch;
        batch.swap(queue_);
        if (!batch.empty()) {
            // Apply outside the lock: SendInput can take milliseconds when
            // the desktop is busy, and submit() must stay non-blocking.
            lock.unlock();
            for (const Pending& pending : batch) {
                follow_input_desktop_if_due();
                if (pending.release) {
                    release_all();
                } else if (pending.has_text) {
                    apply_text(pending.text);
                } else {
                    apply(pending.event);
                }
            }
            lock.lock();
        }
        lock.unlock();
        const auto now = std::chrono::steady_clock::now();
        std::vector<uint16_t> expired;
        for (const HeldKey& held : held_keys_) {
            if (now - held.last_down >= std::chrono::minutes(6)) expired.push_back(held.usage);
        }
        for (const uint16_t usage : expired) apply_key({KeyAction::Up, usage});
        lock.lock();
        if (stopping_ && queue_.empty()) {
            break;
        }
    }
    lock.unlock();
    release_all();
}

void InputInjector::apply(const InputEvent& event) {
    // The native viewer filters AltGr's synthetic Ctrl by scan code. Folding
    // a Ctrl+RightAlt bitmap here would also destroy a real two-key chord.
    const uint16_t reported = event.modifiers & kModifierMask;
    if (event.kind == InputKind::Key) {
        const uint16_t own = modifier_bit_for_usage(event.key.hid_usage);
        if (own != 0) {
            // A modifier transition is applied by the reconciliation itself,
            // so it is injected once and cannot be pressed twice.
            const uint16_t target = event.key.action == KeyAction::Down
                                        ? static_cast<uint16_t>(reported | own)
                                        : static_cast<uint16_t>(reported & ~own);
            sync_modifiers(target);
            return;
        }
    }
    sync_modifiers(reported);
    switch (event.kind) {
        case InputKind::Mouse:
            apply_mouse(event.mouse);
            break;
        case InputKind::Key: {
            // Like RustDesk Map, reconcile LEDs only for affected keys, then
            // restore the host's state. Never toggle locks during a Win/Alt
            // chord or mouse drag, and never synchronize a lock with itself.
            const uint16_t usage = event.key.hid_usage;
            INPUT mapped{};
            uint16_t mask = 0;
            if (lock_bit_for_usage(usage) == 0 && usage != 0x28 && usage != 0x2B &&
                make_keyboard_input(usage, true, mapped)) {
                // Caps affects punctuation/digits on some layouts too.
                mask = kLockCaps;
                if ((usage >= 0x54 && usage <= 0x63) || usage == 0x67 || usage == 0x85) {
                    mask |= kLockNum;
                }
            }
            if (!dry_run_ && (mask != 0 || lock_bit_for_usage(usage) != 0)) {
                // Refresh for every affected key, including changes made by
                // someone at the host keyboard between remote events.
                locks_seeded_ = read_system_locks(host_locks_);
            }
            const uint16_t changed = sync_locks(event.modifiers, mask);
            apply_key(event.key);
            sync_locks(static_cast<uint16_t>(event.modifiers ^ changed), changed);
            break;
        }
        case InputKind::Touch:
            apply_touch(event.touch);
            break;
        case InputKind::Pen:
            apply_pen(event.pen);
            break;
        case InputKind::Gesture:
            if (event.gesture.action == GestureAction::Zoom) {
                const uint16_t control = (reported & (kModifierLeftCtrl | kModifierRightCtrl))
                                             ? 0
                                             : kModifierLeftCtrl;
                sync_modifiers(static_cast<uint16_t>(reported | control));
                apply_mouse({MouseAction::Wheel, 0, 0, event.gesture.y});
                sync_modifiers(reported);
            } else {
                if (event.gesture.x) {
                    apply_mouse({MouseAction::WheelHorizontal, 0, event.gesture.x, 0});
                }
                if (event.gesture.y) {
                    apply_mouse({MouseAction::Wheel, 0, 0, event.gesture.y});
                }
            }
            break;
    }
}

bool InputInjector::lock_state(uint16_t bit) {
    if (!locks_seeded_) {
        locks_seeded_ = true;
        if (!dry_run_) {
            // A dry run must not read the real desktop either: SendInput never
            // flips it, so a model seeded at zero stays deterministic.
            locks_seeded_ = read_system_locks(host_locks_);
        }
    }
    return (host_locks_ & bit) != 0;
}

uint16_t InputInjector::sync_locks(uint16_t reported, uint16_t mask) {
    reported &= kLockStateMask;
    uint16_t changed = 0;
    const uint16_t bits[] = {kLockCaps, kLockNum, kLockScroll};
    for (uint16_t bit : bits) {
        if ((mask & bit) == 0 || ((reported & bit) != 0) == lock_state(bit) ||
            (!dry_run_ && !locks_seeded_)) {
            continue;
        }
        // A lock key is a toggle: one tap, with the release immediately after.
        // apply_key() flips host_locks_ on the press, so the loop's next event
        // sees the new state and does not tap again.
        KeyEvent tap;
        tap.hid_usage = lock_usage(bit);
        tap.action = KeyAction::Down;
        apply_key(tap);
        tap.action = KeyAction::Up;
        apply_key(tap);
        if (((reported & bit) != 0) == lock_state(bit)) changed |= bit;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.lock_toggles;
    }
    return changed;
}

void InputInjector::sync_modifiers(uint16_t reported) {
    uint16_t held = 0;
    for (const HeldKey& key : held_keys_) {
        held |= modifier_bit_for_usage(key.usage);
    }

    const uint16_t lift = held & ~reported;
    if (lift != 0) {
        // Reverse press order, like release_all: lifting a chord in the order
        // it went down keeps intermediate states sensible.
        std::vector<uint16_t> usages;
        for (auto it = held_keys_.rbegin(); it != held_keys_.rend(); ++it) {
            if (lift & modifier_bit_for_usage(it->usage)) {
                usages.push_back(it->usage);
            }
        }
        for (uint16_t usage : usages) {
            KeyEvent up;
            up.action = KeyAction::Up;
            up.hid_usage = usage;
            apply_key(up);
        }
    }

    const uint16_t press = reported & ~held;
    for (uint16_t bit = kModifierLeftCtrl; bit <= kModifierRightGui;
         bit = static_cast<uint16_t>(bit << 1)) {
        if (press & bit) {
            KeyEvent down;
            down.action = KeyAction::Down;
            down.hid_usage = modifier_usage(bit);
            apply_key(down);
        }
    }
}

void InputInjector::apply_mouse(const MouseEvent& event) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    bool posted = false;

    switch (event.action) {
        case MouseAction::Move: {
            int32_t dx = 0;
            int32_t dy = 0;
            const int32_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int32_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            if (!to_absolute(event.x, left, GetSystemMetrics(SM_CXVIRTUALSCREEN), dx) ||
                !to_absolute(event.y, top, GetSystemMetrics(SM_CYVIRTUALSCREEN), dy)) {
                break;
            }
            input.mi.dx = dx;
            input.mi.dy = dy;
            input.mi.dwFlags =
                MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            posted = true;
            break;
        }
        case MouseAction::MoveRelative:
            input.mi.dx = event.x;
            input.mi.dy = event.y;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            posted = true;
            break;
        case MouseAction::ButtonDown:
        case MouseAction::ButtonUp: {
            const uint16_t pressed = static_cast<uint16_t>(event.buttons & 0x1Fu);
            if (pressed == 0) {
                break;
            }
            const bool down = event.action == MouseAction::ButtonDown;
            UINT applied = 0;
            UINT failed = 0;
            const auto send_button = [&](DWORD flags, DWORD data) {
                INPUT button{};
                button.type = INPUT_MOUSE;
                button.mi.dwFlags = flags;
                button.mi.mouseData = data;
                if (send(button) == 1) {
                    ++applied;
                } else {
                    ++failed;
                }
            };
            // One INPUT carries one flag, so a mask with several bits needs
            // one call per bit. This matters for Back+Forward: sending a
            // single XBUTTON left the other button down until release_all().
            const uint16_t primary =
                static_cast<uint16_t>(pressed & ~(kMouseBack | kMouseForward));
            if (primary != 0) {
                send_button(mouse_flag(event.action, primary), 0);
                if (down) {
                    held_buttons_ |= primary;
                } else {
                    held_buttons_ &= static_cast<uint16_t>(~primary);
                }
            }
            for (const uint16_t bit :
                 {static_cast<uint16_t>(kMouseBack), static_cast<uint16_t>(kMouseForward)}) {
                if ((pressed & bit) == 0) {
                    continue;
                }
                send_button(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP,
                            bit == kMouseBack ? XBUTTON1 : XBUTTON2);
                if (down) {
                    held_buttons_ |= bit;
                } else {
                    held_buttons_ &= static_cast<uint16_t>(~bit);
                }
            }
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.applied += applied;
            stats_.failed += failed;
            return;
        }
        case MouseAction::Wheel:
            input.mi.mouseData = static_cast<DWORD>(event.y);
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            posted = true;
            break;
        case MouseAction::WheelHorizontal:
            input.mi.mouseData = static_cast<DWORD>(event.x);
            input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            posted = true;
            break;
    }

    if (!posted) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.rejected;
        return;
    }
    const UINT inserted = send(input);
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (inserted == 1) {
        ++stats_.applied;
    } else {
        ++stats_.failed;
    }
}

bool InputInjector::ensure_touch_available() {
    if (touch_state_ == DeviceState::Unknown) {
        if (dry_run_) {
            // A dry run must not touch the OS; it still exercises the frame
            // builder and the contact bookkeeping below.
            touch_state_ = DeviceState::Ready;
        } else if (InitializeTouchInjection(kMaxTouchContacts, TOUCH_FEEDBACK_NONE)) {
            touch_state_ = DeviceState::Ready;
        } else {
            touch_state_ = DeviceState::Unavailable;
        }
    }
    return touch_state_ == DeviceState::Ready;
}

void InputInjector::apply_touch(const TouchEvent& event) {
    if (event.contact >= kMaxTouchContacts) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.rejected;
        return;
    }
    if (!ensure_touch_available()) {
        apply_touch_fallback(event);
        return;
    }

    TouchSlot& slot = touch_slots_[event.contact];
    TouchAction action = event.action;
    bool deactivate = false;
    int32_t left = 0;
    int32_t top = 0;
    int32_t width = 0;
    int32_t height = 0;
    switch (action) {
        case TouchAction::Down:
        case TouchAction::Move:
            if (!virtual_screen_rect(left, top, width, height)) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.rejected;
                return;
            }
            if (!slot.used) {
                // A Move without a Down (a lost frame, or the viewer joined
                // mid-gesture) opens the contact instead of being rejected:
                // the OS never sees an UPDATE for a pointer it does not know.
                action = TouchAction::Down;
                slot.used = true;
            }
            slot.x = std::clamp(event.x, left, left + width - 1);
            slot.y = std::clamp(event.y, top, top + height - 1);
            slot.pressure = std::min<uint16_t>(event.pressure, 1024);
            break;
        case TouchAction::Up:
        case TouchAction::Cancel:
            if (!slot.used) {
                return;
            }
            // The last frame carrying this contact must repeat the position
            // of its previous UPDATE, so the lift uses the stored point.
            deactivate = true;
            break;
    }

    std::vector<POINTER_TOUCH_INFO> contacts;
    contacts.reserve(kMaxTouchContacts);
    for (size_t i = 0; i < touch_slots_.size(); ++i) {
        const TouchSlot& current = touch_slots_[i];
        if (!current.used) {
            continue;
        }
        POINTER_TOUCH_INFO info{};
        info.pointerInfo.pointerType = PT_TOUCH;
        info.pointerInfo.pointerId = static_cast<UINT32>(i);
        info.pointerInfo.ptPixelLocation.x = current.x;
        info.pointerInfo.ptPixelLocation.y = current.y;
        if (i == event.contact) {
            switch (action) {
                case TouchAction::Down:
                    info.pointerInfo.pointerFlags =
                        POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
                    break;
                case TouchAction::Move:
                    info.pointerInfo.pointerFlags =
                        POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
                    break;
                case TouchAction::Up:
                    info.pointerInfo.pointerFlags = POINTER_FLAG_UP;
                    break;
                case TouchAction::Cancel:
                    info.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
                    break;
            }
        } else {
            // Every frame InjectTouchInput takes must describe all contacts
            // still on the desktop, not just the one that changed.
            info.pointerInfo.pointerFlags =
                POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        }
        if (current.pressure > 0) {
            info.touchMask = TOUCH_MASK_PRESSURE;
            info.pressure = current.pressure;
        }
        contacts.push_back(info);
    }

    bool ok = true;
    if (!contacts.empty() && !dry_run_) {
        ok = InjectTouchInput(static_cast<UINT32>(contacts.size()), contacts.data()) != 0;
    }
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (ok) {
            ++stats_.applied;
        } else {
            ++stats_.failed;
        }
    }
    if (!ok) {
        // Windows cancels every contact when a frame is invalid, so drop our
        // view of them; the next Down starts a fresh sequence.
        for (TouchSlot& current : touch_slots_) {
            current = TouchSlot{};
        }
        return;
    }
    if (deactivate) {
        slot.used = false;
    }
}

bool InputInjector::ensure_pen_available() {
    if (pen_state_ == DeviceState::Unknown) {
        if (dry_run_) {
            pen_state_ = DeviceState::Ready;
        } else {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            if (user32 != nullptr) {
                create_pointer_device_ = reinterpret_cast<CreatePointerDeviceFn>(
                    GetProcAddress(user32, "CreateSyntheticPointerDevice"));
                inject_pointer_input_ = reinterpret_cast<InjectPointerInputFn>(
                    GetProcAddress(user32, "InjectSyntheticPointerInput"));
                destroy_pointer_device_ = reinterpret_cast<DestroyPointerDeviceFn>(
                    GetProcAddress(user32, "DestroySyntheticPointerDevice"));
            }
            if (create_pointer_device_ != nullptr && inject_pointer_input_ != nullptr &&
                destroy_pointer_device_ != nullptr) {
                pen_device_ =
                    create_pointer_device_(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
            }
            pen_state_ = pen_device_ != nullptr ? DeviceState::Ready : DeviceState::Unavailable;
        }
    }
    return pen_state_ == DeviceState::Ready;
}

void InputInjector::apply_pen(const PenEvent& event) {
    if (!ensure_pen_available()) {
        apply_pen_fallback(event);
        return;
    }

    TouchAction action = event.action;
    bool deactivate = false;
    int32_t left = 0;
    int32_t top = 0;
    int32_t width = 0;
    int32_t height = 0;
    switch (action) {
        case TouchAction::Down:
        case TouchAction::Move:
            if (!virtual_screen_rect(left, top, width, height)) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.rejected;
                return;
            }
            if (!pen_down_) {
                action = TouchAction::Down;
                pen_down_ = true;
            }
            pen_x_ = std::clamp(event.x, left, left + width - 1);
            pen_y_ = std::clamp(event.y, top, top + height - 1);
            pen_pressure_ = std::min<uint16_t>(event.pressure, 1024);
            pen_flags_ = event.flags;
            pen_tilt_x_ = event.tilt_x;
            pen_tilt_y_ = event.tilt_y;
            break;
        case TouchAction::Up:
        case TouchAction::Cancel:
            if (!pen_down_) {
                return;
            }
            deactivate = true;
            break;
    }

    // Synthetic pointer coordinates are relative to the virtual-screen
    // origin, unlike touch injection, which takes desktop coordinates.
    if (!virtual_screen_rect(left, top, width, height)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.rejected;
        return;
    }
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    info.penInfo.pointerInfo.pointerType = PT_PEN;
    info.penInfo.pointerInfo.pointerId = 0;
    info.penInfo.pointerInfo.ptPixelLocation.x = pen_x_ - left;
    info.penInfo.pointerInfo.ptPixelLocation.y = pen_y_ - top;
    switch (action) {
        case TouchAction::Down:
            info.penInfo.pointerInfo.pointerFlags =
                POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
            break;
        case TouchAction::Move:
            info.penInfo.pointerInfo.pointerFlags =
                POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
            break;
        case TouchAction::Up:
            info.penInfo.pointerInfo.pointerFlags = POINTER_FLAG_UP;
            break;
        case TouchAction::Cancel:
            info.penInfo.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
            break;
    }
    info.penInfo.penFlags = pen_flags_ & (PEN_FLAG_BARREL | PEN_FLAG_INVERTED | PEN_FLAG_ERASER);
    if (pen_pressure_ > 0) {
        info.penInfo.penMask |= PEN_MASK_PRESSURE;
        info.penInfo.pressure = pen_pressure_;
    }
    if (pen_tilt_x_ != 0) {
        info.penInfo.penMask |= PEN_MASK_TILT_X;
        info.penInfo.tiltX = pen_tilt_x_;
    }
    if (pen_tilt_y_ != 0) {
        info.penInfo.penMask |= PEN_MASK_TILT_Y;
        info.penInfo.tiltY = pen_tilt_y_;
    }

    const bool ok = dry_run_ || inject_pointer_input_(pen_device_, &info, 1) != 0;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (ok) {
            ++stats_.applied;
        } else {
            ++stats_.failed;
        }
    }
    if (!ok || deactivate) {
        pen_down_ = false;
    }
}

void InputInjector::apply_touch_fallback(const TouchEvent& event) {
    switch (event.action) {
        case TouchAction::Down:
        case TouchAction::Move: {
            bool begin = false;
            if (fallback_pointer_ == FallbackPointer::None) {
                fallback_pointer_ = FallbackPointer::Touch;
                fallback_contact_ = event.contact;
                begin = true;
            } else if (fallback_pointer_ != FallbackPointer::Touch ||
                       fallback_contact_ != event.contact) {
                // One emulated pointer is all the mouse can express; extra
                // contacts are ignored until this one lifts.
                return;
            }
            MouseEvent move;
            move.action = MouseAction::Move;
            move.x = event.x;
            move.y = event.y;
            apply_mouse(move);
            if (begin) {
                MouseEvent down;
                down.action = MouseAction::ButtonDown;
                down.buttons = kMouseLeft;
                apply_mouse(down);
            }
            return;
        }
        case TouchAction::Up:
        case TouchAction::Cancel:
            if (fallback_pointer_ == FallbackPointer::Touch &&
                fallback_contact_ == event.contact) {
                release_fallback_pointer();
            }
            return;
    }
}

void InputInjector::apply_pen_fallback(const PenEvent& event) {
    switch (event.action) {
        case TouchAction::Down:
        case TouchAction::Move: {
            bool begin = false;
            if (fallback_pointer_ == FallbackPointer::None) {
                fallback_pointer_ = FallbackPointer::Pen;
                begin = true;
            } else if (fallback_pointer_ != FallbackPointer::Pen) {
                return;
            }
            MouseEvent move;
            move.action = MouseAction::Move;
            move.x = event.x;
            move.y = event.y;
            apply_mouse(move);
            if (begin) {
                MouseEvent down;
                down.action = MouseAction::ButtonDown;
                down.buttons = kMouseLeft;
                apply_mouse(down);
            }
            return;
        }
        case TouchAction::Up:
        case TouchAction::Cancel:
            if (fallback_pointer_ == FallbackPointer::Pen) {
                release_fallback_pointer();
            }
            return;
    }
}

void InputInjector::release_fallback_pointer() {
    if (fallback_pointer_ == FallbackPointer::None) {
        return;
    }
    fallback_pointer_ = FallbackPointer::None;
    MouseEvent up;
    up.action = MouseAction::ButtonUp;
    up.buttons = kMouseLeft;
    apply_mouse(up);
}

void InputInjector::release_touch_contacts() {
    for (size_t i = 0; i < touch_slots_.size(); ++i) {
        if (!touch_slots_[i].used) {
            continue;
        }
        TouchEvent cancel;
        cancel.action = TouchAction::Cancel;
        cancel.contact = static_cast<uint8_t>(i);
        apply_touch(cancel);
    }
}

void InputInjector::release_pen() {
    if (pen_down_) {
        PenEvent cancel;
        cancel.action = TouchAction::Cancel;
        apply_pen(cancel);
    } else if (fallback_pointer_ == FallbackPointer::Pen) {
        release_fallback_pointer();
    }
}

void InputInjector::apply_key(const KeyEvent& event) {
    INPUT input{};
    const bool pressed = event.action == KeyAction::Down;
    if (!make_keyboard_input(event.hid_usage, pressed, input)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.rejected;
        return;
    }

    const auto matches = [&event](const HeldKey& held) {
        return held.usage == event.hid_usage;
    };
    const auto found = std::find_if(held_keys_.begin(), held_keys_.end(), matches);
    const UINT inserted = send(input);
    if (inserted != 1) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.failed;
        return;
    }
    if (pressed) {
        // Auto-repeat arrives as repeated Downs. Only the first press belongs
        // in the held set; otherwise a single Up would leave the key stuck.
        if (found == held_keys_.end()) {
            held_keys_.push_back(HeldKey{event.hid_usage, std::chrono::steady_clock::now()});
            // Whether the tap came from the peer or from sync_locks(), the
            // host's lock state just changed under us.
            if (const uint16_t lock = lock_bit_for_usage(event.hid_usage); lock != 0) {
                host_locks_ ^= lock;
            }
        } else {
            found->last_down = std::chrono::steady_clock::now();
        }
    } else if (found != held_keys_.end()) {
        held_keys_.erase(found);
    }

    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.applied;
}

void InputInjector::apply_text(const std::string& text) {
    // Text arrived through valid_text_input(), so the UTF-8 needs no repair
    // here. Counting is per code point, not per SendInput call: a surrogate
    // pair is one character to the user even though it takes four inputs.
    const auto tap_virtual_key = [this](WORD vk) {
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = vk;
        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        return send(down) == 1 && send(up) == 1;
    };
    const auto type_code_point = [this](uint32_t code_point) {
        uint16_t units[2];
        int count = 1;
        if (code_point <= 0xFFFF) {
            units[0] = static_cast<uint16_t>(code_point);
        } else {
            const uint32_t rest = code_point - 0x10000;
            units[0] = static_cast<uint16_t>(0xD800 + (rest >> 10));
            units[1] = static_cast<uint16_t>(0xDC00 + (rest & 0x3FF));
            count = 2;
        }
        bool ok = true;
        for (int i = 0; i < count; ++i) {
            INPUT down{};
            down.type = INPUT_KEYBOARD;
            down.ki.wScan = units[i];
            down.ki.dwFlags = KEYEVENTF_UNICODE;
            INPUT up = down;
            up.ki.dwFlags |= KEYEVENTF_KEYUP;
            if (send(down) != 1 || send(up) != 1) {
                ok = false;
            }
        }
        return ok;
    };

    size_t applied = 0;
    size_t failed = 0;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        uint32_t code_point = 0;
        size_t length = 1;
        if (lead < 0x80) {
            code_point = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1Fu;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0Fu;
            length = 3;
        } else {
            code_point = lead & 0x07u;
            length = 4;
        }
        for (size_t j = 1; j < length; ++j) {
            code_point = (code_point << 6) |
                         (static_cast<unsigned char>(text[i + j]) & 0x3Fu);
        }
        i += length;

        bool ok = true;
        if (code_point == '\r') {
            // Viewers send CRLF pairs; Return goes out once, on the '\n'.
            continue;
        } else if (code_point == '\n') {
            ok = tap_virtual_key(VK_RETURN);
        } else if (code_point == '\t') {
            ok = tap_virtual_key(VK_TAB);
        } else {
            ok = type_code_point(code_point);
        }
        if (ok) {
            ++applied;
        } else {
            ++failed;
        }
    }

    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.applied += applied;
    stats_.failed += failed;
}

void InputInjector::release_all() {
    // Buttons first: releasing a key mid-drag would still leave the drag on.
    uint16_t buttons = held_buttons_;
    held_buttons_ = 0;
    const MouseButtonBit order[] = {kMouseLeft, kMouseRight, kMouseMiddle, kMouseBack,
                                    kMouseForward};
    for (MouseButtonBit bit : order) {
        if (buttons & bit) {
            MouseEvent release;
            release.action = MouseAction::ButtonUp;
            release.buttons = static_cast<uint16_t>(bit);
            apply_mouse(release);
        }
    }
    // Contacts and the emulated pointer are drags too: lift them before the
    // keys so a gesture cannot stay active behind a released modifier.
    release_touch_contacts();
    release_pen();
    // Catches a mouse-emulated contact when injection never started.
    release_fallback_pointer();
    while (!held_keys_.empty()) {
        const HeldKey held = held_keys_.back();
        held_keys_.pop_back();
        INPUT input{};
        make_keyboard_input(held.usage, false, input);
        const UINT inserted = send(input);
        std::lock_guard<std::mutex> lock(stats_mutex_);
        if (inserted == 1) {
            ++stats_.applied;
        } else {
            ++stats_.failed;
        }
    }
}

}  // namespace chast
