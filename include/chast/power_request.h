#pragma once
#include <memory>
#include <string>

namespace chast {
// One authenticated connection owns one request. Destruction releases only
// this connection's requirements; other connections keep their own requests.
class ISessionPowerRequest {
public:
    virtual ~ISessionPowerRequest() = default;
};

// Prevent automatic system sleep, and optionally display idle power-off.
// A display request failure retains system protection and reports the error.
std::unique_ptr<ISessionPowerRequest> create_session_power_request(
    bool keep_display_on, std::string* error);
}
