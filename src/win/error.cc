// Status text and the retry policy, without the COM helpers. Kept separate
// from com.cc so a library that only formats an Error (the audio codec) does
// not have to link DXGI.

#include "chast/error.h"

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

}  // namespace chast
