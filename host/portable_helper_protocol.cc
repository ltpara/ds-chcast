#include "portable_helper_protocol.h"

#define NOMINMAX
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <thread>

namespace chast::host {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {'C', 'H', 'A', 'S', 'T', 'P', 'H', '1'};
constexpr uint16_t kVersion = 1;
constexpr uint32_t kNoFlags = 0;

void put16(std::vector<uint8_t>& out, size_t at, uint16_t value) {
  out[at] = static_cast<uint8_t>(value);
  out[at + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    out[at + i] = static_cast<uint8_t>(value >> (i * 8));
}

void put64(std::vector<uint8_t>& out, size_t at, uint64_t value) {
  for (size_t i = 0; i < 8; ++i)
    out[at + i] = static_cast<uint8_t>(value >> (i * 8));
}

uint16_t get16(const std::vector<uint8_t>& bytes, size_t at) {
  return static_cast<uint16_t>(bytes[at]) |
         static_cast<uint16_t>(bytes[at + 1] << 8);
}

uint32_t get32(const std::vector<uint8_t>& bytes, size_t at) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i)
    value |= static_cast<uint32_t>(bytes[at + i]) << (i * 8);
  return value;
}

uint64_t get64(const std::vector<uint8_t>& bytes, size_t at) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(bytes[at + i]) << (i * 8);
  return value;
}

bool valid_type(uint32_t type) {
  return type >= static_cast<uint32_t>(PortableHelperMessageType::ClientHello) &&
         type <= static_cast<uint32_t>(PortableHelperMessageType::SessionWorkerStop);
}

bool nonzero_instance(const PortableHelperInstanceId& instance_id) {
  return std::any_of(instance_id.begin(), instance_id.end(),
                     [](uint8_t value) { return value != 0; });
}

bool fail(std::string* error, const char* message);

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

std::wstring sid_string(PSID sid) {
  if (sid == nullptr || !IsValidSid(sid)) return {};
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(sid, &text)) return {};
  std::wstring result(text);
  LocalFree(text);
  return result;
}

bool process_identity(uint32_t process_id,
                      PortableHelperProcessIdentity* identity,
                      std::string* error) {
  if (identity == nullptr || process_id == 0)
    return fail(error, "invalid helper peer process id");
  *identity = {};
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                               FALSE, process_id);
  if (process == nullptr) return fail(error, "cannot open helper peer process");
  const auto close_process = [&] { CloseHandle(process); };

  wchar_t image[32768]{};
  DWORD image_size = static_cast<DWORD>(std::size(image));
  if (!QueryFullProcessImageNameW(process, 0, image, &image_size)) {
    close_process();
    return fail(error, "cannot query helper peer image path");
  }
  identity->image_path.assign(image, image_size);
  DWORD session_id = 0;
  if (!ProcessIdToSessionId(process_id, &session_id)) {
    close_process();
    return fail(error, "cannot query helper peer Windows session");
  }
  identity->session_id = session_id;
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    close_process();
    return fail(error, "cannot query helper peer creation time");
  }
  identity->creation_time = static_cast<uint64_t>(created.dwHighDateTime) << 32 |
                            created.dwLowDateTime;
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
    close_process();
    return fail(error, "cannot open helper peer process token");
  }
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  std::vector<uint8_t> storage(bytes);
  const bool read_token = bytes != 0 &&
      GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes);
  if (read_token) {
    const auto* user_token = reinterpret_cast<const TOKEN_USER*>(storage.data());
    identity->user_sid = sid_string(user_token->User.Sid);
  }
  DWORD group_bytes = 0;
  GetTokenInformation(token, TokenGroups, nullptr, 0, &group_bytes);
  std::vector<uint8_t> groups_storage(group_bytes);
  const bool read_groups = group_bytes != 0 &&
      GetTokenInformation(token, TokenGroups, groups_storage.data(), group_bytes,
                          &group_bytes);
  if (read_groups) {
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groups_storage.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
      if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID)
        continue;
      if (!identity->logon_sid.empty()) {
        identity->logon_sid.clear();
        break;
      }
      identity->logon_sid = sid_string(groups->Groups[i].Sid);
    }
  }
  CloseHandle(token);
  close_process();
  identity->process_id = process_id;
  if (!read_token || identity->user_sid.empty() || identity->logon_sid.empty() ||
      identity->creation_time == 0)
    return fail(error, "helper peer process identity is incomplete");
  return true;
}

