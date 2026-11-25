#include "portable_helper_authorization.h"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace chast::host {
namespace {

constexpr char kProofDomain[] = "CHAST-PORTABLE-HELPER-WORKER-READY-V1";
constexpr size_t kDesktopScopeBytes =
    kPortableHelperInstanceIdBytes + sizeof(uint64_t) + sizeof(uint32_t);
constexpr size_t kInputEventPayloadBytes = 24;
constexpr size_t kSerializedGrantBytes =
    kPortableHelperInstanceIdBytes + kPortableHelperInstanceIdBytes +
    sizeof(uint64_t) + sizeof(uint32_t) * 2 + sizeof(PortableHelperSecret) * 2;

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

class ByteVectorWiper final {
 public:
  explicit ByteVectorWiper(std::vector<uint8_t>& bytes)
      : bytes_(bytes), size_(bytes.size()) {}
  ~ByteVectorWiper() {
    if (!bytes_.empty()) SecureZeroMemory(bytes_.data(), size_);
  }
  ByteVectorWiper(const ByteVectorWiper&) = delete;
  ByteVectorWiper& operator=(const ByteVectorWiper&) = delete;

 private:
  std::vector<uint8_t>& bytes_;
  size_t size_;
};

bool any_nonzero(const uint8_t* bytes, size_t count) {
  return bytes != nullptr &&
      std::any_of(bytes, bytes + count, [](uint8_t value) { return value != 0; });
}

void append_u32(std::vector<uint8_t>* bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes->push_back(static_cast<uint8_t>(value >> shift));
}

void append_u64(std::vector<uint8_t>* bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    bytes->push_back(static_cast<uint8_t>(value >> shift));
}

bool valid_operation_mask(uint32_t operations) {
  return operations != 0 &&
      (operations & ~kPortableHelperKnownOperations) == 0;
}

bool valid_operation(PortableHelperOperation operation) {
  const uint32_t value = static_cast<uint32_t>(operation);
  return value != 0 && (value & (value - 1)) == 0 &&
      (value & kPortableHelperKnownOperations) != 0;
}

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index)
    value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8);
  return value;
}

uint64_t read_u64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0;
  for (size_t index = 0; index < sizeof(value); ++index)
    value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
  return value;
}

void append_desktop_scope(std::vector<uint8_t>* payload,
                          const PortableHelperGrant& grant) {
  payload->insert(payload->end(), grant.connection_id.begin(),
                  grant.connection_id.end());
  append_u64(payload, grant.generation);
  append_u32(payload, grant.windows_session_id);
}

bool read_desktop_scope(const std::vector<uint8_t>& payload,
                        PortableHelperConnectionId* connection_id,
                        uint64_t* generation, uint32_t* windows_session_id,
                        std::string* error) {
  if (connection_id == nullptr || generation == nullptr ||
      windows_session_id == nullptr || payload.size() < kDesktopScopeBytes)
    return fail(error, "portable helper desktop request scope is truncated");
  std::copy_n(payload.begin(), connection_id->size(), connection_id->begin());
  *generation = read_u64(payload, connection_id->size());
  *windows_session_id = read_u32(payload,
      connection_id->size() + sizeof(*generation));
  if (!any_nonzero(connection_id->data(), connection_id->size()) ||
      *generation == 0 || *windows_session_id == 0)
    return fail(error, "portable helper desktop request scope is invalid");
  return true;
}

bool input_event_operation(const chast::InputEvent& event,
                           PortableHelperOperation* operation,
                           std::string* error) {
  if (operation == nullptr)
    return fail(error, "missing portable helper input operation output");
  switch (event.kind) {
    case chast::InputKind::Key:
    case chast::InputKind::LegacyKey:
      *operation = PortableHelperOperation::SendKeyboardInput;
      return true;
    case chast::InputKind::Mouse:
    case chast::InputKind::Touch:
    case chast::InputKind::Pen:
    case chast::InputKind::Gesture:
      *operation = PortableHelperOperation::SendPointerInput;
      return true;
  }
  return fail(error, "portable helper input request has an unknown event kind");
}

