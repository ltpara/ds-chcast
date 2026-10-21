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
// higher one, and events sent while the secure desktop (UAC, lock screen) is up
// are discarded by the OS. Both show up as `failed` in Stats, not as an error
// the session has to handle.

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
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

struct InputStats {
    uint64_t applied = 0;
    uint64_t rejected = 0;  // unknown key or malformed event
    uint64_t failed = 0;    // SendInput reported zero inserted events
    uint64_t dropped = 0;   // queue was full
};

class InputInjector {
public:
    // Starts the injection thread. Fails only when the thread cannot start.
    // With `dry_run` the whole path runs (HID mapping, coordinate conversion,
    // held-key bookkeeping, counters) but SendInput is not called, so a
    // loopback test can exercise it without moving the real pointer or typing
    // into the focused window.
    static std::unique_ptr<InputInjector> create(bool dry_run, std::string* error);

    ~InputInjector();
    InputInjector(const InputInjector&) = delete;
    InputInjector& operator=(const InputInjector&) = delete;

    // Queues one event. Never blocks: when the queue is full the event is
    // dropped and counted, which is the right trade for a pointer stream that
    // is already stale at that point.
    void submit(const InputEvent& event);

    // Releases every key and button this injector is holding down, then stops
    // the thread. Called on teardown so a disconnect cannot leave a modifier
    // stuck on the host.
    void stop();

    InputStats stats() const;

private:
    InputInjector() = default;

    void run();
    void apply(const InputEvent& event);
    void apply_mouse(const MouseEvent& event);
    void apply_key(const KeyEvent& event);
    void release_all();
    UINT send(const INPUT& input);

    bool dry_run_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<InputEvent> queue_;
    bool stopping_ = false;
    std::thread thread_;

    // Owned by the injection thread.
    uint16_t held_buttons_ = 0;
    struct HeldKey {
        uint16_t scan = 0;
        bool extended = false;
    };
    // Keys currently down, in press order, so release_all lifts them in
    // reverse. Scan code alone is ambiguous (0x1D is both Ctrl keys), so the
    // extended flag is kept with it.
    std::deque<HeldKey> held_keys_;

    mutable std::mutex stats_mutex_;
    InputStats stats_;
};

}  // namespace chast