bool identity_matches(const PortableHelperProcessIdentity& actual,
                      const PortableHelperProcessIdentity& expected,
                      std::string* error) {
  if (expected.process_id == 0 || actual.process_id != expected.process_id)
    return fail(error, "helper pipe peer process id does not match");
  if (expected.session_id != actual.session_id)
    return fail(error, "helper pipe peer Windows session does not match");
  if (expected.creation_time == 0 ||
      expected.creation_time != actual.creation_time)
    return fail(error, "helper pipe peer process creation time does not match");
  if (expected.image_path.empty() ||
      _wcsicmp(expected.image_path.c_str(), actual.image_path.c_str()) != 0)
    return fail(error, "helper pipe peer image path does not match");
  if (expected.user_sid.empty() || expected.user_sid != actual.user_sid)
    return fail(error, "helper pipe peer token SID does not match");
  if (expected.logon_sid.empty() || expected.logon_sid != actual.logon_sid)
    return fail(error, "helper pipe peer logon SID does not match");
  return true;
}

bool peer_identity(HANDLE pipe, bool peer_is_client,
                   PortableHelperProcessIdentity* identity,
                   std::string* error) {
  ULONG process_id = 0;
  const BOOL read = peer_is_client ? GetNamedPipeClientProcessId(pipe, &process_id)
                                   : GetNamedPipeServerProcessId(pipe, &process_id);
  if (!read || process_id == 0)
    return fail(error, "cannot query helper pipe peer process id");
  return process_identity(process_id, identity, error);
}

bool complete_io(HANDLE pipe, OVERLAPPED& overlapped, DWORD timeout_ms,
                 DWORD* transferred, std::string* error) {
  const DWORD waited = WaitForSingleObject(overlapped.hEvent, timeout_ms);
  if (waited != WAIT_OBJECT_0) {
    CancelIoEx(pipe, &overlapped);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
    return fail(error, waited == WAIT_TIMEOUT ? "helper pipe I/O timed out"
                                               : "helper pipe I/O wait failed");
  }
  if (!GetOverlappedResult(pipe, &overlapped, transferred, FALSE))
    return fail(error, "helper pipe I/O did not complete");
  return true;
}

std::wstring hex_instance(const PortableHelperInstanceId& instance_id) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring encoded;
  encoded.reserve(instance_id.size() * 2);
  for (uint8_t byte : instance_id) {
    encoded.push_back(digits[byte >> 4]);
    encoded.push_back(digits[byte & 0x0f]);
  }
  return encoded;
}

bool logon_sid_from_token(HANDLE token, std::wstring* result,
                          std::string* error) {
  if (token == nullptr || result == nullptr)
    return fail(error, "missing token or helper logon SID output");
  result->clear();
  DWORD bytes = 0;
  GetTokenInformation(token, TokenGroups, nullptr, 0, &bytes);
  std::vector<uint8_t> storage(bytes);
  const bool read = bytes != 0 &&
      GetTokenInformation(token, TokenGroups, storage.data(), bytes, &bytes);
  if (!read) return fail(error, "cannot read the helper token logon SID");
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(storage.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) != SE_GROUP_LOGON_ID)
      continue;
    if (!result->empty()) {
      result->clear();
      return fail(error, "helper token has multiple logon SIDs");
    }
    *result = sid_string(groups->Groups[i].Sid);
  }
  if (result->empty())
    return fail(error, "helper token has no logon SID");
  return true;
}

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

}  // namespace

bool encode_portable_helper_message(const PortableHelperMessage& message,
                                    std::vector<uint8_t>* encoded,
                                    std::string* error) {
  if (encoded == nullptr) return fail(error, "missing helper message output");
  encoded->clear();
  if (!valid_type(static_cast<uint32_t>(message.type)))
    return fail(error, "unknown helper message type");
  if (!nonzero_instance(message.instance_id))
    return fail(error, "empty helper instance id");
  if (message.sequence == 0)
    return fail(error, "zero helper message sequence");
  if (message.payload.size() > kPortableHelperMaxPayloadBytes ||
      message.payload.size() > std::numeric_limits<uint32_t>::max())
    return fail(error, "helper message payload is too large");

  encoded->resize(kPortableHelperHeaderBytes + message.payload.size());
  std::copy(kMagic.begin(), kMagic.end(), encoded->begin());
  put16(*encoded, 8, kVersion);
  put16(*encoded, 10, static_cast<uint16_t>(kPortableHelperHeaderBytes));
  put32(*encoded, 12, static_cast<uint32_t>(message.type));
  std::copy(message.instance_id.begin(), message.instance_id.end(),
            encoded->begin() + 16);
  put64(*encoded, 32, message.sequence);
  put64(*encoded, 40, message.request_id);
  put32(*encoded, 48, static_cast<uint32_t>(message.payload.size()));
  put32(*encoded, 52, kNoFlags);
  std::copy(message.payload.begin(), message.payload.end(),
            encoded->begin() + kPortableHelperHeaderBytes);
  return true;
}

