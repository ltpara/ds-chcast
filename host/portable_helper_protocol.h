#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chast::host {

// This framing is only for the local portable-helper named pipe. It does not
// authenticate a peer: callers must first verify the pipe server/client PID,
// executable, token SID, Windows session, and process creation time.
inline constexpr size_t kPortableHelperInstanceIdBytes = 16;
inline constexpr size_t kPortableHelperHeaderBytes = 56;
inline constexpr size_t kPortableHelperMaxPayloadBytes = 1024 * 1024;

using PortableHelperInstanceId =
    std::array<uint8_t, kPortableHelperInstanceIdBytes>;

enum class PortableHelperMessageType : uint32_t {
    ClientHello = 1,
    ServiceChallenge = 2,
    ClientProof = 3,
    HostSnapshot = 4,
    WorkerReady = 5,
    CommitTakeover = 6,
    Stop = 7,
    Status = 8,
    Acknowledgement = 9,
    CapabilityGrant = 10,
    // Strictly typed desktop capability requests. Payloads carry a fixed
    // connection/generation/session scope; they are never generic commands.
    CaptureFrameRequest = 11,
    InputEventRequest = 12,
    CaptureFrameChunk = 13,
    // Parent-to-helper control for one locally authorized session worker.
    // The helper accepts only one worker start for each service instance.
    SessionWorkerStart = 14,
    SessionWorkerStop = 15,
    // One-shot, generation-scoped request for an authenticated target-session
    // cleanup process to release the state in its durable journal.
    InputCleanupExecute = 16,
    // Dedicated authenticated host-to-elevated-manager lifecycle channel.
    // These messages are valid only on the manager endpoint, never on SCM or
    // worker endpoints.
    ManagerStart = 17,
    ManagerReady = 18,
    ManagerStop = 19,
    ManagerStopped = 20,
    ManagerCaptureFrameRequest = 21,
    ManagerInputEventRequest = 22,
};

struct PortableHelperMessage {
    PortableHelperMessageType type = PortableHelperMessageType::ClientHello;
    PortableHelperInstanceId instance_id{};
    uint64_t sequence = 0;
    uint64_t request_id = 0;
    std::vector<uint8_t> payload;
};

bool encode_portable_helper_message(const PortableHelperMessage& message,
                                    std::vector<uint8_t>* encoded,
                                    std::string* error);
bool decode_portable_helper_message(const std::vector<uint8_t>& encoded,
                                    PortableHelperMessage* message,
                                    std::string* error);

// Each pipe direction has its own sequence space. The first frame is sequence
// 1; duplicates, gaps, and cross-instance frames all terminate that direction.
class PortableHelperSequence final {
 public:
  explicit PortableHelperSequence(PortableHelperInstanceId instance_id)
      : instance_id_(instance_id) {}

  bool Accept(const PortableHelperMessage& message, std::string* error);
  uint64_t last_sequence() const { return last_sequence_; }

 private:
  PortableHelperInstanceId instance_id_{};
  uint64_t last_sequence_ = 0;
};

struct PortableHelperProcessIdentity {
  uint32_t process_id = 0;
  uint32_t session_id = 0;
  uint64_t creation_time = 0;
  std::wstring image_path;
  std::wstring user_sid;
  std::wstring logon_sid;
};

// A SYSTEM service must derive this value from the already-authorized
// interactive user's token (for example, WTSQueryUserToken), not its own
// service token.
bool portable_helper_logon_sid_from_token(void* token,
                                          std::wstring* logon_sid,
                                          std::string* error);
bool current_portable_helper_logon_sid(std::wstring* logon_sid,
                                       std::string* error);
bool query_portable_helper_process_identity(
    uint32_t process_id, PortableHelperProcessIdentity* identity,
    std::string* error);
bool current_portable_helper_process_identity(
    PortableHelperProcessIdentity* identity, std::string* error);
// A successful query may set `elevated` to false; callers decide whether that
// process is acceptable for the current operation.
bool query_portable_helper_process_elevation(uint32_t process_id,
                                             bool* elevated,
                                             std::string* error);

