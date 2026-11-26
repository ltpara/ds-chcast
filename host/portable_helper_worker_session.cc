#define NOMINMAX
#include "portable_helper_worker_session.h"

#include <chast/machine_state_security.h>
#include <chast/png.h>

#include <sddl.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "portable_helper_recovery.h"
#include "portable_helper_staging.h"

namespace chast::host {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

bool nonzero(const PortableHelperInstanceId& value) {
  return std::any_of(value.begin(), value.end(),
                     [](uint8_t byte) { return byte != 0; });
}

std::string win32_error(const char* action, DWORD code) {
  return std::string(action) + " (Win32 error " + std::to_string(code) + ")";
}

constexpr size_t kPortableHelperFrameChunkDataBytes =
    kPortableHelperMaxPayloadBytes -
    kPortableHelperFrameChunkMetadataBytes;
constexpr uint32_t kPortableHelperCaptureOperation =
    static_cast<uint32_t>(PortableHelperOperation::CaptureFrames);

struct CaptureFrameChunkMetadata final {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t frame_number = 0;
  uint32_t total_bytes = 0;
  uint32_t chunk_index = 0;
  uint32_t chunk_count = 0;
};

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void append_u64(std::vector<uint8_t>& bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    bytes.push_back(static_cast<uint8_t>(value >> shift));
}

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8)
    value |= static_cast<uint32_t>(bytes[offset + shift / 8]) << shift;
  return value;
}

uint64_t read_u64(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8)
    value |= static_cast<uint64_t>(bytes[offset + shift / 8]) << shift;
  return value;
}

bool valid_capture_grant_scope(const PortableHelperGrant& grant,
                               std::string* error) {
  if (!nonzero(grant.instance_id) || !nonzero(grant.connection_id) ||
      grant.generation == 0 || grant.windows_session_id == 0 ||
      (grant.operations & ~kPortableHelperKnownOperations) != 0 ||
      (grant.operations & kPortableHelperCaptureOperation) == 0)
    return fail(error, "capture frame grant scope is incomplete or unauthorized");
  return true;
}

bool valid_capture_frame_dimensions(uint32_t width, uint32_t height,
                                    std::string* error) {
  const uint64_t raw_bytes = static_cast<uint64_t>(width) * height * 4;
  if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
      raw_bytes > kPortableHelperMaxCapturedFrameBytes)
    return fail(error, "capture frame dimensions exceed the bounded image size");
  return true;
}

bool read_capture_chunk_metadata(const PortableHelperGrant& grant,
                                 uint64_t request_id,
                                 const PortableHelperMessage& message,
                                 CaptureFrameChunkMetadata* metadata,
                                 std::string* error) {
  if (metadata == nullptr || request_id == 0 ||
      !valid_capture_grant_scope(grant, error) ||
      message.type != PortableHelperMessageType::CaptureFrameChunk ||
      message.instance_id != grant.instance_id ||
      message.request_id != request_id ||
      message.payload.size() < kPortableHelperFrameChunkMetadataBytes)
    return fail(error, "capture frame chunk has an invalid request or scope");

  const auto& payload = message.payload;
  if (!std::equal(grant.connection_id.begin(), grant.connection_id.end(),
                  payload.begin()) ||
      read_u64(payload, 16) != grant.generation ||
      read_u32(payload, 24) != grant.windows_session_id)
    return fail(error, "capture frame chunk does not match its authorized grant");

  metadata->width = read_u32(payload, 28);
  metadata->height = read_u32(payload, 32);
  metadata->frame_number = read_u64(payload, 36);
  metadata->total_bytes = read_u32(payload, 44);
  metadata->chunk_index = read_u32(payload, 48);
  metadata->chunk_count = read_u32(payload, 52);
  if (!valid_capture_frame_dimensions(metadata->width, metadata->height,
                                      error) ||
      metadata->total_bytes == 0 ||
      metadata->total_bytes > kPortableHelperMaxCapturedFrameBytes)
    return fail(error, "capture frame chunk declares invalid image metadata");

  const uint64_t expected_count =
      (static_cast<uint64_t>(metadata->total_bytes) +
       kPortableHelperFrameChunkDataBytes - 1) /
      kPortableHelperFrameChunkDataBytes;
  if (metadata->chunk_count != expected_count ||
      metadata->chunk_index >= metadata->chunk_count)
    return fail(error, "capture frame chunk count or index is invalid");

  const uint64_t offset =
      static_cast<uint64_t>(metadata->chunk_index) *
      kPortableHelperFrameChunkDataBytes;
  const size_t expected_data = static_cast<size_t>(std::min<uint64_t>(
      kPortableHelperFrameChunkDataBytes,
      static_cast<uint64_t>(metadata->total_bytes) - offset));
  if (payload.size() != kPortableHelperFrameChunkMetadataBytes + expected_data)
    return fail(error, "capture frame chunk length does not match its metadata");
  return true;
}

void append_capture_chunk_metadata(
    const PortableHelperGrant& grant,
    const CaptureFrameChunkMetadata& metadata,
    std::vector<uint8_t>* payload) {
  payload->insert(payload->end(), grant.connection_id.begin(),
                  grant.connection_id.end());
  append_u64(*payload, grant.generation);
  append_u32(*payload, grant.windows_session_id);
  append_u32(*payload, metadata.width);
  append_u32(*payload, metadata.height);
  append_u64(*payload, metadata.frame_number);
  append_u32(*payload, metadata.total_bytes);
  append_u32(*payload, metadata.chunk_index);
  append_u32(*payload, metadata.chunk_count);
}

bool valid_png_envelope(const std::vector<uint8_t>& png) {
  constexpr std::array<uint8_t, 8> signature =
      {137, 80, 78, 71, 13, 10, 26, 10};
  return png.size() >= signature.size() &&
      std::equal(std::begin(signature), std::end(signature), png.begin());
}

int lower_hex_digit(wchar_t value) {
  if (value >= L'0' && value <= L'9') return value - L'0';
  if (value >= L'a' && value <= L'f') return value - L'a' + 10;
  return -1;
}

