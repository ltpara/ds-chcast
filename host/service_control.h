#pragma once
#include "connection_control.h"
#include <memory>

namespace chast::host {
struct ServiceApproval {
    uint64_t id = 0;
    std::string peer_address, device_name;
    SessionMode mode = SessionMode::Desktop;
    PermissionMask permissions = 0;
    bool device_proof = false;
};
struct ServiceNotice {
    uint64_t id = 0;
    std::string text;
    bool incoming = true;
};
struct ServiceRegistrationInfo {
    std::string device_id;
    bool enabled = false;
};
struct ServiceHostInfo {
    std::string device_id, fingerprint;
    HostAuthMode auth_mode = HostAuthMode::Password;
    bool password_configured = false, two_factor_enabled = false;
    bool registration_enabled = false;
};

// Owns a local-only, logon-SID-restricted named pipe in the worker's Windows
// session. start precedes HostServer::start; stop precedes HostServer::stop.
class ServiceControlServer {
public:
    ServiceControlServer();
    ~ServiceControlServer();
    bool start(HostServer& host, std::string* error,
               std::function<ServiceRegistrationInfo()> registration = {});
    void stop();
    ClientApproval approve(const ClientRequest& request);
    void receive_chat(uint64_t id, const std::string& text);
    // An isolated endpoint under the current user's token for existing test
    // harnesses. Production always uses the default constructor/start.
    bool start_for_test(HostServer& host, const std::wstring& pipe, std::string* error,
                        std::function<ServiceRegistrationInfo()> registration = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The UI never waits on the pipe. Polling and commands run on one bounded-I/O
// thread. UI IDs map to (worker instance, remote ID), including after restart.
class ServiceControlClient {
public:
    ServiceControlClient();
    ~ServiceControlClient();
    void start();
    void start_for_test(const std::wstring& pipe, uint32_t expected_pid);
    void stop();
    ConnectionActions actions();
    bool connected() const;
    std::string status() const;
    ServiceHostInfo information() const;
    std::vector<ServiceApproval> approvals() const;
    bool answer(uint64_t id, bool allow, PermissionMask permissions);
    std::vector<ServiceNotice> take_notices();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
