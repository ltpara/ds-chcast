#pragma once
#include <windows.h>
#include <memory>
#include <string>

namespace chast::host {
struct ServiceManagerStartResult {
    DWORD process_id = 0;
    std::string error;
};

// Called by the SYSTEM service for its interactive worker's Windows session.
// Keeps normal exits quiet until the next logon or service restart; failures retry
// with a delay. Closing this supervisor never kills the user's manager.
class ServiceManagerStartup {
public:
    explicit ServiceManagerStartup(std::wstring executable);
    ~ServiceManagerStartup();
    ServiceManagerStartResult poll(DWORD session);
    void logged_off(DWORD session);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The manager is shipped beside chast-service.exe, never resolved via PATH.
std::wstring service_manager_executable();
}