bool decode_portable_helper_message(const std::vector<uint8_t>& encoded,
                                    PortableHelperMessage* message,
                                    std::string* error) {
  if (message == nullptr) return fail(error, "missing helper message output");
  *message = {};
  if (encoded.size() < kPortableHelperHeaderBytes)
    return fail(error, "truncated helper message header");
  if (!std::equal(kMagic.begin(), kMagic.end(), encoded.begin()))
    return fail(error, "invalid helper message magic");
  if (get16(encoded, 8) != kVersion)
    return fail(error, "unsupported helper message version");
  if (get16(encoded, 10) != kPortableHelperHeaderBytes)
    return fail(error, "invalid helper message header size");
  const uint32_t type = get32(encoded, 12);
  if (!valid_type(type)) return fail(error, "unknown helper message type");
  if (get32(encoded, 52) != kNoFlags)
    return fail(error, "unsupported helper message flags");
  std::copy_n(encoded.begin() + 16, message->instance_id.size(),
              message->instance_id.begin());
  if (!nonzero_instance(message->instance_id))
    return fail(error, "empty helper instance id");
  message->sequence = get64(encoded, 32);
  if (message->sequence == 0)
    return fail(error, "zero helper message sequence");
  message->request_id = get64(encoded, 40);
  const uint32_t payload_size = get32(encoded, 48);
  if (payload_size > kPortableHelperMaxPayloadBytes)
    return fail(error, "helper message payload is too large");
  if (encoded.size() != kPortableHelperHeaderBytes + payload_size)
    return fail(error, "helper message length does not match its header");
  message->type = static_cast<PortableHelperMessageType>(type);
  message->payload.assign(encoded.begin() + kPortableHelperHeaderBytes,
                          encoded.end());
  return true;
}

bool PortableHelperSequence::Accept(const PortableHelperMessage& message,
                                    std::string* error) {
  if (message.instance_id != instance_id_)
    return fail(error, "helper message belongs to another instance");
  if (last_sequence_ == std::numeric_limits<uint64_t>::max() ||
      message.sequence != last_sequence_ + 1)
    return fail(error, "helper message sequence is replayed or out of order");
  last_sequence_ = message.sequence;
  return true;
}

bool query_portable_helper_process_identity(
    uint32_t process_id, PortableHelperProcessIdentity* identity,
    std::string* error) {
  return process_identity(process_id, identity, error);
}

bool current_portable_helper_process_identity(
    PortableHelperProcessIdentity* identity, std::string* error) {
  return process_identity(GetCurrentProcessId(), identity, error);
}

bool portable_helper_logon_sid_from_token(void* token,
                                          std::wstring* logon_sid,
                                          std::string* error) {
  return logon_sid_from_token(static_cast<HANDLE>(token), logon_sid, error);
}

bool current_portable_helper_logon_sid(std::wstring* logon_sid,
                                       std::string* error) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return fail(error, "cannot open the current process token");
  const bool ok = logon_sid_from_token(token, logon_sid, error);
  CloseHandle(token);
  return ok;
}

PortableHelperPipe::PortableHelperPipe(
    void* pipe, PortableHelperInstanceId instance_id)
    : pipe_(pipe), instance_id_(instance_id),
      receive_sequence_(instance_id) {}

PortableHelperPipe::Operation::~Operation() {
  if (owner != nullptr) owner->EndOperation();
}

bool PortableHelperPipe::BeginOperation(Operation* operation,
                                        std::string* error,
                                        bool require_authenticated_peer) {
  if (operation == nullptr)
    return fail(error, "missing helper pipe operation state");
  std::lock_guard lock(state_mutex_);
  if (closing_ || pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE)
    return fail(error, "helper named pipe is closed");
  if (require_authenticated_peer && !peer_authenticated_)
    return fail(error, "helper named pipe peer has not been authenticated");
  ++active_operations_;
  operation->owner = this;
  operation->pipe = pipe_;
  return true;
}

