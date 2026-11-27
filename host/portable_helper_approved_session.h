#pragma once

#include "portable_helper_service_channel.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace chast::host {

// Consumes a locally approved receipt and owns the complete parent-side
// helper session transaction: grant, instance-bound staging, service startup,
// WorkerReady, desktop RPCs, revocation, and cleanup. The service backend must
// outlive this object. Call Stop explicitly when possible; the destructor
// makes one bounded best-effort cleanup attempt.
class PortableHelperApprovedSession final {
 public:
  using Stage = std::function<bool(
      const PortableHelperInstanceId&, PortableHelperServiceSpec*,
      std::string*)>;

  static bool Start(
      PortableHelperLocalApprovalReceipt&& approval,
      PortableHelperServiceBackend& backend, Stage stage,
      uint32_t timeout_ms,
      std::unique_ptr<PortableHelperApprovedSession>* session,
      std::string* error,
      PortableHelperSecret* worker_ready_proof = nullptr);
  // Elevated manager entry point for a grant received over an authenticated
  // local parent pipe after the original host consumed its approval receipt.
  // The current process must be in `expected_windows_session_id`.
  static bool StartAuthorizedGrant(
      const PortableHelperGrant& grant,
      uint32_t expected_windows_session_id,
      PortableHelperServiceBackend& backend, Stage stage,
      uint32_t timeout_ms,
      std::unique_ptr<PortableHelperApprovedSession>* session,
      std::string* error,
      PortableHelperSecret* worker_ready_proof = nullptr);
  // `session` is populated even when Start returns false. A caller should
  // inspect service_state() and retry Stop if cleanup remains pending.

  ~PortableHelperApprovedSession();
  PortableHelperApprovedSession(const PortableHelperApprovedSession&) = delete;
  PortableHelperApprovedSession& operator=(
      const PortableHelperApprovedSession&) = delete;

  bool ready() const;
  uint32_t operations() const;
  bool CaptureFrame(uint64_t request_id, uint32_t timeout_ms,
                    PortableHelperCapturedFrame* frame, std::string* error);
  bool SendInputEvent(uint64_t request_id, const chast::InputEvent& event,
                      uint32_t timeout_ms, std::string* error);
  // Stop revokes the worker capability before asking lifecycle cleanup to
  // stop/delete the service and remove its staged image. CleanupPending stays
  // retryable through subsequent Stop calls.
  bool Stop(uint32_t timeout_ms, std::string* error);
  PortableHelperServiceState service_state() const;

 private:
  explicit PortableHelperApprovedSession(PortableHelperServiceBackend& backend);
  bool start(PortableHelperLocalApprovalReceipt&& approval, Stage stage,
             uint32_t timeout_ms, PortableHelperSecret* worker_ready_proof,
             std::string* error);
  bool start_granted(Stage stage, uint32_t timeout_ms,
                     PortableHelperSecret* worker_ready_proof,
                     std::string* error);
  bool stop_locked(uint32_t timeout_ms, std::string* error);
  void revoke_before_worker(std::string* error);

  PortableHelperServiceBackend& backend_;
  mutable std::mutex mutex_;
  PortableHelperAuthorization authorization_;
  PortableHelperGrant grant_;
  std::unique_ptr<PortableHelperServiceLifecycle> lifecycle_;
  bool worker_started_ = false;
  bool ready_ = false;
};

}  // namespace chast::host
