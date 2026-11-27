#pragma once

#include "chast/protocol.h"
#include "portable_helper_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace chast::host {

using PortableHelperConnectionId =
    std::array<uint8_t, kPortableHelperInstanceIdBytes>;
using PortableHelperSecret = std::array<uint8_t, 32>;

enum class PortableHelperOperation : uint32_t {
  CaptureFrames = 1u << 0,
  SendKeyboardInput = 1u << 1,
  SendPointerInput = 1u << 2,
};

inline constexpr uint32_t kPortableHelperKnownOperations =
    static_cast<uint32_t>(PortableHelperOperation::CaptureFrames) |
    static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput) |
    static_cast<uint32_t>(PortableHelperOperation::SendPointerInput);
inline constexpr uint32_t kPortableHelperWaitForever = 0xffffffffu;

// A per-connection capability is delivered only over the already authenticated
// local helper pipe. Never serialize it into remote protocol messages, logs, or
// persistent recovery records.
struct PortableHelperGrant {
  PortableHelperGrant() = default;
  PortableHelperGrant(const PortableHelperGrant& other) noexcept {
    *this = other;
  }
  PortableHelperGrant& operator=(const PortableHelperGrant& other) noexcept {
    if (this == &other) return *this;
    ClearSecrets();
    instance_id = other.instance_id;
    connection_id = other.connection_id;
    generation = other.generation;
    windows_session_id = other.windows_session_id;
    operations = other.operations;
    secret = other.secret;
    challenge = other.challenge;
    return *this;
  }
  ~PortableHelperGrant() { ClearSecrets(); }

  PortableHelperInstanceId instance_id{};
  PortableHelperConnectionId connection_id{};
  uint64_t generation = 0;
  uint32_t windows_session_id = 0;
  uint32_t operations = 0;
  PortableHelperSecret secret{};
  PortableHelperSecret challenge{};

 private:
  void ClearSecrets() noexcept {
    volatile uint8_t* secret_bytes = secret.data();
    volatile uint8_t* challenge_bytes = challenge.data();
    for (size_t index = 0; index < secret.size(); ++index) {
      secret_bytes[index] = 0;
      challenge_bytes[index] = 0;
    }
  }
};

enum class PortableHelperAuthorizationState {
  Idle,
  AwaitingWorkerReady,
  Ready,
  Revoking,
  Revoked,
};

class PortableHelperAuthorization;

struct PortableHelperApprovalRequestView {
  PortableHelperInstanceId instance_id{};
  PortableHelperConnectionId connection_id{};
  uint32_t windows_session_id = 0;
  uint32_t requested_operations = 0;
};

// Opaque, one-use handle for a remote request that is waiting for local
// approval. It carries no capability material and cannot itself authorize a
// helper operation.
class PortableHelperPendingApproval final {
 public:
  PortableHelperPendingApproval() = default;
  ~PortableHelperPendingApproval() { Clear(); }
  PortableHelperPendingApproval(const PortableHelperPendingApproval&) = delete;
  PortableHelperPendingApproval& operator=(
      const PortableHelperPendingApproval&) = delete;
  PortableHelperPendingApproval(PortableHelperPendingApproval&& other) noexcept;
  PortableHelperPendingApproval& operator=(
      PortableHelperPendingApproval&& other) noexcept;

  explicit operator bool() const { return valid_; }
  PortableHelperApprovalRequestView view() const;

 private:
  friend class PortableHelperLocalApprovalGate;
  void Clear() noexcept;
  std::array<uint8_t, 16> ticket_{};
  PortableHelperSecret nonce_{};
  PortableHelperApprovalRequestView view_{};
  uint64_t expires_at_ms_ = 0;
  bool valid_ = false;
};