template <size_t Size>
bool parse_nonzero_hex_id(std::wstring_view text,
                          std::array<uint8_t, Size>* output) {
  if (output == nullptr || text.size() != Size * 2) return false;
  output->fill(0);
  for (size_t index = 0; index < Size; ++index) {
    const int high = lower_hex_digit(text[index * 2]);
    const int low = lower_hex_digit(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    (*output)[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return nonzero(*output);
}

bool parse_canonical_decimal(std::wstring_view text, uint64_t* output) {
  if (output == nullptr || text.empty() ||
      (text.size() > 1 && text.front() == L'0'))
    return false;
  uint64_t value = 0;
  for (wchar_t character : text) {
    if (character < L'0' || character > L'9') return false;
    const uint64_t digit = static_cast<uint64_t>(character - L'0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  if (value == 0) return false;
  *output = value;
  return true;
}

class Handle final {
 public:
  explicit Handle(HANDLE value = nullptr) : value_(value) {}
  ~Handle() {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
      CloseHandle(value_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return value_; }
  HANDLE* put() {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
      CloseHandle(value_);
    value_ = nullptr;
    return &value_;
  }
  HANDLE release() { return std::exchange(value_, nullptr); }

 private:
  HANDLE value_ = nullptr;
};

class LaunchedProcess final {
 public:
  LaunchedProcess(HANDLE process, HANDLE* retain_if_unreaped)
      : process_(process), retain_if_unreaped_(retain_if_unreaped) {}
  ~LaunchedProcess() {
    if (process_ == nullptr) return;
    DWORD ignored = ERROR_SUCCESS;
    if (!TerminateAndWait(&ignored) && retain_if_unreaped_ != nullptr &&
        *retain_if_unreaped_ == nullptr)
      *retain_if_unreaped_ = release();
    if (process_ != nullptr) CloseHandle(std::exchange(process_, nullptr));
  }
  LaunchedProcess(const LaunchedProcess&) = delete;
  LaunchedProcess& operator=(const LaunchedProcess&) = delete;

  HANDLE get() const { return process_; }
  HANDLE release() { return std::exchange(process_, nullptr); }

  bool TerminateAndWait(DWORD* error_code) {
    if (error_code != nullptr) *error_code = ERROR_SUCCESS;
    if (process_ == nullptr) {
      if (error_code != nullptr) *error_code = ERROR_INVALID_HANDLE;
      return false;
    }
    DWORD state = WaitForSingleObject(process_, 0);
    if (state == WAIT_OBJECT_0) return true;
    if (state == WAIT_FAILED) {
      if (error_code != nullptr) *error_code = GetLastError();
      return false;
    }
    if (state != WAIT_TIMEOUT) {
      if (error_code != nullptr) *error_code = ERROR_INVALID_DATA;
      return false;
    }
    if (!TerminateProcess(process_, ERROR_CANCELLED)) {
      const DWORD terminate_error = GetLastError();
      state = WaitForSingleObject(process_, 0);
      if (state != WAIT_OBJECT_0) {
        if (error_code != nullptr)
          *error_code = state == WAIT_FAILED ? GetLastError() : terminate_error;
        return false;
      }
    }
    state = WaitForSingleObject(process_, 5000);
    if (state == WAIT_OBJECT_0) return true;
    if (error_code != nullptr)
      *error_code = state == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    return false;
  }

 private:
  HANDLE process_ = nullptr;
  HANDLE* retain_if_unreaped_ = nullptr;
};

std::wstring token_user_sid(HANDLE token) {
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  if (bytes == 0) return {};
  std::vector<uint8_t> storage(bytes);
  if (!GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes))
    return {};
  const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
  if (user->User.Sid == nullptr || !IsValidSid(user->User.Sid)) return {};
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(user->User.Sid, &text)) return {};
  std::wstring result(text);
  LocalFree(text);
  return result;
}

bool token_session(HANDLE token, DWORD* session) {
  if (token == nullptr || session == nullptr) return false;
  DWORD bytes = 0;
  return GetTokenInformation(token, TokenSessionId, session,
                             sizeof(*session), &bytes) != FALSE &&
      bytes == sizeof(*session);
}

bool exact_identity(const PortableHelperProcessIdentity& actual,
                    const PortableHelperProcessIdentity& expected) {
  return actual.process_id == expected.process_id &&
      actual.session_id == expected.session_id &&
      actual.creation_time == expected.creation_time &&
      !expected.image_path.empty() &&
      _wcsicmp(actual.image_path.c_str(), expected.image_path.c_str()) == 0 &&
      actual.user_sid == expected.user_sid &&
      actual.logon_sid == expected.logon_sid;
}

bool interactive_token_matches(HANDLE token,
                               const PortableHelperProcessIdentity& parent,
                               std::string* error) {
  DWORD session = 0;
  const std::wstring user_sid = token_user_sid(token);
  std::wstring logon_sid;
  if (!token_session(token, &session) || session != parent.session_id ||
      user_sid.empty() ||
      _wcsicmp(user_sid.c_str(), parent.user_sid.c_str()) != 0 ||
      !portable_helper_logon_sid_from_token(token, &logon_sid, error) ||
      _wcsicmp(logon_sid.c_str(), parent.logon_sid.c_str()) != 0 ||
      _wcsicmp(parent.user_sid.c_str(), L"S-1-5-18") == 0)
    return fail(error, "WTS user token no longer matches the authorized logon");
  return true;
}

bool process_is_live(HANDLE process, DWORD* error_code) {
  if (error_code != nullptr) *error_code = ERROR_SUCCESS;
  if (process == nullptr) {
    if (error_code != nullptr) *error_code = ERROR_INVALID_HANDLE;
    return false;
  }
  const DWORD state = WaitForSingleObject(process, 0);
  if (state == WAIT_TIMEOUT) return true;
  if (error_code != nullptr) {
    *error_code = state == WAIT_OBJECT_0
        ? ERROR_PROCESS_ABORTED
        : state == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
  }
  return false;
}

bool process_creation_time(HANDLE process, uint64_t* creation_time,
                           DWORD* error_code) {
  if (error_code != nullptr) *error_code = ERROR_SUCCESS;
  if (process == nullptr || creation_time == nullptr) {
    if (error_code != nullptr) *error_code = ERROR_INVALID_PARAMETER;
    return false;
  }
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    if (error_code != nullptr) *error_code = GetLastError();
    return false;
  }
  *creation_time = (static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                   created.dwLowDateTime;
  return true;
}

std::wstring system_winlogon_image_path() {
  std::vector<wchar_t> windows_directory(32768, L'\0');
  const UINT length = GetWindowsDirectoryW(
      windows_directory.data(), static_cast<UINT>(windows_directory.size()));
  if (length == 0 || length >= windows_directory.size()) return {};
  std::wstring path(windows_directory.data(), length);
  if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
    path.push_back(L'\\');
  path.append(L"System32\\winlogon.exe");
  return path;
}

std::wstring hex_id(const PortableHelperInstanceId& value) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring encoded;
  encoded.reserve(value.size() * 2);
  for (uint8_t byte : value) {
    encoded.push_back(digits[byte >> 4]);
    encoded.push_back(digits[byte & 0x0f]);
  }
  return encoded;
}

std::wstring quote_command_line_argument(const std::wstring& value) {
  std::wstring encoded(1, L'"');
  size_t backslashes = 0;
  for (wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      encoded.append(backslashes * 2 + 1, L'\\');
      encoded.push_back(L'"');
    } else {
      encoded.append(backslashes, L'\\');
      encoded.push_back(character);
    }
    backslashes = 0;
  }
  encoded.append(backslashes * 2, L'\\');
  encoded.push_back(L'"');
  return encoded;
}

bool worker_grant_matches_spec(const PortableHelperServiceSpec& spec,
                               const PortableHelperGrant& grant,
                               std::string* error) {
  if (grant.instance_id != spec.instance_id || grant.generation == 0 ||
      grant.windows_session_id == 0 ||
      grant.windows_session_id != spec.parent.session_id ||
      grant.operations == 0 ||
      (grant.operations & ~kPortableHelperKnownOperations) != 0 ||
      std::all_of(grant.connection_id.begin(), grant.connection_id.end(),
                  [](uint8_t byte) { return byte == 0; }))
    return fail(error, "session worker grant does not match its authorized scope");
  return true;
}

bool same_live_process(HANDLE process, uint32_t expected_pid,
                       uint64_t expected_creation_time,
                       DWORD* error_code) {
  if (error_code != nullptr) *error_code = ERROR_SUCCESS;
  if (process == nullptr || expected_pid == 0 ||
      GetProcessId(process) != expected_pid) {
    if (error_code != nullptr) *error_code = ERROR_INVALID_DATA;
    return false;
  }
  DWORD wait_error = ERROR_SUCCESS;
  uint64_t creation_time = 0;
  if (!process_is_live(process, &wait_error) ||
      !process_creation_time(process, &creation_time, &wait_error) ||
      creation_time != expected_creation_time) {
    if (error_code != nullptr)
      *error_code = wait_error != ERROR_SUCCESS ? wait_error : ERROR_INVALID_DATA;
    return false;
  }
  return true;
}

bool session_user_still_matches(const PortableHelperServiceSpec& spec,
                                std::string* error) {
  Handle user_token;
  if (!WTSQueryUserToken(spec.parent.session_id, user_token.put()))
    return fail(error, win32_error("cannot revalidate the authorized WTS user",
                                   GetLastError()));
  return interactive_token_matches(user_token.get(), spec.parent, error);
}

bool validate_launched_worker(HANDLE process, const std::wstring& image_path,
                              uint32_t session_id,
                              PortableHelperProcessIdentity* identity,
                              std::string* error) {
  if (process == nullptr || identity == nullptr)
    return fail(error, "missing launched worker process or identity output");
  const DWORD process_id = GetProcessId(process);
  if (process_id == 0 ||
      !query_portable_helper_process_identity(process_id, identity, error))
    return false;
  uint64_t handle_creation_time = 0;
  DWORD process_error = ERROR_SUCCESS;
  if (!process_creation_time(process, &handle_creation_time, &process_error) ||
      identity->process_id != process_id ||
      identity->creation_time != handle_creation_time ||
      identity->session_id != session_id ||
      _wcsicmp(identity->image_path.c_str(), image_path.c_str()) != 0 ||
      _wcsicmp(identity->user_sid.c_str(), L"S-1-5-18") != 0 ||
      identity->logon_sid.empty())
    return fail(error, "launched worker identity does not match its process handle and grant");

  Handle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, token.put()))
    return fail(error, win32_error("cannot query the launched worker token",
                                   GetLastError()));
  DWORD token_session_id = 0;
  TOKEN_TYPE token_type = TokenImpersonation;
  DWORD bytes = 0;
  std::wstring token_logon_sid;
  if (!token_session(token.get(), &token_session_id) ||
      token_session_id != session_id ||
      !GetTokenInformation(token.get(), TokenType, &token_type,
                           sizeof(token_type), &bytes) ||
      bytes != sizeof(token_type) || token_type != TokenPrimary ||
      _wcsicmp(token_user_sid(token.get()).c_str(), L"S-1-5-18") != 0 ||
      !portable_helper_logon_sid_from_token(token.get(), &token_logon_sid,
                                            error) ||
      _wcsicmp(token_logon_sid.c_str(), identity->logon_sid.c_str()) != 0)
    return fail(error, "launched worker token is not SYSTEM in the granted session");
  return true;
}

