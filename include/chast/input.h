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

// Modifier bit (see chast/protocol.h) for a HID usage, or 0 when the usage is
// not one of the eight modifier keys.
uint16_t modifier_bit_for_usage(uint16_t hid_usage);

struct InputStats {
    uint64_t applied = 0;
    uint64_t rejected = 0;  // unknown key or malformed event
    uint64_t failed = 0;    // SendInput reported zero inserted events
    uint64_t dropped = 0;   // queue was full
    uint64_t lock_toggles = 0;  // Caps/Num/Scroll taps that matched the client
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

    // Queues a release of everything held so far. It lands behind the events
    // already submitted and ahead of anything later, so a burst of pointer
    // moves cannot be applied after the viewer has given up control.
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
    // Brings held modifiers in line with the bitmap the peer reported.
    void sync_modifiers(uint16_t reported);
    // Taps Caps/Num/Scroll once when the peer's lock state differs from this
    // machine's. `reported` only carries kLockStateMask bits.
    void sync_locks(uint16_t reported);
    bool lock_state(uint16_t bit);
    // Folds Windows' synthetic Ctrl+Right Alt (AltGr) into Right Alt alone.
    uint16_t normalize_modifiers(uint16_t reported);
    void apply_mouse(const MouseEvent& event);
    void apply_key(const KeyEvent& event);
    void release_all();
    UINT send(const INPUT& input);

    bool dry_run_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    struct Pending {
        bool release = false;
        InputEvent event;
    };
    std::deque<Pending> queue_;
    bool stopping_ = false;
    std::thread thread_;

    // Owned by the injection thread.
    uint16_t held_buttons_ = 0;
    struct HeldKey {
        uint16_t scan = 0;
        bool extended = false;
        uint16_t usage = 0;
    };
    // Keys currently down, in press order, so release_all lifts them in
    // reverse. Scan code alone is ambiguous (0x1D is both Ctrl keys), so the
    // extended flag is kept with it.
    std::deque<HeldKey> held_keys_;
    // Set while a reported left Ctrl is the synthetic one Windows emits for
    // AltGr; kept until the peer stops reporting it.
    bool altgr_synthetic_ctrl_ = false;
    // This injector's view of the host lock keys. Seeded from the OS on first
    // use and toggled whenever a lock key is injected (including the taps
    // sync_locks() makes and a lock key the peer presses itself), so a tap
    // is never applied twice. A dry run seeds it at zero, since no tap ever
    // changes the real desktop.
    bool locks_seeded_ = false;
    uint16_t host_locks_ = 0;

    mutable std::mutex stats_mutex_;
    InputStats stats_;
};

}  // namespace chast
