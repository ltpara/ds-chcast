#pragma once

#define NOMINMAX
#include <windows.h>

#include "portable_helper_authorization.h"
#include "portable_helper_lifecycle.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace chast::host {

struct PortableHelperCapturedFrame;

// Owns the pipe and a process handle opened from the exact PID reported by
// SCM. The identity is captured while that handle is held, preventing PID
// reuse from turning a stale SCM status into a different pipe peer.
struct PortableHelperServiceConnection final {
  PortableHelperServiceConnection() = default;
  ~PortableHelperServiceConnection();
  PortableHelperServiceConnection(const PortableHelperServiceConnection&) = delete;
  PortableHelperServiceConnection& operator=(
      const PortableHelperServiceConnection&) = delete;
  PortableHelperServiceConnection(PortableHelperServiceConnection&& other) noexcept;
  PortableHelperServiceConnection& operator=(
      PortableHelperServiceConnection&& other) noexcept;

  void Reset() noexcept;

  std::unique_ptr<PortableHelperPipe> channel;
  HANDLE service_process = nullptr;
  PortableHelperProcessIdentity service_identity;
  PortableHelperInstanceId instance_id{};
  // Serializes request/response RPC pairs on this single duplex control pipe.
  mutable std::mutex rpc_mutex;
};

// Connects only to the currently running, instance-derived LocalSystem SCM
// service after checking its exact image command, service type/account, staged
// image trust, SCM PID and live process identity. The returned process handle
// is suitable for bounded termination/wait callbacks.
bool connect_portable_helper_service_channel(
    const PortableHelperServiceSpec& spec, uint32_t timeout_ms,
    PortableHelperServiceConnection* connection, std::string* error);

// Requests orderly service shutdown over the authenticated channel and waits
// for the exact SCM process handle to exit. Does not release desktop input;
// callers must revoke session authorization before invoking it.
bool request_portable_helper_service_stop(
    PortableHelperServiceConnection& connection, uint32_t timeout_ms,
    std::string* error);

// Parent-side session-worker RPCs. The caller must first obtain explicit local
// consent and create the grant with PortableHelperAuthorization::BeginGrant;
// a remote request alone is never sufficient. Calls on one connection must be
// serialized through this connection. Start opens the parent request gate only after the helper's
// correlated ACK and WorkerReady proof both validate. A failed start revokes
// the local forwarding gate and closes the control channel so the service
// enters its cleanup path.
bool start_portable_helper_session_worker(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint32_t timeout_ms,
    std::string* error);

// Stop is scoped to the exact connection/generation. The helper ACK is sent
// only after worker revocation and confirmed process exit; then the parent
// closes its own forwarding gate. On any failure the local gate is still
// revoked and the channel is closed to force service-side cleanup.
bool stop_portable_helper_session_worker(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint32_t timeout_ms,
    std::string* error);

// Connection-aware desktop RPC wrappers keep each request and its full
// response exchange atomic with respect to Start/Stop and other requests.
bool request_portable_helper_capture_frame(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    uint32_t timeout_ms, PortableHelperCapturedFrame* frame,
    std::string* error);
bool request_portable_helper_input_event(
    PortableHelperServiceConnection& connection,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    const chast::InputEvent& event, uint32_t timeout_ms,
    std::string* error);

using PortableHelperServiceControlRequestHandler = std::function<bool(
    PortableHelperPipe& control_channel, const PortableHelperMessage& request,
    bool* stop_service, std::string* error)>;

// Accepts one exact parent process on the already-created instance pipe, then
// serves only Status and Stop control messages. Stop is acknowledged before
// the callback is invoked. Parent death, SCM stop or Stop all close the pipe;
// malformed messages and unexpected disconnects fail closed.
bool run_portable_helper_service_control_channel(
    std::unique_ptr<PortableHelperPipe> server_channel,
    const PortableHelperProcessIdentity& expected_parent,
    HANDLE parent_process, HANDLE service_stop_event,
    const std::function<void()>& request_service_stop,
    std::string* error,
    PortableHelperServiceControlRequestHandler request_handler = {});

}  // namespace chast::host