// Created only by a local approval decision after the caller revalidates the
// live connection, session and current permissions. The receipt is scoped,
// short-lived, move-only and consumed by BeginGrant even when grant creation
// fails.
class PortableHelperLocalApprovalReceipt final {
 public:
  PortableHelperLocalApprovalReceipt() = default;
  ~PortableHelperLocalApprovalReceipt() { Clear(); }
  PortableHelperLocalApprovalReceipt(
      const PortableHelperLocalApprovalReceipt&) = delete;
  PortableHelperLocalApprovalReceipt& operator=(
      const PortableHelperLocalApprovalReceipt&) = delete;
  PortableHelperLocalApprovalReceipt(
      PortableHelperLocalApprovalReceipt&& other) noexcept;
  PortableHelperLocalApprovalReceipt& operator=(
      PortableHelperLocalApprovalReceipt&& other) noexcept;

  explicit operator bool() const { return valid_; }

 private:
  friend class PortableHelperLocalApprovalGate;
  friend class PortableHelperAuthorization;
  void Clear() noexcept;
  PortableHelperApprovalRequestView view_{};
  uint64_t expires_at_ms_ = 0;
  bool valid_ = false;
};

// Remote requests may only be queued. A receipt is minted after the UI has
// made an explicit approval decision and the supplied callback confirms that
// the same connection/session still has the approved permissions.
class PortableHelperLocalApprovalGate final {
 public:
  using Revalidate = std::function<bool(
      const PortableHelperApprovalRequestView&, uint32_t approved_operations,
      std::string* error)>;

  // Construct this gate on the local UI thread. Remote/network threads may
  // queue requests or cancel a disconnected connection, but only the bound UI
  // thread can approve or deny one.
  PortableHelperLocalApprovalGate();
  ~PortableHelperLocalApprovalGate();
  PortableHelperLocalApprovalGate(const PortableHelperLocalApprovalGate&) =
      delete;
  PortableHelperLocalApprovalGate& operator=(
      const PortableHelperLocalApprovalGate&) = delete;

  bool QueueRemoteRequest(const PortableHelperInstanceId& instance_id,
                         const PortableHelperConnectionId& connection_id,
                         uint32_t windows_session_id,
                         uint32_t requested_operations,
                         PortableHelperPendingApproval* pending,
                         std::string* error);
  bool Approve(PortableHelperPendingApproval&& pending,
               uint32_t approved_operations, const Revalidate& revalidate,
               PortableHelperLocalApprovalReceipt* receipt,
               std::string* error);
  bool Deny(PortableHelperPendingApproval&& pending, std::string* error);
  void CancelConnection(const PortableHelperConnectionId& connection_id);
  size_t pending_count() const;

 private:
  struct Entry {
    Entry() = default;
    ~Entry();
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    Entry(Entry&& other) noexcept;
    Entry& operator=(Entry&& other) noexcept;

    std::array<uint8_t, 16> ticket{};
    PortableHelperSecret nonce{};
    PortableHelperApprovalRequestView view{};
    uint64_t expires_at_ms = 0;
  };

  bool consume_pending(PortableHelperPendingApproval&& pending,
                      Entry* entry, std::string* error);
  void purge_expired_locked(uint64_t now_ms);

  mutable std::mutex mutex_;
  std::vector<Entry> pending_;
  uint32_t approval_thread_id_ = 0;
};

// Computes the worker's possession proof over all grant scope fields and the
// one-time challenge. The caller must still authenticate the pipe peer's exact
// process identity; this proof does not replace OS identity checks.
bool make_portable_helper_worker_ready_proof(
    const PortableHelperGrant& grant, PortableHelperSecret* proof,
    std::string* error);

// Fixed-size grant serialization for authenticated local manager IPC only.
// The encoded payload contains secrets; callers must wipe it after transport.
bool encode_portable_helper_grant_payload(
    const PortableHelperGrant& grant, std::vector<uint8_t>* payload,
    std::string* error);
bool decode_portable_helper_grant_payload(
    const std::vector<uint8_t>& payload,
    const PortableHelperInstanceId& expected_instance,
    uint32_t expected_windows_session_id, PortableHelperGrant* grant,
    std::string* error);