// A single local, message-mode named pipe bound to one helper instance. The
// caller must pass the expected peer identity from the authenticated SCM /
// process-launch state; the instance ID and pipe ACL alone are not proof.
class PortableHelperPipe final {
 public:
  static std::unique_ptr<PortableHelperPipe> CreateServer(
      const PortableHelperInstanceId& instance_id,
      const std::wstring& authorized_logon_sid, std::string* error);
  // Creates a second endpoint for one interactive desktop worker while
  // retaining the helper instance ID in every framed message. The endpoint
  // suffix is derived from the per-connection ID and cannot be caller-chosen.
  static std::unique_ptr<PortableHelperPipe> CreateSessionWorkerServer(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperInstanceId& connection_id,
      const std::wstring& authorized_logon_sid, std::string* error);
  static std::unique_ptr<PortableHelperPipe> CreateInputCleanupServer(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperInstanceId& connection_id, uint64_t generation,
      const std::wstring& authorized_logon_sid, std::string* error);
  static std::unique_ptr<PortableHelperPipe> CreateManagerServer(
      const PortableHelperInstanceId& instance_id,
      const std::wstring& authorized_logon_sid, std::string* error);
  static std::unique_ptr<PortableHelperPipe> ConnectClient(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperProcessIdentity& expected_server,
      uint32_t timeout_ms, std::string* error);
  static std::unique_ptr<PortableHelperPipe> ConnectSessionWorkerClient(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperInstanceId& connection_id,
      const PortableHelperProcessIdentity& expected_server,
      uint32_t timeout_ms, std::string* error);
  static std::unique_ptr<PortableHelperPipe> ConnectInputCleanupClient(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperInstanceId& connection_id, uint64_t generation,
      const PortableHelperProcessIdentity& expected_server,
      uint32_t timeout_ms, std::string* error);
  static std::unique_ptr<PortableHelperPipe> ConnectManagerClient(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperProcessIdentity& expected_server,
      uint32_t timeout_ms, std::string* error);
  ~PortableHelperPipe();
  PortableHelperPipe(const PortableHelperPipe&) = delete;
  PortableHelperPipe& operator=(const PortableHelperPipe&) = delete;

  static std::wstring EndpointName(
      const PortableHelperInstanceId& instance_id);
  static std::wstring SessionWorkerEndpointName(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperInstanceId& connection_id);
  static std::wstring InputCleanupEndpointName(
      const PortableHelperInstanceId& instance_id,
      const PortableHelperInstanceId& connection_id, uint64_t generation);
  static std::wstring ManagerEndpointName(
      const PortableHelperInstanceId& instance_id);

  bool Accept(const PortableHelperProcessIdentity& expected_client,
              uint32_t timeout_ms, std::string* error);
  bool AuthenticatedPeer(PortableHelperProcessIdentity* identity,
                         std::string* error) const;
  // Closes and poisons this channel so a failed RPC cannot be reused.
  void Close() noexcept;
  bool Send(PortableHelperMessage message, uint32_t timeout_ms,
            std::string* error);
  bool Receive(PortableHelperMessage* message, uint32_t timeout_ms,
               std::string* error);

 private:
  struct Operation {
    PortableHelperPipe* owner = nullptr;
    void* pipe = nullptr;
    ~Operation();
  };

  PortableHelperPipe(void* pipe, PortableHelperInstanceId instance_id);
  static std::unique_ptr<PortableHelperPipe> CreateServerAt(
      const PortableHelperInstanceId& instance_id,
      const std::wstring& endpoint,
      const std::wstring& authorized_logon_sid, std::string* error);
  static std::unique_ptr<PortableHelperPipe> ConnectClientAt(
      const PortableHelperInstanceId& instance_id,
      const std::wstring& endpoint,
      const PortableHelperProcessIdentity& expected_server,
      uint32_t timeout_ms, std::string* error);
  bool BeginOperation(Operation* operation, std::string* error,
                      bool require_authenticated_peer = true);
  void EndOperation();
  void Poison();

  void* pipe_ = nullptr;
  PortableHelperInstanceId instance_id_{};
  uint64_t send_sequence_ = 0;
  PortableHelperSequence receive_sequence_;
  mutable std::mutex state_mutex_;
  std::condition_variable state_changed_;
  size_t active_operations_ = 0;
  bool closing_ = false;
  bool accept_started_ = false;
  bool peer_authenticated_ = false;
  PortableHelperProcessIdentity peer_identity_;
  std::mutex send_mutex_;
  std::mutex receive_mutex_;
};

}  // namespace chast::host