bool encode_grant_payload(const PortableHelperGrant& grant,
                          std::vector<uint8_t>* payload,
                          std::string* error) {
  if (payload == nullptr) return fail(error, "missing helper grant payload output");
  payload->clear();
  if (!any_nonzero(grant.instance_id.data(), grant.instance_id.size()) ||
      !any_nonzero(grant.connection_id.data(), grant.connection_id.size()) ||
      grant.generation == 0 || grant.windows_session_id == 0 ||
      !valid_operation_mask(grant.operations) ||
      !any_nonzero(grant.secret.data(), grant.secret.size()) ||
      !any_nonzero(grant.challenge.data(), grant.challenge.size()))
    return fail(error, "portable helper grant is incomplete");
  payload->reserve(kSerializedGrantBytes);
  payload->insert(payload->end(), grant.instance_id.begin(), grant.instance_id.end());
  payload->insert(payload->end(), grant.connection_id.begin(), grant.connection_id.end());
  append_u64(payload, grant.generation);
  append_u32(payload, grant.windows_session_id);
  append_u32(payload, grant.operations);
  payload->insert(payload->end(), grant.secret.begin(), grant.secret.end());
  payload->insert(payload->end(), grant.challenge.begin(), grant.challenge.end());
  return true;
}

bool decode_grant_payload(const std::vector<uint8_t>& payload,
                          PortableHelperGrant* grant,
                          std::string* error) {
  if (grant == nullptr) return fail(error, "missing helper grant output");
  if (payload.size() != kSerializedGrantBytes)
    return fail(error, "portable helper grant payload has an invalid size");
  PortableHelperGrant parsed;
  size_t offset = 0;
  std::copy_n(payload.begin() + offset, parsed.instance_id.size(),
              parsed.instance_id.begin());
  offset += parsed.instance_id.size();
  std::copy_n(payload.begin() + offset, parsed.connection_id.size(),
              parsed.connection_id.begin());
  offset += parsed.connection_id.size();
  parsed.generation = read_u64(payload, offset);
  offset += sizeof(parsed.generation);
  parsed.windows_session_id = read_u32(payload, offset);
  offset += sizeof(parsed.windows_session_id);
  parsed.operations = read_u32(payload, offset);
  offset += sizeof(parsed.operations);
  std::copy_n(payload.begin() + offset, parsed.secret.size(),
              parsed.secret.begin());
  offset += parsed.secret.size();
  std::copy_n(payload.begin() + offset, parsed.challenge.size(),
              parsed.challenge.begin());
  if (!any_nonzero(parsed.instance_id.data(), parsed.instance_id.size()) ||
      !any_nonzero(parsed.connection_id.data(), parsed.connection_id.size()) ||
      parsed.generation == 0 || parsed.windows_session_id == 0 ||
      !valid_operation_mask(parsed.operations) ||
      !any_nonzero(parsed.secret.data(), parsed.secret.size()) ||
      !any_nonzero(parsed.challenge.data(), parsed.challenge.size()))
    return fail(error, "portable helper grant payload contains invalid fields");
  *grant = parsed;
  return true;
}

bool constant_time_equal(const PortableHelperSecret& left,
                         const PortableHelperSecret& right) {
  uint8_t difference = 0;
  for (size_t index = 0; index < left.size(); ++index)
    difference |= left[index] ^ right[index];
  return difference == 0;
}