bool find_session_winlogon(DWORD session, DWORD* process_id,
                           std::string* error) {
  if (process_id == nullptr || session == 0)
    return fail(error, "winlogon lookup requires an interactive session");
  *process_id = 0;
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE)
    return fail(error, win32_error("cannot enumerate session processes",
                                   GetLastError()));
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry))
    return fail(error, win32_error("cannot read the process snapshot",
                                   GetLastError()));
  BOOL more_entries = TRUE;
  while (more_entries) {
    if (_wcsicmp(entry.szExeFile, L"winlogon.exe") == 0) {
      DWORD process_session = 0;
      if (ProcessIdToSessionId(entry.th32ProcessID, &process_session) &&
          process_session == session) {
        if (*process_id != 0)
          return fail(error, "multiple winlogon processes matched one session");
        *process_id = entry.th32ProcessID;
      }
    }
    more_entries = Process32NextW(snapshot.get(), &entry);
  }
  if (GetLastError() != ERROR_NO_MORE_FILES)
    return fail(error, win32_error("cannot finish reading the process snapshot",
                                   GetLastError()));
  if (*process_id == 0)
    return fail(error, "no winlogon process exists in the authorized session");
  return true;
}

}  // namespace

bool parse_portable_helper_session_worker_command_line(
    const std::vector<std::wstring>& arguments,
    PortableHelperSessionWorkerLaunchArguments* parsed,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (parsed == nullptr)
    return fail(error, "missing portable helper worker argument output");
  *parsed = {};
  if (arguments.size() != 10 || arguments[0].empty() ||
      arguments[1] != L"--portable-helper-worker" ||
      arguments[2] != L"--portable-helper-instance" ||
      arguments[4] != L"--portable-helper-connection" ||
      arguments[6] != L"--portable-helper-broker-pid" ||
      arguments[8] != L"--portable-helper-broker-created")
    return fail(error, "portable helper worker command line is not canonical");

  PortableHelperSessionWorkerLaunchArguments candidate;
  uint64_t broker_pid = 0;
  if (!parse_nonzero_hex_id(arguments[3], &candidate.instance_id) ||
      !parse_nonzero_hex_id(arguments[5], &candidate.connection_id) ||
      !parse_canonical_decimal(arguments[7], &broker_pid) ||
      broker_pid > std::numeric_limits<uint32_t>::max() ||
      !parse_canonical_decimal(arguments[9],
                               &candidate.broker_creation_time))
    return fail(error, "portable helper worker command line has an invalid identity");

  candidate.broker_process_id = static_cast<uint32_t>(broker_pid);
  *parsed = candidate;
  return true;
}

bool validate_portable_helper_session_worker_process_identity(
    const PortableHelperSessionWorkerLaunchArguments& arguments,
    PortableHelperProcessIdentity* worker_identity,
    PortableHelperProcessIdentity* broker_identity, std::string* error) {
  if (error != nullptr) error->clear();
  if (worker_identity == nullptr || broker_identity == nullptr)
    return fail(error, "missing portable helper worker identity output");
  *worker_identity = {};
  *broker_identity = {};
  if (!nonzero(arguments.instance_id) || !nonzero(arguments.connection_id) ||
      arguments.broker_process_id == 0 || arguments.broker_creation_time == 0)
    return fail(error, "portable helper worker identity arguments are incomplete");

  PortableHelperProcessIdentity worker;
  if (!current_portable_helper_process_identity(&worker, error)) return false;
  if (worker.session_id == 0 ||
      _wcsicmp(worker.user_sid.c_str(), L"S-1-5-18") != 0 ||
      worker.process_id != GetCurrentProcessId())
    return fail(error, "portable helper worker must run as SYSTEM in an interactive session");

  std::wstring expected_staging_directory;
  std::wstring expected_worker_image;
  if (!portable_helper_staging_paths(arguments.instance_id,
                                     &expected_staging_directory,
                                     &expected_worker_image, error) ||
      expected_worker_image.empty() ||
      _wcsicmp(worker.image_path.c_str(), expected_worker_image.c_str()) != 0)
    return fail(error, "portable helper worker image is not staged for this instance");
  if (!chast::machine_state_file_trusted(expected_worker_image, error) ||
      !verify_configured_portable_helper_publisher_signature(
          expected_worker_image, error))
    return false;

  Handle worker_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, worker_token.put()))
    return fail(error, win32_error("cannot query the desktop worker token",
                                   GetLastError()));
  DWORD worker_token_session = 0;
  TOKEN_TYPE worker_token_type = TokenImpersonation;
  DWORD token_bytes = 0;
  if (!token_session(worker_token.get(), &worker_token_session) ||
      worker_token_session != worker.session_id ||
      !GetTokenInformation(worker_token.get(), TokenType, &worker_token_type,
                           sizeof(worker_token_type), &token_bytes) ||
      token_bytes != sizeof(worker_token_type) ||
      worker_token_type != TokenPrimary ||
      _wcsicmp(token_user_sid(worker_token.get()).c_str(), L"S-1-5-18") != 0)
    return fail(error, "desktop worker token is not a primary SYSTEM token for its session");

  Handle broker_process(OpenProcess(
      SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
      arguments.broker_process_id));
  if (broker_process.get() == nullptr)
    return fail(error, win32_error("cannot open the portable helper broker",
                                   GetLastError()));
  DWORD process_error = ERROR_SUCCESS;
  if (!same_live_process(broker_process.get(), arguments.broker_process_id,
                         arguments.broker_creation_time, &process_error))
    return fail(error, win32_error("portable helper broker identity changed",
                                   process_error));

  PortableHelperProcessIdentity broker;
  if (!query_portable_helper_process_identity(arguments.broker_process_id,
                                               &broker, error) ||
      broker.process_id != arguments.broker_process_id ||
      broker.session_id != 0 ||
      broker.creation_time != arguments.broker_creation_time ||
      _wcsicmp(broker.user_sid.c_str(), L"S-1-5-18") != 0 ||
      _wcsicmp(broker.image_path.c_str(), expected_worker_image.c_str()) != 0)
    return fail(error, "portable helper broker is not the exact Session 0 staged SYSTEM process");

  Handle broker_token;
  if (!OpenProcessToken(broker_process.get(), TOKEN_QUERY, broker_token.put()))
    return fail(error, win32_error("cannot query the portable helper broker token",
                                   GetLastError()));
  DWORD broker_token_session = MAXDWORD;
  TOKEN_TYPE broker_token_type = TokenImpersonation;
  token_bytes = 0;
  if (!token_session(broker_token.get(), &broker_token_session) ||
      broker_token_session != 0 ||
      !GetTokenInformation(broker_token.get(), TokenType, &broker_token_type,
                           sizeof(broker_token_type), &token_bytes) ||
      token_bytes != sizeof(broker_token_type) ||
      broker_token_type != TokenPrimary ||
      _wcsicmp(token_user_sid(broker_token.get()).c_str(), L"S-1-5-18") != 0 ||
      !same_live_process(broker_process.get(), arguments.broker_process_id,
                         arguments.broker_creation_time, &process_error))
    return fail(error, "portable helper broker token or live process does not match its Session 0 identity");

  *worker_identity = std::move(worker);
  *broker_identity = std::move(broker);
  return true;
}