void PortableHelperPipe::EndOperation() {
  HANDLE close_pipe = nullptr;
  {
    std::lock_guard lock(state_mutex_);
    if (active_operations_ > 0) --active_operations_;
    if (active_operations_ == 0) {
      state_changed_.notify_all();
      if (closing_ && pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
        close_pipe = static_cast<HANDLE>(pipe_);
        pipe_ = nullptr;
      }
    }
  }
  if (close_pipe != nullptr) CloseHandle(close_pipe);
}

void PortableHelperPipe::Poison() {
  HANDLE close_pipe = nullptr;
  {
    std::lock_guard lock(state_mutex_);
    closing_ = true;
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE)
      CancelIoEx(pipe, nullptr);
    if (active_operations_ == 0 && pipe_ != nullptr &&
        pipe_ != INVALID_HANDLE_VALUE) {
      close_pipe = pipe;
      pipe_ = nullptr;
    }
  }
  if (close_pipe != nullptr) CloseHandle(close_pipe);
}

PortableHelperPipe::~PortableHelperPipe() {
  HANDLE close_pipe = nullptr;
  {
    std::unique_lock lock(state_mutex_);
    closing_ = true;
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE)
      CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
    state_changed_.wait(lock, [this] { return active_operations_ == 0; });
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
      close_pipe = static_cast<HANDLE>(pipe_);
      pipe_ = nullptr;
    }
  }
  if (close_pipe != nullptr) CloseHandle(close_pipe);
}

std::wstring PortableHelperPipe::EndpointName(
    const PortableHelperInstanceId& instance_id) {
  if (!nonzero_instance(instance_id)) return {};
  return L"\\\\.\\pipe\\ChastPortableHelper-" + hex_instance(instance_id);
}

std::wstring PortableHelperPipe::SessionWorkerEndpointName(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperInstanceId& connection_id) {
  if (!nonzero_instance(instance_id) || !nonzero_instance(connection_id))
    return {};
  return L"\\\\.\\pipe\\ChastPortableHelper-" + hex_instance(instance_id) +
         L"-Worker-" + hex_instance(connection_id);
}

std::unique_ptr<PortableHelperPipe> PortableHelperPipe::CreateServer(
    const PortableHelperInstanceId& instance_id,
    const std::wstring& authorized_logon_sid, std::string* error) {
  return CreateServerAt(instance_id, EndpointName(instance_id),
                        authorized_logon_sid, error);
}

std::unique_ptr<PortableHelperPipe>
PortableHelperPipe::CreateSessionWorkerServer(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperInstanceId& connection_id,
    const std::wstring& authorized_logon_sid, std::string* error) {
  return CreateServerAt(instance_id,
                        SessionWorkerEndpointName(instance_id, connection_id),
                        authorized_logon_sid, error);
}

std::unique_ptr<PortableHelperPipe> PortableHelperPipe::CreateServerAt(
    const PortableHelperInstanceId& instance_id, const std::wstring& endpoint,
    const std::wstring& authorized_logon_sid, std::string* error) {
  if (!nonzero_instance(instance_id)) {
    fail(error, "empty helper instance id");
    return nullptr;
  }
  if (endpoint.empty()) {
    fail(error, "invalid helper pipe endpoint scope");
    return nullptr;
  }
  PSID parsed_logon_sid = nullptr;
  if (authorized_logon_sid.empty() ||
      !ConvertStringSidToSidW(authorized_logon_sid.c_str(), &parsed_logon_sid) ||
      !IsValidSid(parsed_logon_sid)) {
    if (parsed_logon_sid != nullptr) LocalFree(parsed_logon_sid);
    fail(error, "invalid authorized helper logon SID");
    return nullptr;
  }
  LPWSTR canonical_sid = nullptr;
  const bool converted = ConvertSidToStringSidW(parsed_logon_sid, &canonical_sid) != FALSE;
  LocalFree(parsed_logon_sid);
  if (!converted || canonical_sid == nullptr) {
    fail(error, "cannot normalize the authorized helper logon SID");
    return nullptr;
  }
  const std::wstring logon_sid(canonical_sid);
  LocalFree(canonical_sid);
  const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" +
                            logon_sid + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    fail(error, "cannot create the helper pipe security descriptor");
    return nullptr;
  }
  SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
  HANDLE pipe = CreateNamedPipeW(
      endpoint.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, static_cast<DWORD>(kPortableHelperHeaderBytes +
                            kPortableHelperMaxPayloadBytes),
      static_cast<DWORD>(kPortableHelperHeaderBytes +
                         kPortableHelperMaxPayloadBytes),
      0, &security);
  const DWORD creation_error = GetLastError();
  LocalFree(descriptor);
  if (pipe == INVALID_HANDLE_VALUE) {
    SetLastError(creation_error);
    fail(error, "cannot create the helper named pipe");
    return nullptr;
  }
  return std::unique_ptr<PortableHelperPipe>(
      new PortableHelperPipe(pipe, instance_id));
}

