#pragma once

// The WinSta0 desktop that currently receives input, and moving a thread onto
// it.
//
// A remote session has to follow the input desktop: when the workstation locks
// or a UAC prompt appears, input moves to a secure desktop (Winlogon) and both
// capture and SendInput only reach the desktop their thread is attached to.
// Only a process running as SYSTEM inside the user's session can open a secure
// desktop; the tray application cannot and reports Unavailable instead.

#include <string>

namespace chast {

enum class InputDesktopKind {
    // winsta0\default, the ordinary desktop.
    Default,
    // Another input desktop: the lock screen, a UAC prompt or the disconnect
    // screen. Capturable by a service, not by the tray.
    Secure,
    // The input desktop could not be queried at all.
    Unavailable,
};

struct InputDesktopStatus {
    InputDesktopKind kind = InputDesktopKind::Unavailable;
    // The WTS lock state, independent of the desktop name: a UAC prompt
    // switches desktops without locking the session.
    bool locked = false;
    // Desktop name as Win32 reports it ("Default", "Winlogon", "Disconnect");
    // empty when it could not be read.
    std::string name;
    // Why the status is Unavailable, or empty.
    std::string error;
};

// Switches the process to per-monitor DPI awareness so that every Win32
// coordinate API (GetSystemMetrics, GetCursorPos, GetMonitorInfo, SendInput's
// virtual-desktop normalization) reports physical pixels, the same space as
// the DXGI output rects and the coordinates viewers send. Without it a process
// on a scaled display sees logical pixels and remote mouse, touch and pen
// input lands off target. Must be called before any window or capture object
// exists; callers that already have a DPI-aware manifest get the manifest's
// awareness instead. Returns true when the process ended up per-monitor aware.
bool enable_physical_desktop_coordinates();

// Opens the input desktop, reads the lock state and closes it again. Does not
// change the calling thread's desktop.
InputDesktopStatus input_desktop_status();

// What the host can type an OS password into. Only Logon and Locked are
// prompts; a UAC prompt while the session is unlocked must never receive it.
enum class OsLoginState {
    Unlocked,
    // No user signed in to this session yet: the sign-in screen.
    Logon,
    // A signed-in session showing its lock screen.
    Locked,
    // The state or the secure desktop could not be read; fail closed.
    Unavailable,
};

struct OsLoginStatus {
    OsLoginState state = OsLoginState::Unavailable;
    // Why the state is Unavailable, or empty.
    std::string error;
};

// Reads the session user and lock state, then for a logon-like state confirms
// that the input desktop is a secure one this process may open and type on.
// A process without secure-desktop access (the tray application) reports
// Unavailable even while the workstation is locked.
OsLoginStatus os_login_status();

// True when the calling thread is attached to the desktop that receives
// input. False when the desktop cannot be queried.
bool on_input_desktop();

// Attaches the calling thread to the input desktop. Returns true when the
// thread already was there or the switch succeeded. `switched` is set only for
// an actual move, so callers know whether their capture objects are now stale.
// The thread must not own windows or hooks; on failure *error explains why and
// the thread stays where it was.
bool follow_input_desktop(std::string* error, bool* switched = nullptr);

}  // namespace chast