bool connect_portable_helper_session_worker_pipe(
    const PortableHelperSessionWorkerLaunchArguments& arguments,
    const PortableHelperProcessIdentity& broker_identity,
    uint32_t timeout_ms, std::unique_ptr<PortableHelperPipe>* channel,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (channel == nullptr || *channel != nullptr ||
      !nonzero(arguments.instance_id) || !nonzero(arguments.connection_id) ||
      arguments.broker_process_id == 0 || arguments.broker_creation_time == 0)
    return fail(error, "portable helper worker pipe inputs are invalid");
  if (broker_identity.process_id != arguments.broker_process_id ||
      broker_identity.creation_time != arguments.broker_creation_time ||
      broker_identity.session_id != 0 ||
      _wcsicmp(broker_identity.user_sid.c_str(), L"S-1-5-18") != 0 ||
      broker_identity.image_path.empty() || broker_identity.logon_sid.empty())
    return fail(error, "portable helper worker broker identity is outside its launch scope");

  auto connected = PortableHelperPipe::ConnectSessionWorkerClient(
      arguments.instance_id, arguments.connection_id, broker_identity,
      timeout_ms, error);
  if (!connected) return false;
  *channel = std::move(connected);
  return true;
}

bool accept_portable_helper_session_worker_grant(
    PortableHelperPipe& channel,
    const PortableHelperSessionWorkerLaunchArguments& arguments,
    const PortableHelperProcessIdentity& expected_broker,
    const PortableHelperProcessIdentity& expected_worker,
    PortableHelperAuthorization& authorization,
    PortableHelperGrant* grant, uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  if (grant == nullptr)
    return fail(error, "missing portable helper worker grant output");
  *grant = {};

  if (arguments.broker_process_id == 0 ||
      arguments.broker_creation_time == 0 ||
      expected_broker.process_id != arguments.broker_process_id ||
      expected_broker.creation_time != arguments.broker_creation_time ||
      expected_broker.session_id != 0 ||
      _wcsicmp(expected_broker.user_sid.c_str(), L"S-1-5-18") != 0 ||
      expected_worker.process_id != GetCurrentProcessId() ||
      expected_worker.session_id == 0 ||
      _wcsicmp(expected_worker.user_sid.c_str(), L"S-1-5-18") != 0 ||
      _wcsicmp(expected_worker.image_path.c_str(),
               expected_broker.image_path.c_str()) != 0)
    return fail(error, "portable helper worker grant identities do not match the launcher");

  PortableHelperProcessIdentity actual_worker;
  if (!current_portable_helper_process_identity(&actual_worker, error) ||
      !exact_identity(actual_worker, expected_worker))
    return fail(error, "portable helper worker identity changed before grant import");

  PortableHelperProcessIdentity actual_broker;
  if (!channel.AuthenticatedPeer(&actual_broker, error) ||
      !exact_identity(actual_broker, expected_broker))
    return fail(error, "portable helper session pipe is not bound to the expected broker");

  PortableHelperGrant received;
  if (!receive_portable_helper_grant(channel, &received, timeout_ms, error))
    return false;
  if (received.instance_id != arguments.instance_id ||
      received.connection_id != arguments.connection_id ||
      received.windows_session_id != expected_worker.session_id ||
      received.generation == 0 || received.operations == 0 ||
      (received.operations & ~kPortableHelperKnownOperations) != 0)
    return fail(error, "portable helper worker grant does not match the process and pipe scope");
  if (!authorization.AcceptWorkerGrant(received,
                                       expected_worker.session_id, error))
    return false;
  *grant = received;
  return true;
}

bool make_portable_helper_capture_frame_chunks(
    const PortableHelperGrant& grant, uint64_t request_id,
    const PortableHelperCapturedFrame& frame,
    std::vector<PortableHelperMessage>* chunks, std::string* error) {
  if (error != nullptr) error->clear();
  if (chunks == nullptr)
    return fail(error, "missing capture frame chunk output");
  chunks->clear();
  if (!valid_capture_grant_scope(grant, error) || request_id == 0 ||
      !valid_capture_frame_dimensions(frame.width, frame.height, error) ||
      frame.png.empty() ||
      frame.png.size() > kPortableHelperMaxCapturedFrameBytes ||
      frame.png.size() > std::numeric_limits<uint32_t>::max() ||
      !valid_png_envelope(frame.png))
    return fail(error, "captured frame is invalid or exceeds its IPC bound");

  const uint32_t total_bytes = static_cast<uint32_t>(frame.png.size());
  const uint32_t chunk_count = static_cast<uint32_t>(
      (static_cast<uint64_t>(total_bytes) +
       kPortableHelperFrameChunkDataBytes - 1) /
      kPortableHelperFrameChunkDataBytes);
  chunks->reserve(chunk_count);
  for (uint32_t index = 0; index < chunk_count; ++index) {
    const size_t offset = static_cast<size_t>(index) *
                          kPortableHelperFrameChunkDataBytes;
    const size_t length = std::min(kPortableHelperFrameChunkDataBytes,
                                   frame.png.size() - offset);
    PortableHelperMessage message;
    message.type = PortableHelperMessageType::CaptureFrameChunk;
    message.instance_id = grant.instance_id;
    message.request_id = request_id;
    CaptureFrameChunkMetadata metadata;
    metadata.width = frame.width;
    metadata.height = frame.height;
    metadata.frame_number = frame.frame_number;
    metadata.total_bytes = total_bytes;
    metadata.chunk_index = index;
    metadata.chunk_count = chunk_count;
    message.payload.reserve(kPortableHelperFrameChunkMetadataBytes + length);
    append_capture_chunk_metadata(grant, metadata, &message.payload);
    message.payload.insert(message.payload.end(), frame.png.begin() + offset,
                           frame.png.begin() + offset + length);
    chunks->push_back(std::move(message));
  }
  return true;
}

