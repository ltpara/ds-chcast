#pragma once

#define NOMINMAX
#include <windows.h>

#include "portable_helper_authorization.h"
#include "portable_helper_protocol.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace chast::host {

// Typed messages for the dedicated parent/elevated-manager pipe. The pipe
// must already have authenticated its exact peer process identity. Grant
// payloads contain local capability secrets and must never cross the network.
bool send_portable_helper_manager_start(
    PortableHelperPipe& pipe, const PortableHelperGrant& grant,
    uint64_t request_id, uint32_t timeout_ms, std::string* error);
bool receive_portable_helper_manager_start(
    PortableHelperPipe& pipe, uint32_t expected_windows_session_id,
    uint32_t timeout_ms, uint64_t* request_id, PortableHelperGrant* grant,
    std::string* error);

// A ready response carries the proof returned by the manager's authenticated
// helper session coordinator. The parent validates it against its original
// pending grant before opening its forwarding gate.
bool send_portable_helper_manager_ready(
    PortableHelperPipe& pipe, uint64_t request_id, bool ready,
    const PortableHelperSecret* worker_ready_proof, uint32_t timeout_ms,
    std::string* error);
bool receive_portable_helper_manager_ready(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t expected_request_id,
    uint32_t timeout_ms, PortableHelperSecret* worker_ready_proof,
    std::string* error);

bool send_portable_helper_manager_stop(PortableHelperPipe& pipe,
                                       uint64_t request_id,
                                       uint32_t timeout_ms,
                                       std::string* error);
bool receive_portable_helper_manager_stop(PortableHelperPipe& pipe,
                                          uint64_t expected_request_id,
                                          uint32_t timeout_ms,
                                          std::string* error);
bool send_portable_helper_manager_stopped(PortableHelperPipe& pipe,
                                           uint64_t request_id, bool stopped,
                                           uint32_t timeout_ms,
                                           std::string* error);
bool receive_portable_helper_manager_stopped(PortableHelperPipe& pipe,
                                              uint64_t expected_request_id,
                                              uint32_t timeout_ms,
                                              std::string* error);

// Manager-side session loop. The caller must authenticate the host peer
// before entering this function. A successful Start owns the elevated helper
// session until a correlated Stop arrives or the host pipe disconnects; both
// paths invoke stop_session before returning.
using PortableHelperManagerStartSession = std::function<bool(
    const PortableHelperGrant&, PortableHelperSecret*, std::string*)>;
using PortableHelperManagerStopSession =
    std::function<bool(std::string*)>;
struct PortableHelperCapturedFrame;
using PortableHelperManagerCaptureFrame = std::function<bool(
    uint64_t, uint32_t, PortableHelperCapturedFrame*, std::string*)>;
using PortableHelperManagerInputEvent = std::function<bool(
    uint64_t, const chast::InputEvent&, uint32_t, std::string*)>;
bool run_portable_helper_manager_control_channel(
    PortableHelperPipe& pipe, uint32_t expected_windows_session_id,
    uint32_t startup_timeout_ms,
    const PortableHelperManagerStartSession& start_session,
    const PortableHelperManagerStopSession& stop_session,
    std::string* error,
    const PortableHelperManagerCaptureFrame& capture_frame = {},
    const PortableHelperManagerInputEvent& input_event = {});

// Serializes parent-side request/response pairs over one authenticated manager
// pipe. The pipe, authorization and grant must outlive this proxy.
class PortableHelperManagerRpc final {
 public:
  PortableHelperManagerRpc(PortableHelperPipe& pipe,
                           PortableHelperAuthorization& authorization,
                           const PortableHelperGrant& grant);
  ~PortableHelperManagerRpc();
  PortableHelperManagerRpc(const PortableHelperManagerRpc&) = delete;
  PortableHelperManagerRpc& operator=(const PortableHelperManagerRpc&) = delete;

  bool CaptureFrame(uint64_t request_id, uint32_t timeout_ms,
                    PortableHelperCapturedFrame* frame, std::string* error);
  bool SendInputEvent(uint64_t request_id, const chast::InputEvent& event,
                      uint32_t timeout_ms, std::string* error);
  bool Stop(uint64_t request_id, uint32_t timeout_ms, std::string* error);

 private:
  bool fail_closed(const std::string& reason, std::string* error);

  PortableHelperPipe& pipe_;
  PortableHelperAuthorization& authorization_;
  const PortableHelperGrant& grant_;
  std::mutex rpc_mutex_;
  bool closed_ = false;
};

// Parent-side elevated-manager transaction. Start consumes the local approval
// receipt, creates the instance-scoped endpoint, launches the same binary with
// `runas`, authenticates the child identity/elevated token, and waits for the
// grant-bound WorkerReady proof. Call Start from a background thread because
// ShellExecuteEx may wait while Windows displays the UAC prompt.
class PortableHelperElevatedManagerSession final {
 public:
  using Launcher = std::function<HANDLE(const std::wstring&, DWORD&)>;
  using Revalidate = std::function<bool(std::string*)>;

  static bool Start(
      PortableHelperLocalApprovalReceipt&& approval, uint32_t timeout_ms,
      Revalidate revalidate,
      std::unique_ptr<PortableHelperElevatedManagerSession>* session,
      std::string* error, Launcher launcher = {});
  ~PortableHelperElevatedManagerSession();
  PortableHelperElevatedManagerSession(
      const PortableHelperElevatedManagerSession&) = delete;
  PortableHelperElevatedManagerSession& operator=(
      const PortableHelperElevatedManagerSession&) = delete;

  bool ready() const;
  const PortableHelperGrant& grant() const { return grant_; }
  bool CaptureFrame(uint64_t request_id, uint32_t timeout_ms,
                    PortableHelperCapturedFrame* frame, std::string* error);
  bool SendInputEvent(uint64_t request_id, const chast::InputEvent& event,
                      uint32_t timeout_ms, std::string* error);
  // A successful return requires a correlated manager cleanup ACK and process
  // exit. If the wait times out after the ACK, a later call may retry the wait.
  bool Stop(uint64_t request_id, uint32_t timeout_ms, std::string* error);

 private:
  PortableHelperElevatedManagerSession() = default;
  bool start(PortableHelperLocalApprovalReceipt&& approval,
             uint32_t timeout_ms, Revalidate revalidate,
             const Launcher& launcher,
             std::string* error);
  bool connection_is_live(std::string* error) const;
  void revoke_parent_gate() noexcept;

  PortableHelperAuthorization authorization_;
  PortableHelperGrant grant_;
  std::unique_ptr<PortableHelperPipe> pipe_;
  std::unique_ptr<PortableHelperManagerRpc> rpc_;
  Revalidate revalidate_;
  HANDLE manager_process_ = nullptr;
  bool ready_ = false;
  bool stop_acknowledged_ = false;
  bool stopped_ = false;
};

// Entry point for the same-binary `--portable-helper-manager` process. It
// verifies the runas token and exact parent identity before connecting, then
// owns StartAuthorizedGrant and the helper session until Stop/disconnect.
int run_portable_helper_elevated_manager(
    const PortableHelperInstanceId& instance_id, uint32_t parent_process_id,
    uint64_t parent_creation_time, std::string* error);

}  // namespace chast::host
