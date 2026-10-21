#include "chast/error.h"

#include <comdef.h>

#include <cstdio>

namespace chast {

std::string to_string(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::AccessLost: return "access lost";
        case Status::DeviceRemoved: return "device removed";
        case Status::DeviceReset: return "device reset";
        case Status::NotCurrentlyAvailable: return "not currently available";
        case Status::WaitTimeout: return "wait timeout";
        case Status::WouldBlock: return "no new frame";
        case Status::Unsupported: return "unsupported";
        case Status::Unavailable: return "unavailable";
        case Status::InvalidCall: return "invalid call";
        case Status::PermissionDenied: return "permission denied";
        case Status::NoFrame: return "no frame captured yet";
        case Status::CaptureFailed: return "capture failed";
    }
    return "unknown";
}

std::string Error::to_string() const {
    if (ok()) {
        return "ok";
    }
    return std::string(chast::to_string(status_)) + ": " + message_;
}

bool needs_reinit(Status status) {
    switch (status) {
        case Status::AccessLost:
        case Status::DeviceRemoved:
        case Status::DeviceReset:
        case Status::Unavailable:
            return true;
        default:
            return false;
    }
}

std::string hresult_message(HRESULT hr) {
    // _com_error walks FormatMessage for the system message; fall back to the
    // raw code when no text is registered (common for DXGI-specific codes).
    _com_error err(hr);
    const wchar_t* text = err.Description();
    std::string out;
    if (text && *text) {
        // -1 includes the terminator in the byte count, so the buffer has to
        // hold `len` bytes even though the result excludes the NUL.
        const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            std::string converted(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, converted.data(), len, nullptr, nullptr);
            converted.resize(static_cast<size_t>(len - 1));
            out = std::move(converted);
        }
    }
    char code[16];
    sprintf_s(code, "0x%08X", static_cast<unsigned>(hr));
    return out.empty() ? code : out + " (" + code + ")";
}

Status status_from_hresult(HRESULT hr) {
    switch (hr) {
        case S_OK:
        case S_FALSE:
            return Status::Ok;
        case DXGI_ERROR_ACCESS_LOST:
        case DXGI_ERROR_SESSION_DISCONNECTED:
            return Status::AccessLost;
        case DXGI_ERROR_DEVICE_REMOVED:
            return Status::DeviceRemoved;
        case DXGI_ERROR_DEVICE_RESET:
            return Status::DeviceReset;
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
            return Status::NotCurrentlyAvailable;
        case DXGI_ERROR_WAIT_TIMEOUT:
            return Status::WaitTimeout;
        case DXGI_ERROR_UNSUPPORTED:
            return Status::Unsupported;
        case DXGI_ERROR_INVALID_CALL:
        case E_INVALIDARG:
            return Status::InvalidCall;
        case E_ACCESSDENIED:
            return Status::CaptureFailed;
        default:
            return Status::CaptureFailed;
    }
}

Error check(HRESULT hr, const char* what) {
    Status status = status_from_hresult(hr);
    if (status == Status::Ok) {
        return Error::success();
    }
    return Error(status, std::string(what) + " failed: " + hresult_message(hr));
}

}  // namespace chast