bool reassemble_portable_helper_capture_frame_chunks(
    const PortableHelperGrant& grant, uint64_t request_id,
    const std::vector<PortableHelperMessage>& chunks,
    PortableHelperCapturedFrame* frame, std::string* error) {
  if (error != nullptr) error->clear();
  if (frame == nullptr)
    return fail(error, "missing reassembled capture frame output");
  *frame = {};
  if (!valid_capture_grant_scope(grant, error) || request_id == 0 ||
      chunks.empty())
    return fail(error, "capture frame chunk set is empty or out of scope");

  CaptureFrameChunkMetadata first;
  if (!read_capture_chunk_metadata(grant, request_id, chunks.front(), &first,
                                   error) ||
      first.chunk_index != 0 || first.chunk_count != chunks.size())
    return fail(error, "capture frame chunk set has no valid first chunk");

  PortableHelperCapturedFrame assembled;
  assembled.width = first.width;
  assembled.height = first.height;
  assembled.frame_number = first.frame_number;
  assembled.png.reserve(first.total_bytes);
  for (size_t index = 0; index < chunks.size(); ++index) {
    CaptureFrameChunkMetadata metadata;
    if (!read_capture_chunk_metadata(grant, request_id, chunks[index],
                                     &metadata, error) ||
        metadata.width != first.width || metadata.height != first.height ||
        metadata.frame_number != first.frame_number ||
        metadata.total_bytes != first.total_bytes ||
        metadata.chunk_count != first.chunk_count ||
        metadata.chunk_index != index)
      return fail(error, "capture frame chunks are reordered or inconsistent");
    const auto begin = chunks[index].payload.begin() +
                       kPortableHelperFrameChunkMetadataBytes;
    assembled.png.insert(assembled.png.end(), begin,
                         chunks[index].payload.end());
  }
  if (assembled.png.size() != first.total_bytes ||
      !valid_png_envelope(assembled.png))
    return fail(error, "reassembled capture frame has an invalid PNG envelope");
  *frame = std::move(assembled);
  return true;
}

bool send_portable_helper_captured_frame(
    PortableHelperPipe& pipe, const PortableHelperGrant& grant,
    uint64_t request_id, const PortableHelperCapturedFrame& frame,
    uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  std::vector<PortableHelperMessage> chunks;
  if (!make_portable_helper_capture_frame_chunks(grant, request_id, frame,
                                                &chunks, error))
    return false;
  for (auto& chunk : chunks) {
    if (!pipe.Send(std::move(chunk), timeout_ms, error)) return false;
  }
  return true;
}

bool receive_portable_helper_captured_frame(
    PortableHelperPipe& pipe, const PortableHelperGrant& grant,
    uint64_t request_id, PortableHelperCapturedFrame* frame,
    uint32_t timeout_ms, std::string* error) {
  if (error != nullptr) error->clear();
  if (frame == nullptr)
    return fail(error, "missing received capture frame output");
  *frame = {};
  PortableHelperMessage first_chunk;
  if (!pipe.Receive(&first_chunk, timeout_ms, error)) return false;
  CaptureFrameChunkMetadata metadata;
  if (!read_capture_chunk_metadata(grant, request_id, first_chunk, &metadata,
                                   error) ||
      metadata.chunk_index != 0)
    return fail(error, "capture frame response does not begin with chunk zero");

  std::vector<PortableHelperMessage> chunks;
  chunks.reserve(metadata.chunk_count);
  chunks.push_back(std::move(first_chunk));
  for (uint32_t index = 1; index < metadata.chunk_count; ++index) {
    PortableHelperMessage chunk;
    if (!pipe.Receive(&chunk, timeout_ms, error)) return false;
    chunks.push_back(std::move(chunk));
  }
  return reassemble_portable_helper_capture_frame_chunks(
      grant, request_id, chunks, frame, error);
}

bool open_portable_helper_session_worker_token(
    const PortableHelperServiceSpec& spec, HANDLE* primary_token,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (primary_token == nullptr || *primary_token != nullptr)
    return fail(error, "session worker token output must be empty");
  if (spec.parent.session_id == 0 || spec.parent.process_id == 0 ||
      spec.parent.creation_time == 0 || spec.parent.user_sid.empty() ||
      spec.parent.logon_sid.empty() || spec.executable_path.empty())
    return fail(error, "authorized helper parent identity is incomplete");

  PortableHelperServiceSpec canonical;
  if (!make_portable_helper_service_spec(spec.instance_id,
                                         spec.executable_path, spec.parent,
                                         &canonical, error) ||
      canonical.service_name != spec.service_name ||
      _wcsicmp(canonical.staging_directory.c_str(),
               spec.staging_directory.c_str()) != 0 ||
      _wcsicmp(canonical.executable_path.c_str(),
               spec.executable_path.c_str()) != 0)
    return fail(error, "portable helper worker spec is not canonical");

  PortableHelperProcessIdentity helper;
  if (!current_portable_helper_process_identity(&helper, error)) return false;
  if (helper.session_id != 0 ||
      _wcsicmp(helper.user_sid.c_str(), L"S-1-5-18") != 0 ||
      _wcsicmp(helper.image_path.c_str(), spec.executable_path.c_str()) != 0)
    return fail(error, "session worker tokens are available only to this staged Session 0 SYSTEM helper");

  PortableHelperProcessIdentity parent;
  if (!query_portable_helper_process_identity(spec.parent.process_id, &parent,
                                               error) ||
      !exact_identity(parent, spec.parent))
    return fail(error, "authorized parent identity changed before worker launch");
  Handle parent_process(OpenProcess(
      SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
      spec.parent.process_id));
  if (parent_process.get() == nullptr)
    return fail(error, win32_error("authorized parent is not live",
                                   GetLastError()));
  DWORD process_error = ERROR_SUCCESS;
  if (GetProcessId(parent_process.get()) != spec.parent.process_id ||
      !process_is_live(parent_process.get(), &process_error))
    return fail(error, win32_error("authorized parent is not live",
                                   process_error));
  uint64_t parent_creation_time = 0;
  if (!process_creation_time(parent_process.get(), &parent_creation_time,
                             &process_error) ||
      parent_creation_time != spec.parent.creation_time)
    return fail(error, "authorized parent process creation time changed");

  Handle interactive_user_token;
  if (!WTSQueryUserToken(spec.parent.session_id,
                         interactive_user_token.put()))
    return fail(error, win32_error("cannot query the authorized session user token",
                                   GetLastError()));
  if (!interactive_token_matches(interactive_user_token.get(), spec.parent,
                                 error))
    return false;

  DWORD winlogon_pid = 0;
  if (!find_session_winlogon(spec.parent.session_id, &winlogon_pid, error))
    return false;
  Handle winlogon_process(OpenProcess(
      PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, winlogon_pid));
  if (winlogon_process.get() == nullptr)
    return fail(error, win32_error("cannot open the target session winlogon process",
                                   GetLastError()));
  if (GetProcessId(winlogon_process.get()) != winlogon_pid ||
      !process_is_live(winlogon_process.get(), &process_error))
    return fail(error, win32_error("target session winlogon process is not live",
                                   process_error));
  PortableHelperProcessIdentity winlogon;
  const std::wstring expected_winlogon_path = system_winlogon_image_path();
  uint64_t opened_winlogon_creation_time = 0;
  if (!query_portable_helper_process_identity(winlogon_pid, &winlogon, error) ||
      expected_winlogon_path.empty() ||
      winlogon.process_id != winlogon_pid ||
      winlogon.session_id != spec.parent.session_id ||
      _wcsicmp(winlogon.image_path.c_str(), expected_winlogon_path.c_str()) != 0 ||
      _wcsicmp(winlogon.user_sid.c_str(), L"S-1-5-18") != 0 ||
      !process_creation_time(winlogon_process.get(),
                             &opened_winlogon_creation_time, &process_error) ||
      winlogon.creation_time != opened_winlogon_creation_time)
    return fail(error, "target winlogon process identity is outside the SYSTEM session scope");

  Handle winlogon_token;
  if (!OpenProcessToken(winlogon_process.get(), TOKEN_QUERY | TOKEN_DUPLICATE,
                        winlogon_token.put()))
    return fail(error, win32_error("cannot open the target winlogon token",
                                   GetLastError()));
  DWORD winlogon_session = 0;
  std::wstring winlogon_logon_sid;
  if (_wcsicmp(token_user_sid(winlogon_token.get()).c_str(), L"S-1-5-18") != 0 ||
      !token_session(winlogon_token.get(), &winlogon_session) ||
      winlogon_session != spec.parent.session_id ||
      !portable_helper_logon_sid_from_token(winlogon_token.get(),
                                            &winlogon_logon_sid, error))
    return fail(error, "target winlogon token is not a SYSTEM token for the authorized session");

  Handle duplicate;
  if (!DuplicateTokenEx(winlogon_token.get(),
                        TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                            TOKEN_ADJUST_DEFAULT,
                        nullptr, SecurityImpersonation, TokenPrimary,
                        duplicate.put()))
    return fail(error, win32_error("cannot duplicate the target session SYSTEM token",
                                   GetLastError()));
  DWORD duplicate_session = 0;
  std::wstring duplicate_logon_sid;
  if (_wcsicmp(token_user_sid(duplicate.get()).c_str(), L"S-1-5-18") != 0 ||
      !token_session(duplicate.get(), &duplicate_session) ||
      duplicate_session != spec.parent.session_id ||
      !portable_helper_logon_sid_from_token(duplicate.get(),
                                            &duplicate_logon_sid, error) ||
      _wcsicmp(duplicate_logon_sid.c_str(), winlogon_logon_sid.c_str()) != 0)
    return fail(error, "duplicated worker token changed its SYSTEM session identity");

  // WTS session IDs can be reused after logoff. Re-check the authorized user
  // after opening and duplicating the desktop token before returning it.
  Handle final_user_token;
  if (!WTSQueryUserToken(spec.parent.session_id, final_user_token.put()) ||
      !interactive_token_matches(final_user_token.get(), spec.parent, error) ||
      !process_is_live(parent_process.get(), &process_error) ||
      !process_is_live(winlogon_process.get(), &process_error))
    return fail(error, "authorized session changed while preparing the worker token");

  *primary_token = duplicate.release();
  return true;
}

