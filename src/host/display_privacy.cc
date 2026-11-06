#include "chast/display_privacy.h"

#include <utility>

namespace chast {

VirtualDisplayPrivacy::VirtualDisplayPrivacy(std::unique_ptr<DisplayPrivacyBackend> backend)
    : backend_(std::move(backend)) {}

VirtualDisplayPrivacy::~VirtualDisplayPrivacy() {
    std::string ignored;
    disable(&ignored);
}

bool VirtualDisplayPrivacy::enable(const Capture& capture, std::string* error) {
    if (active_) return intact(error);
    if (armed_) {
        if (error) *error = "display recovery is still pending";
        return false;
    }
    if (!backend_ || !capture) {
        if (error) *error = "virtual display privacy is unavailable";
        return false;
    }
    topology_touched_ = false;
    if (!backend_->arm(error)) return false;
    armed_ = true;
    topology_touched_ = true;
    std::wstring device;
    try {
        if (backend_->prepare(&device, error) && !device.empty() &&
            capture(device, false, error) && backend_->isolate(error) &&
            capture(device, true, error) && backend_->intact(error)) {
            active_ = true;
            return true;
        }
        if (error && error->empty()) *error = "virtual display activation did not produce a usable output";
    } catch (...) {
        if (error) *error = "virtual display activation threw an exception";
    }
    std::string recovery_error;
    if (!disable(&recovery_error) && error) {
        *error += "; recovery pending: " + recovery_error;
    }
    return false;
}

bool VirtualDisplayPrivacy::disable(std::string* error) {
    if (!armed_) return true;
    if (!backend_->restore(error)) return false;
    armed_ = false;
    active_ = false;
    return true;
}

bool VirtualDisplayPrivacy::intact(std::string* error) {
    return active_ && backend_->intact(error);
}

}  // namespace chast