bool equal_identity(const PortableHelperProcessIdentity& actual,
                    const PortableHelperProcessIdentity& expected) {
  return expected.process_id != 0 && actual.process_id == expected.process_id &&
      actual.session_id == expected.session_id &&
      expected.creation_time != 0 &&
      actual.creation_time == expected.creation_time &&
      !expected.image_path.empty() &&
      _wcsicmp(actual.image_path.c_str(), expected.image_path.c_str()) == 0 &&
      !expected.user_sid.empty() &&
      _wcsicmp(actual.user_sid.c_str(), expected.user_sid.c_str()) == 0 &&
      !expected.logon_sid.empty() &&
      _wcsicmp(actual.logon_sid.c_str(), expected.logon_sid.c_str()) == 0;
}

bool make_proof(const PortableHelperGrant& grant,
                PortableHelperSecret* proof, std::string* error) {
  if (proof == nullptr) return fail(error, "missing helper proof output");
  proof->fill(0);
  if (!any_nonzero(grant.instance_id.data(), grant.instance_id.size()) ||
      !any_nonzero(grant.connection_id.data(), grant.connection_id.size()) ||
      grant.generation == 0 || grant.windows_session_id == 0 ||
      !valid_operation_mask(grant.operations) ||
      !any_nonzero(grant.secret.data(), grant.secret.size()) ||
      !any_nonzero(grant.challenge.data(), grant.challenge.size()))
    return fail(error, "portable helper grant is incomplete");

  std::vector<uint8_t> message;
  message.reserve(sizeof(kProofDomain) + grant.instance_id.size() +
                  grant.connection_id.size() + sizeof(uint64_t) +
                  sizeof(uint32_t) * 2 + grant.challenge.size());
  message.insert(message.end(), kProofDomain,
                 kProofDomain + sizeof(kProofDomain) - 1);
  message.insert(message.end(), grant.instance_id.begin(), grant.instance_id.end());
  message.insert(message.end(), grant.connection_id.begin(), grant.connection_id.end());
  append_u64(&message, grant.generation);
  append_u32(&message, grant.windows_session_id);
  append_u32(&message, grant.operations);
  message.insert(message.end(), grant.challenge.begin(), grant.challenge.end());

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<uint8_t> hash_object;
  DWORD object_size = 0;
  DWORD result_size = 0;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
  if (status < 0) return fail(error, "cannot open helper HMAC provider");
  status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0);
  if (status < 0 || object_size == 0) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return fail(error, "cannot query helper HMAC provider size");
  }
  hash_object.resize(object_size);
  status = BCryptCreateHash(algorithm, &hash, hash_object.data(), object_size,
      const_cast<PUCHAR>(grant.secret.data()),
      static_cast<ULONG>(grant.secret.size()), 0);
  if (status >= 0)
    status = BCryptHashData(hash, message.data(),
                            static_cast<ULONG>(message.size()), 0);
  if (status >= 0)
    status = BCryptFinishHash(hash, proof->data(),
                              static_cast<ULONG>(proof->size()), 0);
  if (hash != nullptr) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  SecureZeroMemory(hash_object.data(), hash_object.size());
  SecureZeroMemory(message.data(), message.size());
  if (status < 0) {
    proof->fill(0);
    return fail(error, "cannot compute helper worker-ready proof");
  }
  return true;
}

void erase_grant(PortableHelperGrant* grant) {
  if (grant == nullptr) return;
  SecureZeroMemory(grant->secret.data(), grant->secret.size());
  SecureZeroMemory(grant->challenge.data(), grant->challenge.size());
  grant->operations = 0;
}

}  // namespace

bool make_portable_helper_worker_ready_proof(
    const PortableHelperGrant& grant, PortableHelperSecret* proof,
    std::string* error) {
  if (error != nullptr) error->clear();
  return make_proof(grant, proof, error);
}

bool send_portable_helper_grant(PortableHelperPipe& pipe,
                               const PortableHelperGrant& grant,
                               uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperMessage message;
  message.type = PortableHelperMessageType::CapabilityGrant;
  message.instance_id = grant.instance_id;
  message.request_id = grant.generation;
  if (!encode_grant_payload(grant, &message.payload, error)) return false;
  return pipe.Send(std::move(message), timeout_ms, error);
}