bool launch_portable_helper_session_worker_process(
    const PortableHelperServiceSpec& spec, const PortableHelperGrant& grant,
    HANDLE* worker_process, std::string* error) {
  if (error != nullptr) error->clear();
  if (worker_process == nullptr || *worker_process != nullptr)
    return fail(error, "session worker process output must be empty");
  if (!worker_grant_matches_spec(spec, grant, error)) return false;

  PortableHelperServiceSpec canonical;
  if (!make_portable_helper_service_spec(spec.instance_id,
                                         spec.executable_path, spec.parent,
                                         &canonical, error) ||
      canonical.service_name != spec.service_name ||
      _wcsicmp(canonical.staging_directory.c_str(),
               spec.staging_directory.c_str()) != 0 ||
      _wcsicmp(canonical.executable_path.c_str(),
               spec.executable_path.c_str()) != 0)
    return fail(error, "portable helper worker launch spec is not canonical");

  PortableHelperServiceSpec recorded;
  if (!read_portable_helper_recovery_record(spec, &recorded, error) ||
      recorded.instance_id != spec.instance_id ||
      recorded.parent.process_id != spec.parent.process_id ||
      recorded.parent.session_id != spec.parent.session_id ||
      recorded.parent.creation_time != spec.parent.creation_time ||
      _wcsicmp(recorded.parent.image_path.c_str(),
               spec.parent.image_path.c_str()) != 0 ||
      recorded.parent.user_sid != spec.parent.user_sid ||
      recorded.parent.logon_sid != spec.parent.logon_sid ||
      _wcsicmp(recorded.executable_path.c_str(), spec.executable_path.c_str()) != 0)
    return fail(error, "session worker launch does not match its protected recovery record");

  PortableHelperProcessIdentity helper;
  if (!current_portable_helper_process_identity(&helper, error) ||
      helper.session_id != 0 ||
      _wcsicmp(helper.user_sid.c_str(), L"S-1-5-18") != 0 ||
      _wcsicmp(helper.image_path.c_str(), spec.executable_path.c_str()) != 0)
    return fail(error, "session worker launch is restricted to its staged Session 0 SYSTEM helper");
  DWORD process_error = ERROR_SUCCESS;
  Handle helper_process(OpenProcess(
      SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
      helper.process_id));
  if (helper_process.get() == nullptr ||
      !same_live_process(helper_process.get(), helper.process_id,
                         helper.creation_time, &process_error))
    return fail(error, win32_error("helper process identity changed before worker launch",
                                   helper_process.get() == nullptr
                                       ? GetLastError()
                                       : process_error));
  if (!chast::machine_state_file_trusted(spec.executable_path, error) ||
      !verify_configured_portable_helper_publisher_signature(
          spec.executable_path, error))
    return false;

  HANDLE primary_token = nullptr;
  if (!open_portable_helper_session_worker_token(spec, &primary_token, error))
    return false;
  Handle token(primary_token);

  Handle authorized_parent(OpenProcess(
      SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
      spec.parent.process_id));
  if (authorized_parent.get() == nullptr ||
      !same_live_process(authorized_parent.get(), spec.parent.process_id,
                         spec.parent.creation_time, &process_error))
    return fail(error, win32_error("authorized helper parent is not live",
                                   authorized_parent.get() == nullptr
                                       ? GetLastError()
                                       : process_error));

  LPVOID environment = nullptr;
  if (!CreateEnvironmentBlock(&environment, token.get(), FALSE))
    return fail(error, win32_error("cannot create the session worker environment",
                                   GetLastError()));

  std::wstring command = quote_command_line_argument(spec.executable_path);
  command.append(L" --portable-helper-worker --portable-helper-instance ");
  command.append(hex_id(spec.instance_id));
  command.append(L" --portable-helper-connection ");
  command.append(hex_id(grant.connection_id));
  command.append(L" --portable-helper-broker-pid ");
  command.append(std::to_wstring(helper.process_id));
  command.append(L" --portable-helper-broker-created ");
  command.append(std::to_wstring(helper.creation_time));
  std::vector<wchar_t> command_buffer(command.begin(), command.end());
  command_buffer.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
  PROCESS_INFORMATION created{};
  const BOOL started = CreateProcessAsUserW(
      token.get(), spec.executable_path.c_str(), command_buffer.data(), nullptr,
      nullptr, FALSE,
      CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
      environment, spec.staging_directory.c_str(), &startup, &created);
  const DWORD create_error = started ? ERROR_SUCCESS : GetLastError();
  DestroyEnvironmentBlock(environment);
  if (!started)
    return fail(error, win32_error("cannot create the authorized session worker",
                                   create_error));

  LaunchedProcess child(created.hProcess, worker_process);
  Handle initial_thread(created.hThread);
  const auto fail_and_reap = [&](const std::string& reason) {
    if (!child.TerminateAndWait(&process_error)) {
      *worker_process = child.release();
      return fail(error, reason + "; " +
          win32_error("failed session worker has not exited", process_error));
    }
    return fail(error, reason);
  };

  PortableHelperProcessIdentity worker_identity;
  if (!validate_launched_worker(child.get(), spec.executable_path,
                                grant.windows_session_id, &worker_identity,
                                error)) {
    const std::string identity_error = error == nullptr || error->empty()
        ? "launched session worker identity validation failed"
        : *error;
    return fail_and_reap(identity_error);
  }
  if (!session_user_still_matches(spec, error) ||
      !same_live_process(helper_process.get(), helper.process_id,
                         helper.creation_time, &process_error) ||
      !same_live_process(authorized_parent.get(), spec.parent.process_id,
                         spec.parent.creation_time, &process_error) ||
      GetProcessId(helper_process.get()) != helper.process_id)
    return fail_and_reap(error != nullptr && !error->empty()
        ? *error
        : win32_error("authorized session changed before worker resume",
                       process_error));

  const DWORD previous_suspend_count = ResumeThread(initial_thread.get());
  if (previous_suspend_count == static_cast<DWORD>(-1))
    return fail_and_reap(win32_error("cannot resume the session worker",
                                     GetLastError()));
  if (previous_suspend_count != 1)
    return fail_and_reap("session worker initial thread suspend count changed unexpectedly");

  if (!session_user_still_matches(spec, error) ||
      !same_live_process(helper_process.get(), helper.process_id,
                         helper.creation_time, &process_error) ||
      !same_live_process(authorized_parent.get(), spec.parent.process_id,
                         spec.parent.creation_time, &process_error) ||
      !process_is_live(child.get(), &process_error))
    return fail_and_reap(error != nullptr && !error->empty()
        ? *error
        : win32_error("authorized session changed after worker resume",
                       process_error));

  *worker_process = child.release();
  return true;
}