std::unique_ptr<PortableHelperPipe> PortableHelperPipe::ConnectClient(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperProcessIdentity& expected_server,
    uint32_t timeout_ms, std::string* error) {
  return ConnectClientAt(instance_id, EndpointName(instance_id),
                         expected_server, timeout_ms, error);
}

std::unique_ptr<PortableHelperPipe>
PortableHelperPipe::ConnectSessionWorkerClient(
    const PortableHelperInstanceId& instance_id,
    const PortableHelperInstanceId& connection_id,
    const PortableHelperProcessIdentity& expected_server,
    uint32_t timeout_ms, std::string* error) {
  return ConnectClientAt(
      instance_id, SessionWorkerEndpointName(instance_id, connection_id),
      expected_server, timeout_ms, error);
}

std::unique_ptr<PortableHelperPipe> PortableHelperPipe::ConnectClientAt(
    const PortableHelperInstanceId& instance_id, const std::wstring& endpoint,
    const PortableHelperProcessIdentity& expected_server,
    uint32_t timeout_ms, std::string* error) {
  if (!nonzero_instance(instance_id)) {
    fail(error, "empty helper instance id");
    return nullptr;
  }
  if (endpoint.empty()) {
    fail(error, "cannot encode the helper pipe endpoint");
    return nullptr;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  HANDLE pipe = INVALID_HANDLE_VALUE;
  do {
    pipe = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                       nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    const DWORD code = GetLastError();
    if (code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) {
      fail(error, "cannot open the helper named pipe");
      return nullptr;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      fail(error, "helper named pipe connection timed out");
      return nullptr;
    }
    const DWORD remaining = static_cast<DWORD>(std::chrono::duration_cast<
        std::chrono::milliseconds>(deadline - now).count());
    const DWORD wait_ms = remaining < 100 ? remaining : 100;
    (void)WaitNamedPipeW(endpoint.c_str(), wait_ms);
  } while (std::chrono::steady_clock::now() < deadline);
  if (pipe == INVALID_HANDLE_VALUE) {
    fail(error, "helper named pipe connection timed out");
    return nullptr;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
    CloseHandle(pipe);
    fail(error, "cannot configure helper pipe message mode");
    return nullptr;
  }
  PortableHelperProcessIdentity actual;
  if (!peer_identity(pipe, false, &actual, error) ||
      !identity_matches(actual, expected_server, error)) {
    CloseHandle(pipe);
    return nullptr;
  }
  auto client = std::unique_ptr<PortableHelperPipe>(
      new PortableHelperPipe(pipe, instance_id));
  client->peer_identity_ = std::move(actual);
  client->peer_authenticated_ = true;
  return client;
}

bool PortableHelperPipe::Accept(
    const PortableHelperProcessIdentity& expected_client,
    uint32_t timeout_ms, std::string* error) {
  {
    std::lock_guard lock(state_mutex_);
    if (accept_started_ || peer_authenticated_)
      return fail(error, "helper named pipe accept can only run once");
    accept_started_ = true;
  }
  Operation operation;
  if (!BeginOperation(&operation, error, false)) return false;
  HANDLE pipe = static_cast<HANDLE>(operation.pipe);
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr)
    return fail(error, "cannot create helper pipe connect event");
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;
  const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
  const DWORD code = connected ? ERROR_SUCCESS : GetLastError();
  bool complete = connected != FALSE || code == ERROR_PIPE_CONNECTED;
  if (!complete && code == ERROR_IO_PENDING) {
    DWORD transferred = 0;
    complete = complete_io(pipe, overlapped, timeout_ms, &transferred, error);
  }
  CloseHandle(event);
  if (!complete) {
    if (code != ERROR_IO_PENDING && code != ERROR_PIPE_CONNECTED)
      fail(error, "helper named pipe client did not connect");
    Poison();
    return false;
  }
  PortableHelperProcessIdentity actual;
  if (!peer_identity(pipe, true, &actual, error) ||
      !identity_matches(actual, expected_client, error)) {
    Poison();
    return false;
  }
  {
    std::lock_guard lock(state_mutex_);
    if (closing_ || pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE) {
      fail(error, "helper named pipe closed during peer authentication");
      return false;
    }
    peer_identity_ = std::move(actual);
    peer_authenticated_ = true;
  }
  return true;
}