// Grant/WorkerReady exchange over a PortableHelperPipe that has already
// authenticated the exact peer process. The grant payload is fixed-size and
// contains a local-only secret; callers must not log or forward it remotely.
bool send_portable_helper_grant(PortableHelperPipe& pipe,
                               const PortableHelperGrant& grant,
                               uint32_t timeout_ms, std::string* error);
bool make_portable_helper_session_worker_start_request(
    const PortableHelperGrant& grant, uint64_t request_id,
    PortableHelperMessage* request, std::string* error);
bool parse_portable_helper_session_worker_start_request(
    const PortableHelperMessage& request,
    const PortableHelperInstanceId& expected_instance,
    uint32_t expected_session, PortableHelperGrant* grant,
    std::string* error);
bool receive_portable_helper_grant(PortableHelperPipe& pipe,
                                   PortableHelperGrant* grant,
                                   uint32_t timeout_ms, std::string* error);
bool send_portable_helper_worker_ready(PortableHelperPipe& pipe,
                                       const PortableHelperGrant& grant,
                                       uint32_t timeout_ms,
                                       std::string* error);
bool receive_portable_helper_worker_ready(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant,
    const PortableHelperProcessIdentity& expected_worker,
    uint32_t timeout_ms, std::string* error,
    PortableHelperSecret* worker_ready_proof = nullptr);

class PortableHelperAuthorization final {
 public:
  // Hold an operation lease for the entire capture/injection side effect.
  // Revocation closes the gate and waits for all leases before releasing input.
  class OperationLease final {
   public:
    OperationLease() = default;
    ~OperationLease();
    OperationLease(const OperationLease&) = delete;
    OperationLease& operator=(const OperationLease&) = delete;
    OperationLease(OperationLease&& other) noexcept;
    OperationLease& operator=(OperationLease&& other) noexcept;
    explicit operator bool() const { return owner_ != nullptr; }

   private:
    friend class PortableHelperAuthorization;
    explicit OperationLease(PortableHelperAuthorization* owner)
        : owner_(owner) {}
    void Reset() noexcept;
    PortableHelperAuthorization* owner_ = nullptr;
  };

  PortableHelperAuthorization() = default;
  ~PortableHelperAuthorization();
  PortableHelperAuthorization(const PortableHelperAuthorization&) = delete;
  PortableHelperAuthorization& operator=(
      const PortableHelperAuthorization&) = delete;

  // A remote request cannot call this API directly: it must first pass through
  // PortableHelperLocalApprovalGate and consume a matching local approval
  // receipt. This creates a fresh connection generation, scoped operation
  // mask, secret, and challenge.
  bool BeginGrant(PortableHelperLocalApprovalReceipt&& approval,
                  PortableHelperGrant* grant, std::string* error);

  // Worker-side initialization after receiving a grant over an authenticated
  // per-connection helper pipe. This does not issue a capability or prove the
  // pipe peer; callers must authenticate the Session 0 helper first and send
  // WorkerReady only after the worker is prepared to enforce this local gate.
  bool AcceptWorkerGrant(const PortableHelperGrant& grant,
                         uint32_t actual_windows_session_id,
                         std::string* error);

  // Imports a capability from the exact authenticated local parent/service
  // control channel. The caller must establish explicit local consent before
  // sending it; a remote request alone is never sufficient.
  bool AcceptParentAuthorizedGrant(const PortableHelperGrant& grant,
                                   uint32_t expected_windows_session_id,
                                   std::string* error);

  // The parent uses this only after a successful correlated Start response and
  // WorkerReady proof over its authenticated helper-service control pipe.
  // The service attests that it authenticated the actual worker identity.
  bool ConfirmBrokerManagedWorkerReady(
      const PortableHelperGrant& grant,
      const PortableHelperSecret& proof, std::string* error);

  // WorkerReady is accepted only for the exact expected desktop worker, whose
  // Windows session must equal the grant's target session, and a valid
  // challenge proof bound to the currently outstanding generation. Do not pass
  // the Session 0 broker identity as expected_worker.
  bool ConfirmWorkerReady(const PortableHelperGrant& grant,
                          const PortableHelperProcessIdentity& expected_worker,
                          const PortableHelperProcessIdentity& actual_worker,
                          const PortableHelperSecret& proof,
                          std::string* error);

