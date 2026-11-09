#pragma once

// Server-side input injection.
//
// The wire format is platform neutral (chast/protocol.h): pointer coordinates
// in virtual-desktop pixels and keys as USB HID usages. This layer is the
// Windows end of it: it turns a usage id into a PS/2 set 1 scan code and
// drives SendInput, which is layout independent (KEYEVENTF_SCANCODE) and never
// needs to know what character the host layout would produce there.
//
// Input arrives on an MsQuic callback thread, so submit() only enqueues; one
// injection thread applies the events in order. That keeps SendInput on a
// single thread and lets the network callback return promptly.
//
// Injection is subject to UIPI: a lower-integrity process cannot inject into a
// higher one, and events sent while the thread is attached to a desktop that
// does not receive input (UAC, lock screen) are discarded by the OS. Both show
// up as `failed` in Stats, not as an error the session has to handle. The
// injection thread follows the input desktop where its process may open it.

#include <windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "chast/protocol.h"

namespace chast {

// Maps a usage id from HID usage page 0x07 to a PS/2 set 1 scan code. Returns
// false for usages the table does not carry (media keys live on another page,
// and Pause needs its E1 prefix, which SendInput cannot express as a scan
// code).
bool hid_to_scancode(uint16_t hid_usage, uint16_t& scan_code, bool& extended);

// Reverse mapping for the native viewer hook. Zero means unsupported. Pause
// is identified by virtual key because its E1 sequence is not a set-1 key.
uint16_t windows_key_to_hid(uint32_t virtual_key, uint32_t scan_code, bool extended);
bool make_keyboard_input(uint16_t usage, bool down, INPUT& input);
// Legacy keyboard mode injects a named virtual key instead of a scan code.
// Windows resolves it against the host layout; extended keys keep their
// navigation behavior. VK_PACKET and VK_PROCESSKEY are rejected because they
// only make sense with data this path does not carry.
bool make_legacy_keyboard_input(uint16_t virtual_key, bool down, INPUT& input);
inline constexpr ULONG_PTR kInputInjectionTag = 0x43484153u;

// Modifier bit (see chast/protocol.h) for a HID usage, or 0 when the usage is
// not one of the eight modifier keys.
uint16_t modifier_bit_for_usage(uint16_t hid_usage);
// The same bitmap for a Windows virtual key, including the side-less
// VK_SHIFT/VK_CONTROL/VK_MENU forms a hook can report.
uint16_t modifier_bit_for_vk(uint16_t virtual_key);

struct InputStats {
    uint64_t applied = 0;
    uint64_t rejected = 0;  // unknown key or malformed event
    uint64_t failed = 0;    // SendInput reported zero inserted events
    uint64_t dropped = 0;   // queue overflow, including work discarded for release
    uint64_t lock_toggles = 0;  // temporary lock adjustments and restoration
};

class InputInjector {
public:
    // Starts the injection thread. Fails only when the thread cannot start.
    // With `dry_run` the whole path runs (HID mapping, coordinate conversion,
    // held-key bookkeeping, counters) but SendInput is not called, so a
    // loopback test can exercise it without moving the real pointer or typing
    // into the focused window.
    // An optional dry-run observer receives each OS input in injection order
    // on the worker thread; it must outlive stop(). Ignored for live input.
    static std::unique_ptr<InputInjector> create(bool dry_run, std::string* error,
        std::function<void(const INPUT&)> dry_run_observer = {});

    ~InputInjector();
    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // Queues one event. Never blocks: when the queue is full the event is
    // dropped and counted, which is the right trade for a pointer stream that
    // is already stale at that point.
    void submit(const InputEvent& event);

    // Queues a block of composed text (protocol v36). The injection thread
    // types it as Unicode, so it lands as the viewer sent it regardless of the
    // host keyboard layout. Invalid text (see valid_text_input()) is rejected
    // and counted instead of queued. Same non-blocking queue and drop policy
    // as submit().
    void submit_text(const std::string& text);

    // Queues a release of everything held so far. It lands behind the events
    // already submitted and ahead of anything later, so a burst of pointer
    // moves cannot be applied after the viewer has given up control. A full
    // queue is discarded to reserve space for this release.
    void release_held();

    // Releases every key and button this injector is holding down, then stops
    // the thread. Called on teardown so a disconnect cannot leave a modifier
    // stuck on the host.
    void stop();

    InputStats stats() const;

private:
    InputInjector() = default;