bool PortableHelperPipe::AuthenticatedPeer(
    PortableHelperProcessIdentity* identity, std::string* error) const {
  if (error != nullptr) error->clear();
  if (identity == nullptr)
    return fail(error, "missing authenticated helper peer identity output");
  std::lock_guard lock(state_mutex_);
  if (closing_ || pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE ||
      !peer_authenticated_)
    return fail(error, "helper named pipe has no authenticated live peer");
  *identity = peer_identity_;
  return true;
}

bool PortableHelperPipe::Send(PortableHelperMessage message,
                              uint32_t timeout_ms, std::string* error) {
  ByteVectorWiper payload_wiper(message.payload);
  std::lock_guard lock(send_mutex_);
  Operation operation;
  if (!BeginOperation(&operation, error)) return false;
  HANDLE pipe = static_cast<HANDLE>(operation.pipe);
  if (send_sequence_ == std::numeric_limits<uint64_t>::max())
    return fail(error, "helper send sequence is exhausted");
  const uint64_t next_sequence = send_sequence_ + 1;
  message.instance_id = instance_id_;
  message.sequence = next_sequence;
  std::vector<uint8_t> encoded;
  if (!encode_portable_helper_message(message, &encoded, error)) return false;
  ByteVectorWiper encoded_wiper(encoded);
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr)
    return fail(error, "cannot create helper pipe write event");
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;
  DWORD written = 0;
  const BOOL ok = WriteFile(pipe, encoded.data(),
                            static_cast<DWORD>(encoded.size()), &written,
                            &overlapped);
  bool complete = ok != FALSE;
  const DWORD write_error = complete ? ERROR_SUCCESS : GetLastError();
  if (!complete && write_error == ERROR_IO_PENDING)
    complete = complete_io(pipe, overlapped, timeout_ms, &written, error);
  else if (!complete)
    fail(error, "cannot write helper pipe message");
  CloseHandle(event);
  if (!complete) {
    Poison();
    return false;
  }
  if (written != encoded.size()) {
    Poison();
    return fail(error, "helper pipe message was only partially written");
  }
  send_sequence_ = next_sequence;
  return true;
}

bool PortableHelperPipe::Receive(PortableHelperMessage* message,
                                 uint32_t timeout_ms, std::string* error) {
  std::lock_guard lock(receive_mutex_);
  if (message == nullptr) return fail(error, "missing helper message output");
  Operation operation;
  if (!BeginOperation(&operation, error)) return false;
  HANDLE pipe = static_cast<HANDLE>(operation.pipe);
  std::vector<uint8_t> encoded(kPortableHelperHeaderBytes +
                               kPortableHelperMaxPayloadBytes);
  ByteVectorWiper encoded_wiper(encoded);
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr)
    return fail(error, "cannot create helper pipe read event");
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;
  DWORD received = 0;
  const BOOL ok = ReadFile(pipe, encoded.data(),
                           static_cast<DWORD>(encoded.size()), &received,
                           &overlapped);
  bool complete = ok != FALSE;
  const DWORD read_error = complete ? ERROR_SUCCESS : GetLastError();
  if (!complete && read_error == ERROR_IO_PENDING)
    complete = complete_io(pipe, overlapped, timeout_ms, &received, error);
  else if (!complete) {
    if (read_error == ERROR_MORE_DATA)
      fail(error, "helper pipe message exceeds the receive buffer");
    else
      fail(error, "cannot read helper pipe message");
  }
  CloseHandle(event);
  if (!complete) {
    Poison();
    return false;
  }
  if (received > encoded.size()) {
    Poison();
    return fail(error, "helper pipe returned an oversized message");
  }
  std::vector<uint8_t> frame(encoded.begin(), encoded.begin() + received);
  ByteVectorWiper frame_wiper(frame);
  PortableHelperMessage decoded;
  if (!decode_portable_helper_message(frame, &decoded, error)) {
    Poison();
    return false;
  }
  if (!receive_sequence_.Accept(decoded, error)) {
    if (!decoded.payload.empty())
      SecureZeroMemory(decoded.payload.data(), decoded.payload.size());
    Poison();
    return false;
  }
  *message = std::move(decoded);
  return true;
}

}  // namespace chast::host
