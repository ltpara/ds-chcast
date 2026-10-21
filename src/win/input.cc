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
#include <vector>

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
        // F13-F24 are not expressible with set 1 scan codes alone.

        case 0x46: scan = 0x37; extended = true; return true;  // Print Screen
        case 0x47: scan = 0x46; return true;                   // Scroll Lock
        // 0x48 Pause: needs the E1 prefix, which SendInput cannot emit as a
        // scan code. Rejected rather than sent as a wrong key.

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
        case 0x55: scan = 0x37; extended = true; return true;  // KP *
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
        // 0x68-0x73 F13-F24: no set 1 scan codes.

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

DWORD x_button(uint16_t buttons) {
    return (buttons & kMouseBack) ? XBUTTON1 : XBUTTON2;
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
    // GetAsyncKeyState, not GetKeyState: the injection thread has no message
    // loop, and the thread-local state GetKeyState reports only advances when
    // messages are dispatched. The low bit is the lock's toggle state here.
    switch (bit) {
        case kLockCaps: return (GetAsyncKeyState(VK_CAPITAL) & 0x0001) != 0;
        case kLockNum: return (GetAsyncKeyState(VK_NUMLOCK) & 0x0001) != 0;
        case kLockScroll: return (GetAsyncKeyState(VK_SCROLL) & 0x0001) != 0;
        default: return false;
    }
}

}  // namespace

bool hid_to_scancode(uint16_t hid_usage, uint16_t& scan_code, bool& extended) {
    return lookup(hid_usage, scan_code, extended);
}

uint16_t modifier_bit_for_usage(uint16_t hid_usage) {
    if (hid_usage >= 0xE0 && hid_usage <= 0xE7) {
        return static_cast<uint16_t>(1u << (hid_usage - 0xE0));
    }
    return 0;
}

std::unique_ptr<InputInjector> InputInjector::create(bool dry_run, std::string* error) {
    std::unique_ptr<InputInjector> injector(new InputInjector());
    injector->dry_run_ = dry_run;
    try {
        injector->thread_ = std::thread(&InputInjector::run, injector.get());
    } catch (const std::exception& e) {
        *error = std::string("input thread: ") + e.what();
        return nullptr;
    }
    return injector;
}

UINT InputInjector::send(const INPUT& input) {
    // A dry run stops at the OS boundary: everything above it (mapping,
    // coordinate conversion, held-key tracking) runs exactly as it does for a
    // real event, so a loopback check covers all of our code and none of the
    // desktop's.
    return dry_run_ ? 1u : SendInput(1, const_cast<INPUT*>(&input), sizeof(input));
}

InputInjector::~InputInjector() {
    stop();
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

void InputInjector::release_held() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || queue_.size() >= 1024) {
            return;
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
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        std::deque<Pending> batch;
        batch.swap(queue_);
        if (!batch.empty()) {
            // Apply outside the lock: SendInput can take milliseconds when
            // the desktop is busy, and submit() must stay non-blocking.
            lock.unlock();
            for (const Pending& pending : batch) {
                if (pending.release) {
                    release_all();
                } else {
                    apply(pending.event);
                }
            }
            lock.lock();
        }
        if (stopping_ && queue_.empty()) {
            break;
        }
    }
    lock.unlock();
    release_all();
}

void InputInjector::apply(const InputEvent& event) {
    uint16_t locks = static_cast<uint16_t>(event.modifiers & kLockStateMask);
    if (event.kind == InputKind::Key) {
        // A lock key toggles the host by itself; reconciling that same bit
        // first would tap it twice.
        locks = static_cast<uint16_t>(locks & ~lock_bit_for_usage(event.key.hid_usage));
    }
    sync_locks(locks);

    const uint16_t reported = normalize_modifiers(event.modifiers);
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
    if (event.kind == InputKind::Mouse) {
        apply_mouse(event.mouse);
    } else {
        apply_key(event.key);
    }
}