bool apply_portable_helper_worker_input_request(
    PortableHelperAuthorization& authorization, chast::InputInjector& injector,
    const PortableHelperMessage& message, std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperAuthorization::OperationLease lease;
  PortableHelperOperation operation{};
  chast::InputEvent event;
  if (!acquire_portable_helper_desktop_request(
          authorization, message, &lease, &operation, &event, error))
    return false;
  if (operation != PortableHelperOperation::SendKeyboardInput &&
      operation != PortableHelperOperation::SendPointerInput)
    return fail(error, "worker input adapter received a non-input capability request");
  if (!injector.submit_and_wait(event))
    return fail(error, "worker input adapter could not enqueue the authorized event");
  return true;
}

bool capture_portable_helper_worker_frame(
    PortableHelperAuthorization& authorization,
    chast::IDxgiCapturer& capturer, const PortableHelperMessage& message,
    PortableHelperCapturedFrame* frame, std::string* error) {
  if (error != nullptr) error->clear();
  if (frame == nullptr)
    return fail(error, "missing portable helper captured-frame output");
  *frame = {};

  PortableHelperAuthorization::OperationLease lease;
  PortableHelperOperation operation{};
  if (!acquire_portable_helper_desktop_request(
          authorization, message, &lease, &operation, nullptr, error))
    return false;
  if (operation != PortableHelperOperation::CaptureFrames)
    return fail(error, "worker capture adapter received a non-capture request");

  chast::CaptureResult result =
      capturer.capture(std::chrono::milliseconds(1000));
  if (result == chast::CaptureResult::ReinitNeeded) {
    const chast::Error reinitialized = capturer.reinitialize();
    if (!reinitialized.ok())
      return fail(error, "worker desktop capturer reinitialization failed: " +
                             reinitialized.message());
    result = capturer.capture(std::chrono::milliseconds(1000));
  }
  if (result != chast::CaptureResult::Frame &&
      result != chast::CaptureResult::NoNewFrame)
    return fail(error, "worker desktop capture did not return a frame");

  const chast::FrameInfo& image = capturer.frame();
  constexpr uint64_t kMaxWorkerFrameBytes = 64ull * 1024 * 1024;
  const uint64_t bgra_bytes =
      static_cast<uint64_t>(image.width) * image.height * 4;
  if (!image.cpu_bgra || image.width == 0 || image.height == 0 ||
      image.width > 16384 || image.height > 16384 ||
      bgra_bytes > kMaxWorkerFrameBytes ||
      bgra_bytes != image.cpu_bgra->size())
    return fail(error, "worker desktop capturer did not provide a bounded BGRA frame");

  std::vector<uint8_t> encoded;
  if (!chast::encode_png_bgra(image.cpu_bgra->data(), image.width,
                              image.height, image.width * 4, encoded, error))
    return false;
  if (encoded.empty() || encoded.size() > kMaxWorkerFrameBytes)
    return fail(error, "worker desktop PNG output is empty or exceeds its frame bound");

  frame->width = image.width;
  frame->height = image.height;
  frame->frame_number = image.frame_number;
  frame->png = std::move(encoded);
  return true;
}

bool run_portable_helper_session_worker_request_loop(
    PortableHelperPipe& pipe, PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, chast::IDxgiCapturer* capturer,
    chast::InputInjector* injector, std::string* error) {
  if (error != nullptr) error->clear();
  const uint32_t input_operations =
      static_cast<uint32_t>(PortableHelperOperation::SendKeyboardInput) |
      static_cast<uint32_t>(PortableHelperOperation::SendPointerInput);
  const bool capture_granted =
      (grant.operations &
       static_cast<uint32_t>(PortableHelperOperation::CaptureFrames)) != 0;
  const bool input_granted = (grant.operations & input_operations) != 0;
  const auto finish = [&](bool operation_succeeded) {
    std::string cleanup_error;
    if (!authorization.Revoke(
            [] { return true; },
            [&] {
              if (injector != nullptr) injector->stop();
              return true;
            },
            &cleanup_error)) {
      if (error != nullptr) {
        const std::string primary = *error;
        *error = cleanup_error.empty()
            ? "session worker authorization cleanup failed"
            : "session worker authorization cleanup failed: " + cleanup_error;
        if (!primary.empty()) *error += "; request failed: " + primary;
      }
      return false;
    }
    return operation_succeeded;
  };

  if (grant.generation == 0 || grant.windows_session_id == 0 ||
      grant.operations == 0 ||
      (grant.operations & ~kPortableHelperKnownOperations) != 0 ||
      (capturer != nullptr) != capture_granted ||
      (injector != nullptr) != input_granted ||
      authorization.state() != PortableHelperAuthorizationState::Ready) {
    fail(error, "session worker adapters do not match the active grant");
    return finish(false);
  }
  if ((capture_granted && !authorization.Authorize(
          grant.instance_id, grant.connection_id, grant.windows_session_id,
          grant.generation, PortableHelperOperation::CaptureFrames)) ||
      ((grant.operations & static_cast<uint32_t>(
            PortableHelperOperation::SendKeyboardInput)) != 0 &&
       !authorization.Authorize(
           grant.instance_id, grant.connection_id, grant.windows_session_id,
           grant.generation, PortableHelperOperation::SendKeyboardInput)) ||
      ((grant.operations & static_cast<uint32_t>(
            PortableHelperOperation::SendPointerInput)) != 0 &&
       !authorization.Authorize(
           grant.instance_id, grant.connection_id, grant.windows_session_id,
           grant.generation, PortableHelperOperation::SendPointerInput)))
  {
    fail(error, "session worker grant is not ready for its adapters");
    return finish(false);
  }

  // Nothing is reported ready until the caller has initialized every adapter
  // authorized by this grant and this loop has verified the exact operation
  // scope it will serve.
  if (!send_portable_helper_worker_ready(pipe, grant, INFINITE, error))
    return finish(false);

  for (;;) {
    PortableHelperMessage request;
    if (!pipe.Receive(&request, INFINITE, error)) return finish(false);
    switch (request.type) {
      case PortableHelperMessageType::CaptureFrameRequest: {
        if (capturer == nullptr) {
          fail(error, "capture request has no initialized adapter");
          return finish(false);
        }
        PortableHelperCapturedFrame frame;
        if (!capture_portable_helper_worker_frame(
                authorization, *capturer, request, &frame, error))
          return finish(false);
        const bool sent = send_portable_helper_captured_frame(
            pipe, grant, request.request_id, frame, INFINITE, error);
        if (!frame.png.empty())
          SecureZeroMemory(frame.png.data(), frame.png.size());
        if (!sent)
          return finish(false);
        break;
      }
      case PortableHelperMessageType::InputEventRequest: {
        if (injector == nullptr) {
          fail(error, "input request has no initialized adapter");
          return finish(false);
        }
        if (!apply_portable_helper_worker_input_request(
                authorization, *injector, request, error))
          return finish(false);
        PortableHelperMessage response;
        response.type = PortableHelperMessageType::Acknowledgement;
        response.request_id = request.request_id;
        response.payload = {1};
        if (!pipe.Send(std::move(response), INFINITE, error))
          return finish(false);
        break;
      }
      case PortableHelperMessageType::Stop: {
        if (request.request_id == 0 || !request.payload.empty()) {
          fail(error, "session worker stop request is malformed");
          return finish(false);
        }
        if (!finish(true)) return false;
        PortableHelperMessage response;
        response.type = PortableHelperMessageType::Acknowledgement;
        response.request_id = request.request_id;
        response.payload = {1};
        return pipe.Send(std::move(response), INFINITE, error);
      }
      default:
        fail(error, "session worker received an unsupported message type");
        return finish(false);
    }
  }
}

