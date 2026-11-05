#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace chast {

// The backend must arm independent recovery BEFORE prepare() can change the
// topology. A failed restore retains the recovery process and its snapshot.
class DisplayPrivacyBackend {
public:
    virtual ~DisplayPrivacyBackend() = default;
    virtual bool arm(std::string* error) = 0;
    virtual bool prepare(std::wstring* device, std::string* error) = 0;
    virtual bool isolate(std::string* error) = 0;
    virtual bool restore(std::string* error) = 0;
    virtual bool intact(std::string* error) = 0;
    virtual bool toggle(const std::wstring&, bool, std::vector<std::wstring>*, std::string* error) {
        if (error) *error = "targeted display management is unsupported";
        return false;
    }
    virtual bool configure(uint32_t, std::vector<std::wstring>*, std::string* error) {
        if (error) *error = "display management is unsupported";
        return false;
    }
};

class VirtualDisplayPrivacy {
public:
    // commit=false proves GPU capture AND encoding before physical outputs
    // are disabled. commit=true rebuilds and publishes the actual stream.
    using Capture = std::function<bool(const std::wstring&, bool commit, std::string*)>;
    explicit VirtualDisplayPrivacy(std::unique_ptr<DisplayPrivacyBackend> backend);
    ~VirtualDisplayPrivacy();
    VirtualDisplayPrivacy(const VirtualDisplayPrivacy&) = delete;
    VirtualDisplayPrivacy& operator=(const VirtualDisplayPrivacy&) = delete;
    bool enable(const Capture& capture, std::string* error);
    bool disable(std::string* error);
    bool intact(std::string* error);
    bool active() const { return active_; }
    bool recovery_pending() const { return armed_; }
    bool topology_may_have_changed() const { return topology_touched_; }
private:
    std::unique_ptr<DisplayPrivacyBackend> backend_;
    bool armed_ = false;
    bool active_ = false;
    bool topology_touched_ = false;
};

std::unique_ptr<DisplayPrivacyBackend> make_display_privacy_backend(
    const std::string& resolution, const std::wstring& package_dir);

// Temporary extended desktops preserve the original layout and never delete
// PnP monitors belonging to the operator or another application.
std::unique_ptr<DisplayPrivacyBackend> make_display_management_backend(const std::wstring& package_dir);
uint32_t attached_virtual_display_count();
struct VirtualDisplayOutput {
    std::wstring identity;
    bool active = false;
};
bool enumerate_virtual_display_outputs(std::vector<VirtualDisplayOutput>& outputs, std::string* error);

// Internal helper executable entry point. Its only inputs are inherited
// kernel handles, never a user-supplied recovery file or process id.
int run_display_recovery_guard(int argc, wchar_t** argv);

}  // namespace chast