uint16_t InputInjector::normalize_modifiers(uint16_t reported) {
    reported &= kModifierMask;
    if ((reported & kModifierRightAlt) != 0) {
        if ((reported & kModifierLeftCtrl) != 0) {
            // Windows turns AltGr into a synthetic left Ctrl plus Right Alt.
            // Right Alt already means AltGr on the host, so fold the pair and
            // remember the Ctrl was synthetic until the peer stops reporting
            // it; otherwise releasing AltGr would press a stray Ctrl.
            altgr_synthetic_ctrl_ = true;
            reported &= static_cast<uint16_t>(~kModifierLeftCtrl);
        }
    } else if (altgr_synthetic_ctrl_) {
        if ((reported & kModifierLeftCtrl) != 0) {
            reported &= static_cast<uint16_t>(~kModifierLeftCtrl);
        } else {
            altgr_synthetic_ctrl_ = false;
        }
    }
    return reported;
}

bool InputInjector::lock_state(uint16_t bit) {
    if (!locks_seeded_) {
        locks_seeded_ = true;
        if (!dry_run_) {
            // A dry run must not read the real desktop either: SendInput never
            // flips it, so a model seeded at zero stays deterministic.
            host_locks_ = 0;
            if (system_lock_state(kLockCaps)) host_locks_ |= kLockCaps;
            if (system_lock_state(kLockNum)) host_locks_ |= kLockNum;
            if (system_lock_state(kLockScroll)) host_locks_ |= kLockScroll;
        }
    }
    return (host_locks_ & bit) != 0;
}

void InputInjector::sync_locks(uint16_t reported) {
    reported &= kLockStateMask;
    const uint16_t bits[] = {kLockCaps, kLockNum, kLockScroll};
    for (uint16_t bit : bits) {
        if (((reported & bit) != 0) == lock_state(bit)) {
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
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.lock_toggles;
    }
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
            if ((event.buttons & 0x1Fu) == 0) {
                break;
            }
            input.mi.dwFlags = mouse_flag(event.action, event.buttons);
            if (event.buttons & (kMouseBack | kMouseForward)) {
                input.mi.mouseData = x_button(event.buttons);
            }
            if (event.action == MouseAction::ButtonDown) {
                held_buttons_ |= static_cast<uint16_t>(event.buttons & 0x1Fu);
            } else {
                held_buttons_ &= static_cast<uint16_t>(~(event.buttons & 0x1Fu));
            }
            posted = true;
            break;
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

void InputInjector::apply_key(const KeyEvent& event) {
    uint16_t scan = 0;
    bool extended = false;
    if (!lookup(event.hid_usage, scan, extended)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.rejected;
        return;
    }

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (extended) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (event.action == KeyAction::Up) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    const bool pressed = event.action == KeyAction::Down;
    const auto matches = [scan, extended](const HeldKey& held) {
        return held.scan == scan && held.extended == extended;
    };
    const auto found = std::find_if(held_keys_.begin(), held_keys_.end(), matches);
    if (pressed) {
        // Auto-repeat arrives as repeated Downs. Only the first press belongs
        // in the held set; otherwise a single Up would leave the key stuck.
        if (found == held_keys_.end()) {
            held_keys_.push_back(HeldKey{scan, extended, event.hid_usage});
            // Whether the tap came from the peer or from sync_locks(), the
            // host's lock state just changed under us.
            if (const uint16_t lock = lock_bit_for_usage(event.hid_usage); lock != 0) {
                host_locks_ ^= lock;
            }
        }
    } else if (found != held_keys_.end()) {
        held_keys_.erase(found);
    }

    const UINT inserted = send(input);
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (inserted == 1) {
        ++stats_.applied;
    } else {
        ++stats_.failed;
    }
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
    while (!held_keys_.empty()) {
        const HeldKey held = held_keys_.back();
        held_keys_.pop_back();
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = held.scan;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if (held.extended) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
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
