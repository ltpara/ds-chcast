#define NOMINMAX
#include "chast/power_request.h"
#include <windows.h>

namespace chast {
namespace {
class WindowsSessionPowerRequest final : public ISessionPowerRequest {
public:
    ~WindowsSessionPowerRequest() override {
        if (handle == INVALID_HANDLE_VALUE) return;
        if (display) PowerClearRequest(handle, PowerRequestDisplayRequired);
        if (system) PowerClearRequest(handle, PowerRequestSystemRequired);
        CloseHandle(handle);
    }
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool system = false;
    bool display = false;
};
}

std::unique_ptr<ISessionPowerRequest> create_session_power_request(
    bool keep_display_on, std::string* error) {
    if (error) error->clear();
    const auto report = [&](const char* operation) {
        if (error) *error = std::string(operation) + " failed (error " + std::to_string(GetLastError()) + ")";
    };
    auto request = std::make_unique<WindowsSessionPowerRequest>();
    wchar_t reason[] = L"chast: an authenticated remote session is active";
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = reason;
    request->handle = PowerCreateRequest(&context);
    if (request->handle == INVALID_HANDLE_VALUE) { report("PowerCreateRequest"); return nullptr; }
    if (!PowerSetRequest(request->handle, PowerRequestSystemRequired)) {
        report("PowerRequestSystemRequired");
        return nullptr;
    }
    request->system = true;
    if (keep_display_on) {
        if (PowerSetRequest(request->handle, PowerRequestDisplayRequired)) request->display = true;
        else report("PowerRequestDisplayRequired");
    }
    return request;
}
}
