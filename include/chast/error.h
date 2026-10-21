#pragma once

// HRESULT plumbing. Every DXGI/D3D call in chast goes through these two
// helpers so that failures carry the call site and a readable reason, and so
// the retry policy (ACCESS_LOST, device removal, ...) can be decided in one
// place instead of at each call.

#include <windows.h>

#include <string>
#include <system_error>
#include <utility>

namespace chast {

enum class Status {
    Ok = 0,
    AccessLost,        // duplication invalidated: desktop switch, mode change, secure desktop
    DeviceRemoved,     // adapter reset or removed
    DeviceReset,       // device needs recreation
    NotCurrentlyAvailable,
    WaitTimeout,       // no frame arrived within the requested timeout
    WouldBlock,        // frame acquired but no new desktop image
    Unsupported,       // API not available on this stack (e.g. no HDR pass)
    Unavailable,       // the target no longer exists (display unplugged, adapter gone)
    InvalidCall,
    PermissionDenied,  // the peer refused the handshake (token or approval)
    NoFrame,           // nothing captured yet
    CaptureFailed,     // generic failure with a message
};

std::string to_string(Status status);

class Error {
public:
    Error(Status status, std::string message)
        : status_(status), message_(std::move(message)) {}

    Status status() const noexcept { return status_; }
    const std::string& message() const noexcept { return message_; }
    bool ok() const noexcept { return status_ == Status::Ok; }

    std::string to_string() const;

    static Error success() { return Error(Status::Ok, {}); }

private:
    Status status_;
    std::string message_;
};

// True when a new duplication object has to be created before capturing can
// resume. WouldBlock and WaitTimeout are normal control flow, not failures.
bool needs_reinit(Status status);

// Maps an HRESULT to a Status. Unknown codes come back as CaptureFailed.
Status status_from_hresult(HRESULT hr);

// Returns Ok on S_OK/S_FALSE; otherwise an Error carrying the failing call.
Error check(HRESULT hr, const char* what);
inline Error check_win32(BOOL ok, const char* what) {
    if (ok) {
        return Error::success();
    }
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed, GetLastError=" +
                     std::to_string(GetLastError()));
}

std::string hresult_message(HRESULT hr);

}  // namespace chast
