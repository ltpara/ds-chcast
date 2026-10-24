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

// Opens the input desktop, reads the lock state and closes it again. Does not
// change the calling thread's desktop.
InputDesktopStatus input_desktop_status();

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