bool receive_portable_helper_grant(PortableHelperPipe& pipe,
                                   PortableHelperGrant* grant,
                                   uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  if (grant == nullptr) return fail(error, "missing portable helper grant output");
  PortableHelperMessage message;
  if (!pipe.Receive(&message, timeout_ms, error)) return false;
  ByteVectorWiper payload_wiper(message.payload);
  if (message.type != PortableHelperMessageType::CapabilityGrant ||
      message.request_id == 0)
    return fail(error, "expected a portable helper capability grant");
  PortableHelperGrant parsed;
  if (!decode_grant_payload(message.payload, &parsed, error)) return false;
  if (message.instance_id != parsed.instance_id ||
      message.request_id != parsed.generation)
    return fail(error, "portable helper grant frame scope does not match its payload");
  *grant = parsed;
  return true;
}

bool send_portable_helper_worker_ready(PortableHelperPipe& pipe,
                                       const PortableHelperGrant& grant,
                                       uint32_t timeout_ms,
                                       std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperSecret proof{};
  if (!make_proof(grant, &proof, error)) return false;
  PortableHelperMessage message;
  message.type = PortableHelperMessageType::WorkerReady;
  message.instance_id = grant.instance_id;
  message.request_id = grant.generation;
  message.payload.assign(proof.begin(), proof.end());
  SecureZeroMemory(proof.data(), proof.size());
  return pipe.Send(std::move(message), timeout_ms, error);
}

bool receive_portable_helper_worker_ready(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant,
    const PortableHelperProcessIdentity& expected_worker,
    uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperMessage message;
  if (!pipe.Receive(&message, timeout_ms, error)) return false;
  ByteVectorWiper payload_wiper(message.payload);
  if (message.type != PortableHelperMessageType::WorkerReady ||
      message.instance_id != grant.instance_id ||
      message.request_id != grant.generation ||
      message.payload.size() != PortableHelperSecret{}.size())
    return fail(error, "portable helper WorkerReady frame has an invalid scope or size");
  PortableHelperSecret proof{};
  std::copy(message.payload.begin(), message.payload.end(), proof.begin());
  PortableHelperProcessIdentity actual_worker;
  if (!pipe.AuthenticatedPeer(&actual_worker, error)) {
    SecureZeroMemory(proof.data(), proof.size());
    return false;
  }
  const bool ready = authorization.ConfirmWorkerReady(
      grant, expected_worker, actual_worker, proof, error);
  SecureZeroMemory(proof.data(), proof.size());
  return ready;
}

PortableHelperAuthorization::~PortableHelperAuthorization() {
  std::unique_lock lock(mutex_);
  state_ = PortableHelperAuthorizationState::Revoking;
  operations_changed_.wait(lock, [this] { return active_operations_ == 0; });
  erase_grant(&grant_);
}

PortableHelperAuthorization::OperationLease::~OperationLease() {
  Reset();
}