    void run();
    void apply(const InputEvent& event);
    // Types composed text as Unicode. '\n' and '\t' become Return and Tab,
    // '\r' is skipped (viewers send CRLF pairs), and every other code point
    // is injected with KEYEVENTF_UNICODE, surrogates paired as one character.
    void apply_text(const std::string& text);
    // Brings held modifiers in line with the bitmap the peer reported.
    // `legacy` selects the injection mechanism for modifiers it has to press;
    // a held modifier is always lifted the way it was pressed.
    void sync_modifiers(uint16_t reported, bool legacy = false);
    // Temporarily reconciles the selected lock bits. Returns bits changed,
    // so apply() can restore the host's state after the key was injected.
    uint16_t sync_locks(uint16_t reported, uint16_t mask);
    bool lock_state(uint16_t bit);
    void apply_mouse(const MouseEvent& event);
    void apply_key(const KeyEvent& event);
    void apply_legacy_key(const LegacyKeyEvent& event);
    // Touch goes through Windows' touch injection, which takes a frame
    // containing every active contact; the injector keeps the active set and
    // rebuilds the frame on each event. When touch injection cannot start
    // (a session without the required desktop access), the first contact is
    // emulated as a left-button drag so the desktop stays usable.
    void apply_touch(const TouchEvent& event);
    // A pen is one synthetic pointer device (Windows 10 1809+), loaded
    // dynamically so the host still starts on older builds; without it the
    // pen falls back to a left-button drag as well.
    void apply_pen(const PenEvent& event);
    bool ensure_touch_available();
    bool ensure_pen_available();
    void apply_touch_fallback(const TouchEvent& event);
    void apply_pen_fallback(const PenEvent& event);
    void release_touch_contacts();
    void release_pen();
    // Emulated pointer used when the real injection API is unavailable.
    // Only one contact is tracked; additional contacts are ignored until it
    // lifts.
    enum class FallbackPointer : uint8_t { None, Touch, Pen };
    FallbackPointer fallback_pointer_ = FallbackPointer::None;
    uint8_t fallback_contact_ = 0;
    void release_fallback_pointer();
    void release_all();
    UINT send(const INPUT& input);
    // Moves the injection thread onto the desktop that currently receives
    // input, at most every 250 ms or immediately after SendInput rejected an
    // event. An actual switch forgets held keys and buttons: they do not
    // travel between desktops.
    void follow_input_desktop_if_due();

    bool dry_run_ = false;
    std::function<void(const INPUT&)> dry_run_observer_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    struct Pending {
        bool release = false;
        bool has_text = false;
        InputEvent event;
        std::string text;
    };
    std::deque<Pending> queue_;
    bool stopping_ = false;
    std::thread thread_;

    // Owned by the injection thread.
    uint16_t held_buttons_ = 0;
    struct HeldKey {
        uint16_t usage = 0;
        bool legacy = false;
        std::chrono::steady_clock::time_point last_down;
    };
    // Keys currently down, in press order, so release_all lifts them in
    // reverse. HID usage distinguishes left/right keys and Pause's fallback.
    std::deque<HeldKey> held_keys_;

    enum class DeviceState : uint8_t { Unknown, Ready, Unavailable };
    DeviceState touch_state_ = DeviceState::Unknown;
    DeviceState pen_state_ = DeviceState::Unknown;
    struct TouchSlot {
        bool used = false;
        int32_t x = 0;
        int32_t y = 0;
        uint16_t pressure = 0;
    };
    // Contacts the host currently holds down, addressed by protocol slot. A
    // frame sent to InjectTouchInput must repeat every one of them.
    std::array<TouchSlot, kMaxTouchContacts> touch_slots_{};
    bool pen_down_ = false;
    int32_t pen_x_ = 0;
    int32_t pen_y_ = 0;
    uint16_t pen_pressure_ = 0;
    uint8_t pen_flags_ = 0;
    int16_t pen_tilt_x_ = 0;
    int16_t pen_tilt_y_ = 0;

    // CreateSyntheticPointerDevice and friends appeared in Windows 10 1809,
    // so they are resolved at runtime instead of becoming a load-time
    // dependency of the whole host.
    using CreatePointerDeviceFn =
        void* (WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
    using InjectPointerInputFn = BOOL(WINAPI*)(void*, const POINTER_TYPE_INFO*, UINT32);
    using DestroyPointerDeviceFn = void(WINAPI*)(void*);
    CreatePointerDeviceFn create_pointer_device_ = nullptr;
    InjectPointerInputFn inject_pointer_input_ = nullptr;
    DestroyPointerDeviceFn destroy_pointer_device_ = nullptr;
    void* pen_device_ = nullptr;

    // Refreshed from the foreground input queue before affected keys, then
    // updated only on successful injection. Dry runs start at zero.
    bool locks_seeded_ = false;
    uint16_t host_locks_ = 0;
    // Started due so the first event checks the desktop before injecting.
    bool desktop_check_due_ = true;
    std::chrono::steady_clock::time_point last_desktop_follow_{};

    mutable std::mutex stats_mutex_;
    InputStats stats_;
};

}  // namespace chast