  bool Authorize(const PortableHelperInstanceId& instance_id,
                 const PortableHelperConnectionId& connection_id,
                 uint32_t windows_session_id, uint64_t generation,
                 PortableHelperOperation operation) const;

  // Acquiring a lease is the operation-level authorization boundary. The
  // caller must hold it until the corresponding side effect has returned.
  bool Acquire(const PortableHelperInstanceId& instance_id,
               const PortableHelperConnectionId& connection_id,
               uint32_t windows_session_id, uint64_t generation,
               PortableHelperOperation operation, OperationLease* lease,
               std::string* error);

  // Input permissions are revoked before the release callback runs. If input
  // was in scope, a release callback is mandatory. The grant is erased even if
  // release fails; the caller must then terminate/close the worker channel.
  bool Revoke(const std::function<bool()>& release_input,
              std::string* error);
  // Closes the gate and erases capability material, asks the worker to cancel
  // in-flight work, waits for all operation leases, then releases held input.
  // Cancellation/release failures still leave the capability revoked.
  bool Revoke(const std::function<bool()>& cancel_operations,
              const std::function<bool()>& release_input,
              std::string* error);

  // Split revocation for process supervisors: close the gate and request
  // cancellation, wait for a bounded interval, let the supervisor terminate a
  // stuck worker if needed, then complete input cleanup after all leases drain.
  bool BeginRevocation(const std::function<bool()>& cancel_operations,
                       std::string* error);
  bool WaitForOperations(uint32_t timeout_ms, std::string* error);
  bool FinishRevocation(const std::function<bool()>& release_input,
                        std::string* error);

  PortableHelperAuthorizationState state() const;
  uint64_t generation() const;

 private:
  void ReleaseOperation() noexcept;
  bool AuthorizeLocked(const PortableHelperInstanceId& instance_id,
                       const PortableHelperConnectionId& connection_id,
                       uint32_t windows_session_id, uint64_t generation,
                       PortableHelperOperation operation) const;
  bool begin_revocation_locked(std::string* error);

  mutable std::mutex mutex_;
  std::condition_variable operations_changed_;
  size_t active_operations_ = 0;
  PortableHelperAuthorizationState state_ =
      PortableHelperAuthorizationState::Idle;
  PortableHelperGrant grant_{};
  bool revocation_input_granted_ = false;
  bool revocation_cancel_completed_ = false;
  bool revocation_cancel_succeeded_ = true;
  bool revocation_finish_in_progress_ = false;
};

// Desktop IPC is intentionally limited to frame-capture requests and the
// existing validated InputEvent schema. The fixed scope envelope is bound to
// the capability; no command name, path, process, or arbitrary byte operation
// can be represented by these helpers.
bool make_portable_helper_capture_request(
    const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    PortableHelperMessage* message, std::string* error);
bool make_portable_helper_input_request(
    const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    const chast::InputEvent& event, PortableHelperMessage* message,
    std::string* error);
bool send_portable_helper_capture_request(
    PortableHelperPipe& pipe, const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    uint32_t timeout_ms, std::string* error);
bool send_portable_helper_input_request(
    PortableHelperPipe& pipe, const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    const chast::InputEvent& event, uint32_t timeout_ms,
    std::string* error);

// Validates message type, exact payload length, scope and InputEvent fields,
// then acquires a lease. Keep the lease alive through capture or input
// injection so Revoke cannot race a side effect after closing the gate.
bool acquire_portable_helper_desktop_request(
    PortableHelperAuthorization& authorization,
    const PortableHelperMessage& message,
    PortableHelperAuthorization::OperationLease* lease,
    PortableHelperOperation* operation, chast::InputEvent* input_event,
    std::string* error);
bool receive_portable_helper_desktop_request(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    PortableHelperAuthorization::OperationLease* lease,
    PortableHelperOperation* operation, chast::InputEvent* input_event,
    uint32_t timeout_ms, std::string* error);

}  // namespace chast::host