bool start_portable_helper_session_worker_channel(
    const PortableHelperServiceSpec& spec,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant,
    const PortableHelperSessionWorkerLauncher& launch_worker,
    uint32_t timeout_ms,
    std::unique_ptr<PortableHelperPipe>* channel,
    HANDLE* worker_process,
    PortableHelperProcessIdentity* worker_identity,
    std::string* error,
    PortableHelperSecret* worker_ready_proof) {
  if (error != nullptr) error->clear();
  if (worker_ready_proof != nullptr) worker_ready_proof->fill(0);
  if (channel == nullptr || worker_process == nullptr ||
      worker_identity == nullptr || *channel != nullptr ||
      *worker_process != nullptr || !launch_worker)
    return fail(error, "session worker startup outputs or launcher are invalid");
  *worker_identity = {};
  if (!nonzero(spec.instance_id) || spec.instance_id != grant.instance_id ||
      !nonzero(grant.connection_id) || grant.generation == 0 ||
      grant.windows_session_id == 0 ||
      grant.windows_session_id != spec.parent.session_id ||
      grant.operations == 0 ||
      (grant.operations & ~kPortableHelperKnownOperations) != 0 ||
      spec.executable_path.empty() || spec.parent.logon_sid.empty() ||
      spec.parent.process_id == 0 || spec.parent.creation_time == 0)
    return fail(error, "session worker grant does not match its authorized parent");
  if (authorization.state() !=
          PortableHelperAuthorizationState::AwaitingWorkerReady ||
      authorization.generation() != grant.generation)
    return fail(error, "authorization is not awaiting this worker generation");

  auto server = PortableHelperPipe::CreateSessionWorkerServer(
      spec.instance_id, grant.connection_id, spec.parent.logon_sid, error);
  if (!server) return false;

  HANDLE process = nullptr;
  PortableHelperProcessIdentity identity;
  std::string launch_error;
  try {
    if (!launch_worker(spec, grant, &process, &launch_error)) {
      if (launch_error.empty())
        launch_error = "session worker launcher refused to start the process";
    }
  } catch (...) {
    launch_error = "session worker launcher raised an exception";
  }

  const auto cleanup_failed_start = [&](const std::string& primary_error) {
    server.reset();
    std::string cleanup_error;
    if (authorization.state() ==
            PortableHelperAuthorizationState::AwaitingWorkerReady &&
        !authorization.Revoke([] { return true; }, [] { return true; },
                              &cleanup_error)) {
      // Even if local cleanup reports an error, Revoke clears the capability
      // before invoking either callback. Keep the original startup failure as
      // the primary error and retain the cleanup detail for diagnosis.
    }
    const std::string failure = cleanup_error.empty()
        ? primary_error
        : primary_error + "; pending grant revocation: " + cleanup_error;
    if (process == nullptr || process == INVALID_HANDLE_VALUE)
      return fail(error, failure);

    const DWORD initial_wait = WaitForSingleObject(process, 0);
    if (initial_wait == WAIT_TIMEOUT) {
      if (!TerminateProcess(process, ERROR_CANCELLED)) {
        const DWORD terminate_error = GetLastError();
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
          *worker_process = process;
          process = nullptr;
          if (identity.process_id != 0) *worker_identity = std::move(identity);
          return fail(error, failure + "; " +
              win32_error("cannot terminate failed session worker", terminate_error));
        }
      }
      const DWORD terminated = WaitForSingleObject(process, 5000);
      if (terminated != WAIT_OBJECT_0) {
        const DWORD wait_error = terminated == WAIT_FAILED ? GetLastError() :
                                                              ERROR_TIMEOUT;
        *worker_process = process;
        process = nullptr;
        if (identity.process_id != 0) *worker_identity = std::move(identity);
        return fail(error, failure + "; " +
            win32_error("failed session worker has not exited", wait_error));
      }
    } else if (initial_wait != WAIT_OBJECT_0) {
      const DWORD wait_error = GetLastError();
      *worker_process = process;
      process = nullptr;
      if (identity.process_id != 0) *worker_identity = std::move(identity);
      return fail(error, failure + "; " +
          win32_error("cannot query failed session worker", wait_error));
    }

    CloseHandle(process);
    process = nullptr;
    return fail(error, failure);
  };

  if (process == nullptr || process == INVALID_HANDLE_VALUE)
    return cleanup_failed_start(launch_error.empty()
        ? "session worker launcher returned no process handle"
        : launch_error);

  const DWORD process_id = GetProcessId(process);
  std::string identity_error;
  if (process_id == 0 ||
      !query_portable_helper_process_identity(process_id, &identity,
                                               &identity_error))
    return cleanup_failed_start(identity_error.empty()
        ? "cannot query launched session worker identity"
        : identity_error);
  if (process_id == spec.parent.process_id ||
      identity.session_id != grant.windows_session_id ||
      identity.creation_time == 0 || identity.image_path.empty() ||
      _wcsicmp(identity.image_path.c_str(), spec.executable_path.c_str()) != 0 ||
      identity.user_sid.empty() || identity.logon_sid.empty())
    return cleanup_failed_start(
        "launched session worker identity is outside the authorized scope");
  const DWORD process_state = WaitForSingleObject(process, 0);
  if (process_state != WAIT_TIMEOUT)
    return cleanup_failed_start(process_state == WAIT_OBJECT_0
        ? "session worker exited before pipe authentication"
        : win32_error("cannot query launched session worker", GetLastError()));

  if (!server->Accept(identity, timeout_ms, error))
    return cleanup_failed_start(error != nullptr && !error->empty()
        ? "session worker pipe authentication failed: " + *error
        : "session worker pipe authentication failed");
  if (!send_portable_helper_grant(*server, grant, timeout_ms, error))
    return cleanup_failed_start(error != nullptr && !error->empty()
        ? "cannot send capability to session worker: " + *error
        : "cannot send capability to session worker");
  if (!receive_portable_helper_worker_ready(
          *server, authorization, grant, identity, timeout_ms, error,
          worker_ready_proof))
    return cleanup_failed_start(error != nullptr && !error->empty()
        ? "session worker did not authenticate WorkerReady: " + *error
        : "session worker did not authenticate WorkerReady");

  *worker_identity = std::move(identity);
  *worker_process = process;
  process = nullptr;
  *channel = std::move(server);
  return true;
}

bool start_portable_helper_session_worker_channel(
    const PortableHelperServiceSpec& spec,
    PortableHelperAuthorization& authorization,
    const PortableHelperGrant& grant, uint32_t timeout_ms,
    std::unique_ptr<PortableHelperPipe>* channel, HANDLE* worker_process,
    PortableHelperProcessIdentity* worker_identity, std::string* error,
    PortableHelperSecret* worker_ready_proof) {
  const PortableHelperSessionWorkerLauncher launcher =
      launch_portable_helper_session_worker_process;
  return start_portable_helper_session_worker_channel(
      spec, authorization, grant, launcher, timeout_ms, channel,
      worker_process, worker_identity, error, worker_ready_proof);
}

}  // namespace chast::host