PortableHelperAuthorization::OperationLease::OperationLease(
    OperationLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

PortableHelperAuthorization::OperationLease&
PortableHelperAuthorization::OperationLease::operator=(
    OperationLease&& other) noexcept {
  if (this != &other) {
    Reset();
    owner_ = std::exchange(other.owner_, nullptr);
  }
  return *this;
}

void PortableHelperAuthorization::OperationLease::Reset() noexcept {
  if (owner_ == nullptr) return;
  PortableHelperAuthorization* owner = std::exchange(owner_, nullptr);
  owner->ReleaseOperation();
}

bool PortableHelperAuthorization::BeginGrant(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperConnectionId& connection_id,
    uint32_t windows_session_id, uint32_t operations,
    PortableHelperGrant* grant, std::string* error) {
  if (error != nullptr) error->clear();
  if (grant == nullptr) return fail(error, "missing portable helper grant output");
  if (!any_nonzero(instance_id.data(), instance_id.size()) ||
      !any_nonzero(connection_id.data(), connection_id.size()) ||
      windows_session_id == 0 || !valid_operation_mask(operations))
    return fail(error, "portable helper grant scope is invalid");

  std::lock_guard lock(mutex_);
  if (state_ != PortableHelperAuthorizationState::Idle &&
      state_ != PortableHelperAuthorizationState::Revoked)
    return fail(error, "portable helper authorization is already active");
  if (grant_.generation == (std::numeric_limits<uint64_t>::max)())
    return fail(error, "portable helper authorization generation exhausted");

  PortableHelperGrant next;
  next.instance_id = instance_id;
  next.connection_id = connection_id;
  next.generation = grant_.generation + 1;
  next.windows_session_id = windows_session_id;
  next.operations = operations;
  if (BCryptGenRandom(nullptr, next.secret.data(),
                      static_cast<ULONG>(next.secret.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
      BCryptGenRandom(nullptr, next.challenge.data(),
                      static_cast<ULONG>(next.challenge.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
      !any_nonzero(next.secret.data(), next.secret.size()) ||
      !any_nonzero(next.challenge.data(), next.challenge.size())) {
    erase_grant(&next);
    return fail(error, "cannot create portable helper capability material");
  }
  grant_ = next;
  *grant = next;
  erase_grant(&next);
  state_ = PortableHelperAuthorizationState::AwaitingWorkerReady;
  return true;
}

bool PortableHelperAuthorization::ConfirmWorkerReady(
    const PortableHelperGrant& grant,
    const PortableHelperProcessIdentity& expected_worker,
    const PortableHelperProcessIdentity& actual_worker,
    const PortableHelperSecret& proof, std::string* error) {
  if (error != nullptr) error->clear();
  std::lock_guard lock(mutex_);
  if (state_ != PortableHelperAuthorizationState::AwaitingWorkerReady)
    return fail(error, "portable helper is not awaiting worker readiness");
  if (grant.instance_id != grant_.instance_id ||
      grant.connection_id != grant_.connection_id ||
      grant.generation != grant_.generation ||
      grant.windows_session_id != grant_.windows_session_id ||
      grant.operations != grant_.operations ||
      !constant_time_equal(grant.secret, grant_.secret) ||
      !constant_time_equal(grant.challenge, grant_.challenge))
    return fail(error, "portable helper worker-ready grant is stale or mismatched");
  if (expected_worker.session_id != grant_.windows_session_id)
    return fail(error, "portable helper worker belongs to a different Windows session");
  if (!equal_identity(actual_worker, expected_worker))
    return fail(error, "portable helper worker process identity does not match");
  PortableHelperSecret expected_proof{};
  if (!make_proof(grant_, &expected_proof, error)) return false;
  const bool valid = constant_time_equal(expected_proof, proof);
  SecureZeroMemory(expected_proof.data(), expected_proof.size());
  if (!valid) return fail(error, "portable helper worker-ready proof is invalid");
  state_ = PortableHelperAuthorizationState::Ready;
  return true;
}

bool PortableHelperAuthorization::Authorize(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperConnectionId& connection_id,
    uint32_t windows_session_id, uint64_t generation,
    PortableHelperOperation operation) const {
  std::lock_guard lock(mutex_);
  return AuthorizeLocked(instance_id, connection_id, windows_session_id,
                         generation, operation);
}

bool PortableHelperAuthorization::AuthorizeLocked(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperConnectionId& connection_id,
    uint32_t windows_session_id, uint64_t generation,
    PortableHelperOperation operation) const {
  return valid_operation(operation) &&
      state_ == PortableHelperAuthorizationState::Ready &&
      grant_.instance_id == instance_id &&
      grant_.connection_id == connection_id &&
      grant_.windows_session_id == windows_session_id &&
      grant_.generation == generation &&
      (grant_.operations & static_cast<uint32_t>(operation)) != 0;
}

bool PortableHelperAuthorization::Acquire(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperConnectionId& connection_id,
    uint32_t windows_session_id, uint64_t generation,
    PortableHelperOperation operation, OperationLease* lease,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (lease == nullptr || static_cast<bool>(*lease))
    return fail(error, "missing or already-held portable helper operation lease");
  std::lock_guard lock(mutex_);
  if (!AuthorizeLocked(instance_id, connection_id, windows_session_id,
                       generation, operation))
    return fail(error, "portable helper operation is not authorized");
  if (active_operations_ == (std::numeric_limits<size_t>::max)())
    return fail(error, "portable helper operation lease count is exhausted");
  ++active_operations_;
  lease->owner_ = this;
  return true;
}

void PortableHelperAuthorization::ReleaseOperation() noexcept {
  std::lock_guard lock(mutex_);
  if (active_operations_ > 0) --active_operations_;
  if (active_operations_ == 0) operations_changed_.notify_all();
}

bool PortableHelperAuthorization::Revoke(
    const std::function<bool()>& release_input, std::string* error) {
  return Revoke({}, release_input, error);
}

bool PortableHelperAuthorization::Revoke(
    const std::function<bool()>& cancel_operations,
    const std::function<bool()>& release_input, std::string* error) {
  if (error != nullptr) error->clear();
  const PortableHelperAuthorizationState current = state();
  if (current == PortableHelperAuthorizationState::Idle ||
      current == PortableHelperAuthorizationState::Revoked)
    return true;

  bool began = true;
  std::string begin_error;
  if (current != PortableHelperAuthorizationState::Revoking)
    began = BeginRevocation(cancel_operations, &begin_error);

  std::string wait_error;
  if (!WaitForOperations(kPortableHelperWaitForever, &wait_error)) {
    if (error != nullptr) *error = wait_error;
    return false;
  }
  std::string finish_error;
  const bool finished = FinishRevocation(release_input, &finish_error);
  if (!began || !finished) {
    if (error != nullptr)
      *error = !finish_error.empty() ? finish_error : begin_error;
    return false;
  }
  return true;
}

bool PortableHelperAuthorization::begin_revocation_locked(
    std::string* error) {
  if (state_ == PortableHelperAuthorizationState::Revoking)
    return fail(error, "portable helper authorization revocation is in progress");
  if (state_ == PortableHelperAuthorizationState::Idle ||
      state_ == PortableHelperAuthorizationState::Revoked)
    return true;

  revocation_input_granted_ = (grant_.operations &
      (static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput) |
       static_cast<uint32_t>(PortableHelperOperation::SendPointerInput))) != 0;
  state_ = PortableHelperAuthorizationState::Revoking;
  revocation_cancel_completed_ = false;
  revocation_cancel_succeeded_ = true;
  revocation_finish_in_progress_ = false;
  const uint64_t last_generation = grant_.generation;
  erase_grant(&grant_);
  grant_.generation = last_generation;
  return true;
}

bool PortableHelperAuthorization::BeginRevocation(
    const std::function<bool()>& cancel_operations, std::string* error) {
  if (error != nullptr) error->clear();
  {
    std::lock_guard lock(mutex_);
    if (state_ == PortableHelperAuthorizationState::Idle ||
        state_ == PortableHelperAuthorizationState::Revoked)
      return true;
    if (!begin_revocation_locked(error)) return false;
  }

  bool cancellation_requested = true;
  if (cancel_operations) {
    try {
      cancellation_requested = cancel_operations();
    } catch (...) {
      cancellation_requested = false;
    }
  }
  {
    std::lock_guard lock(mutex_);
    revocation_cancel_completed_ = true;
    revocation_cancel_succeeded_ = cancellation_requested;
  }
  operations_changed_.notify_all();
  if (!cancellation_requested)
    return fail(error, "helper cancellation failed; capability was revoked and its channel must close");
  return true;
}

bool PortableHelperAuthorization::WaitForOperations(uint32_t timeout_ms,
                                                    std::string* error) {
  if (error != nullptr) error->clear();
  std::unique_lock lock(mutex_);
  if (state_ != PortableHelperAuthorizationState::Revoking)
    return fail(error, "portable helper revocation has not begun");
  const auto drained = [this] {
    return revocation_cancel_completed_ && active_operations_ == 0;
  };
  if (timeout_ms == kPortableHelperWaitForever) {
    operations_changed_.wait(lock, drained);
  } else if (!operations_changed_.wait_for(
                 lock, std::chrono::milliseconds(timeout_ms), drained)) {
    return fail(error, "portable helper operation drain timed out; authorization remains revoked");
  }
  return true;
}

bool PortableHelperAuthorization::FinishRevocation(
    const std::function<bool()>& release_input, std::string* error) {
  if (error != nullptr) error->clear();
  bool input_granted = false;
  bool cancellation_requested = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ == PortableHelperAuthorizationState::Idle ||
        state_ == PortableHelperAuthorizationState::Revoked)
      return true;
    if (state_ != PortableHelperAuthorizationState::Revoking)
      return fail(error, "portable helper revocation has not begun");
    if (!revocation_cancel_completed_)
      return fail(error, "portable helper cancellation callback is still running");
    if (active_operations_ != 0)
      return fail(error, "portable helper operations are still in flight");
    if (revocation_finish_in_progress_)
      return fail(error, "portable helper input cleanup is already in progress");
    revocation_finish_in_progress_ = true;
    input_granted = revocation_input_granted_;
    cancellation_requested = revocation_cancel_succeeded_;
  }

  bool released = true;
  if (input_granted) {
    try {
      released = release_input && release_input();
    } catch (...) {
      released = false;
    }
  }
  {
    std::lock_guard lock(mutex_);
    state_ = PortableHelperAuthorizationState::Revoked;
    revocation_input_granted_ = false;
    revocation_cancel_completed_ = false;
    revocation_cancel_succeeded_ = true;
    revocation_finish_in_progress_ = false;
  }
  operations_changed_.notify_all();
  if (!cancellation_requested)
    return fail(error, "helper cancellation failed; capability was revoked and its channel must close");
  if (!released)
    return fail(error, "remote input release failed; helper authorization was revoked and its channel must close");
  return true;
}

PortableHelperAuthorizationState PortableHelperAuthorization::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

uint64_t PortableHelperAuthorization::generation() const {
  std::lock_guard lock(mutex_);
  return grant_.generation;
}

bool make_portable_helper_capture_request(
    const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    PortableHelperMessage* message, std::string* error) {
  if (error != nullptr) error->clear();
  if (message == nullptr || request_id == 0)
    return fail(error, "missing capture request output or request ID");
  if (!authorization.Authorize(
          grant.instance_id, grant.connection_id, grant.windows_session_id,
          grant.generation, PortableHelperOperation::CaptureFrames))
    return fail(error, "portable helper capture capability is not ready");

  PortableHelperMessage encoded;
  encoded.type = PortableHelperMessageType::CaptureFrameRequest;
  encoded.instance_id = grant.instance_id;
  encoded.request_id = request_id;
  encoded.payload.reserve(kDesktopScopeBytes);
  append_desktop_scope(&encoded.payload, grant);
  *message = std::move(encoded);
  return true;
}

bool make_portable_helper_input_request(
    const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    const chast::InputEvent& event, PortableHelperMessage* message,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (message == nullptr || request_id == 0)
    return fail(error, "missing input request output or request ID");

  PortableHelperOperation operation;
  if (!input_event_operation(event, &operation, error)) return false;
  if (!authorization.Authorize(grant.instance_id, grant.connection_id,
                               grant.windows_session_id, grant.generation,
                               operation))
    return fail(error, "portable helper input capability is not ready");

  std::vector<uint8_t> event_payload = chast::make_input_event_payload(event);
  ByteVectorWiper event_wiper(event_payload);
  chast::InputEvent validated_event;
  if (!chast::parse_input_event(event_payload, validated_event, error))
    return false;

  PortableHelperMessage encoded;
  encoded.type = PortableHelperMessageType::InputEventRequest;
  encoded.instance_id = grant.instance_id;
  encoded.request_id = request_id;
  encoded.payload.reserve(kDesktopScopeBytes + event_payload.size());
  append_desktop_scope(&encoded.payload, grant);
  encoded.payload.insert(encoded.payload.end(), event_payload.begin(),
                         event_payload.end());
  *message = std::move(encoded);
  return true;
}

bool send_portable_helper_capture_request(
    PortableHelperPipe& pipe, const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperMessage message;
  if (!make_portable_helper_capture_request(authorization, grant, request_id,
                                            &message, error))
    return false;
  return pipe.Send(std::move(message), timeout_ms, error);
}

bool send_portable_helper_input_request(
    PortableHelperPipe& pipe, const PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint64_t request_id,
    const chast::InputEvent& event, uint32_t timeout_ms,
    std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperMessage message;
  if (!make_portable_helper_input_request(authorization, grant, request_id,
                                          event, &message, error))
    return false;
  return pipe.Send(std::move(message), timeout_ms, error);
}

bool acquire_portable_helper_desktop_request(
    PortableHelperAuthorization& authorization,
    const PortableHelperMessage& message,
    PortableHelperAuthorization::OperationLease* lease,
    PortableHelperOperation* operation, chast::InputEvent* input_event,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (lease == nullptr || operation == nullptr ||
      static_cast<bool>(*lease) || message.request_id == 0)
    return fail(error, "portable helper desktop request outputs or ID are invalid");

  PortableHelperConnectionId connection_id{};
  uint64_t generation = 0;
  uint32_t windows_session_id = 0;
  if (!read_desktop_scope(message.payload, &connection_id, &generation,
                          &windows_session_id, error))
    return false;

  PortableHelperOperation requested_operation;
  chast::InputEvent parsed_event;
  switch (message.type) {
    case PortableHelperMessageType::CaptureFrameRequest:
      if (message.payload.size() != kDesktopScopeBytes)
        return fail(error, "capture request carries an unexpected payload");
      requested_operation = PortableHelperOperation::CaptureFrames;
      break;
    case PortableHelperMessageType::InputEventRequest: {
      if (input_event == nullptr ||
          message.payload.size() != kDesktopScopeBytes +
                                       kInputEventPayloadBytes)
        return fail(error, "input request has an invalid payload size");
      std::vector<uint8_t> event_payload(
          message.payload.begin() + kDesktopScopeBytes,
          message.payload.end());
      ByteVectorWiper event_wiper(event_payload);
      if (!chast::parse_input_event(event_payload, parsed_event, error) ||
          !input_event_operation(parsed_event, &requested_operation, error))
        return false;
      break;
    }
    default:
      return fail(error, "portable helper message is not a desktop request");
  }

  if (!authorization.Acquire(message.instance_id, connection_id,
                             windows_session_id, generation,
                             requested_operation, lease, error))
    return false;
  *operation = requested_operation;
  if (message.type == PortableHelperMessageType::InputEventRequest)
    *input_event = parsed_event;
  else if (input_event != nullptr)
    *input_event = {};
  return true;
}

bool receive_portable_helper_desktop_request(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    PortableHelperAuthorization::OperationLease* lease,
    PortableHelperOperation* operation, chast::InputEvent* input_event,
    uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperMessage message;
  if (!pipe.Receive(&message, timeout_ms, error)) return false;
  ByteVectorWiper payload_wiper(message.payload);
  return acquire_portable_helper_desktop_request(
      authorization, message, lease, operation, input_event, error);
}

}  // namespace chast::host
