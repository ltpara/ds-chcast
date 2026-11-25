#define NOMINMAX
#include "portable_helper_recovery.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include "chast/machine_state_security.h"

namespace chast::host {
namespace {

constexpr std::array<uint8_t, 8> kMagic = {'C', 'H', 'A', 'S', 'T', 'H', 'R', '1'};
constexpr uint32_t kVersion = 1;
constexpr size_t kMaximumRecordBytes = 128 * 1024;
constexpr size_t kMaximumStringCharacters = 32767;
constexpr size_t kMaximumRecoveryCandidates = 4096;
constexpr wchar_t kRecordName[] = L"recovery.dat";
constexpr wchar_t kTemporaryName[] = L"recovery.tmp";

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~ScopedHandle() {
    if (valid()) CloseHandle(value_);
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& other) noexcept
      : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      if (valid()) CloseHandle(value_);
      value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  HANDLE get() const { return value_; }
  bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
  HANDLE release() { return std::exchange(value_, INVALID_HANDLE_VALUE); }

 private:
  HANDLE value_;
};

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

std::string win32_error(const char* operation, DWORD code) {
  return std::string(operation) + " failed (Win32 error " +
      std::to_string(code) + ")";
}

void put32(std::vector<uint8_t>* output, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    output->push_back(static_cast<uint8_t>(value >> shift));
}

void put64(std::vector<uint8_t>* output, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    output->push_back(static_cast<uint8_t>(value >> shift));
}

bool put_string(std::vector<uint8_t>* output, const std::wstring& value,
                std::string* error) {
  if (output == nullptr || value.size() > kMaximumStringCharacters ||
      value.find(L'\0') != std::wstring::npos)
    return fail(error, "portable helper recovery string is invalid or too long");
  put32(output, static_cast<uint32_t>(value.size()));
  for (wchar_t character : value) {
    const uint16_t unit = static_cast<uint16_t>(character);
    output->push_back(static_cast<uint8_t>(unit));
    output->push_back(static_cast<uint8_t>(unit >> 8));
  }
  return true;
}

class Reader final {
 public:
  explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  bool take(void* output, size_t length) {
    if (output == nullptr || length > bytes_.size() - offset_) return false;
    std::copy_n(bytes_.begin() + static_cast<ptrdiff_t>(offset_), length,
                static_cast<uint8_t*>(output));
    offset_ += length;
    return true;
  }

  bool get32(uint32_t* value) {
    uint8_t bytes[4]{};
    if (value == nullptr || !take(bytes, sizeof(bytes))) return false;
    *value = static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
  }

  bool get64(uint64_t* value) {
    uint8_t bytes[8]{};
    if (value == nullptr || !take(bytes, sizeof(bytes))) return false;
    *value = 0;
    for (unsigned index = 0; index < sizeof(bytes); ++index)
      *value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    return true;
  }

  bool get_string(std::wstring* value) {
    uint32_t characters = 0;
    if (value == nullptr || !get32(&characters) ||
        characters > kMaximumStringCharacters ||
        static_cast<size_t>(characters) > (bytes_.size() - offset_) / 2)
      return false;
    value->clear();
    value->reserve(characters);
    for (uint32_t index = 0; index < characters; ++index) {
      uint8_t units[2]{};
      if (!take(units, sizeof(units))) return false;
      const uint16_t unit = static_cast<uint16_t>(units[0]) |
          static_cast<uint16_t>(static_cast<uint16_t>(units[1]) << 8);
      if (unit == 0) return false;
      value->push_back(static_cast<wchar_t>(unit));
    }
    return true;
  }

  bool done() const { return offset_ == bytes_.size(); }

 private:
  const std::vector<uint8_t>& bytes_;
  size_t offset_ = 0;
};

bool specs_match(const PortableHelperServiceSpec& left,
                 const PortableHelperServiceSpec& right) {
  return left.instance_id == right.instance_id &&
      left.service_name == right.service_name &&
      _wcsicmp(left.staging_directory.c_str(), right.staging_directory.c_str()) == 0 &&
      _wcsicmp(left.executable_path.c_str(), right.executable_path.c_str()) == 0 &&
      left.parent.process_id == right.parent.process_id &&
      left.parent.session_id == right.parent.session_id &&
      left.parent.creation_time == right.parent.creation_time &&
      _wcsicmp(left.parent.image_path.c_str(), right.parent.image_path.c_str()) == 0 &&
      left.parent.user_sid == right.parent.user_sid &&
      left.parent.logon_sid == right.parent.logon_sid;
}

bool trusted_parent_directories(const PortableHelperServiceSpec& spec,
                                std::string* error) {
  const std::filesystem::path instance(spec.staging_directory);
  const auto helper_root = instance.parent_path();
  const auto chast_root = helper_root.parent_path();
  return chast::machine_state_directory_trusted(spec.staging_directory, error) &&
      chast::machine_state_directory_trusted(helper_root.wstring(), error) &&
      chast::machine_state_directory_trusted(chast_root.wstring(), error);
}

bool write_all(HANDLE file, const std::vector<uint8_t>& bytes,
               std::string* error) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD request = static_cast<DWORD>(std::min<size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + offset, request, &written, nullptr))
      return fail(error, win32_error("WriteFile(helper recovery record)", GetLastError()));
    if (written == 0) return fail(error, "helper recovery write made no progress");
    offset += written;
  }
  if (!FlushFileBuffers(file))
    return fail(error, win32_error("FlushFileBuffers(helper recovery record)", GetLastError()));
  return true;
}

bool remove_file_by_path_if_present(const std::wstring& path,
                                    std::string* error) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    return fail(error, win32_error("GetFileAttributes(helper recovery file)", code));
  }
  if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    return fail(error, "refusing to remove a non-regular recovery file");
  ScopedHandle file(CreateFileW(path.c_str(), DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid())
    return fail(error, win32_error("OpenFile(helper recovery cleanup)", GetLastError()));
  FILE_ATTRIBUTE_TAG_INFO tag{};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo,
                                    &tag, sizeof(tag)) ||
      (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
      !GetFileInformationByHandle(file.get(), &info) || info.nNumberOfLinks != 1)
    return fail(error, "recovery file changed type or link count before cleanup");
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(file.get(), FileDispositionInfo,
                                  &disposition, sizeof(disposition)))
    return fail(error, win32_error("delete helper recovery file", GetLastError()));
  return true;
}

bool instance_id_from_directory_name(const wchar_t* name,
                                     PortableHelperInstanceId* instance_id) {
  if (name == nullptr || instance_id == nullptr || wcslen(name) != 32) return false;
  auto digit = [](wchar_t value, uint8_t* result) {
    if (value >= L'0' && value <= L'9') {
      *result = static_cast<uint8_t>(value - L'0');
      return true;
    }
    if (value >= L'a' && value <= L'f') {
      *result = static_cast<uint8_t>(value - L'a' + 10);
      return true;
    }
    if (value >= L'A' && value <= L'F') {
      *result = static_cast<uint8_t>(value - L'A' + 10);
      return true;
    }
    return false;
  };
  *instance_id = {};
  for (size_t index = 0; index < instance_id->size(); ++index) {
    uint8_t high = 0, low = 0;
    if (!digit(name[index * 2], &high) || !digit(name[index * 2 + 1], &low))
      return false;
    (*instance_id)[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return std::any_of(instance_id->begin(), instance_id->end(),
                     [](uint8_t value) { return value != 0; });
}

void recovery_detail(PortableHelperRecoveryReport* report,
                     const std::wstring& instance, const std::string& detail) {
  if (report == nullptr) return;
  std::string label;
  if (!instance.empty()) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, instance.data(),
        static_cast<int>(instance.size()), nullptr, 0, nullptr, nullptr);
    if (length > 0) {
      label.resize(static_cast<size_t>(length));
      if (WideCharToMultiByte(CP_UTF8, 0, instance.data(),
              static_cast<int>(instance.size()), label.data(), length,
              nullptr, nullptr) != length)
        label.clear();
    }
  }
  report->details.push_back(std::string("instance ") +
      (label.empty() ? std::string("<unknown>") : label) + ": " + detail);
}

bool parent_process_is_alive(const PortableHelperProcessIdentity& expected,
                             bool* alive, std::string* error) {
  if (alive == nullptr) return fail(error, "missing parent liveness output");
  *alive = false;
  HANDLE raw = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, expected.process_id);
  if (raw == nullptr) {
    const DWORD code = GetLastError();
    if (code == ERROR_INVALID_PARAMETER) return true;
    return fail(error, win32_error("OpenProcess(helper parent)", code));
  }
  ScopedHandle process(raw);
  const DWORD wait = WaitForSingleObject(raw, 0);
  if (wait == WAIT_OBJECT_0) return true;
  if (wait != WAIT_TIMEOUT)
    return fail(error, win32_error("WaitForSingleObject(helper parent)", GetLastError()));

  PortableHelperProcessIdentity actual;
  if (!query_portable_helper_process_identity(expected.process_id, &actual, error))
    return false;
  *alive = actual.process_id == expected.process_id &&
      actual.session_id == expected.session_id &&
      actual.creation_time == expected.creation_time &&
      _wcsicmp(actual.image_path.c_str(), expected.image_path.c_str()) == 0 &&
      actual.user_sid == expected.user_sid && actual.logon_sid == expected.logon_sid;
  if (*alive) {
    const DWORD final_wait = WaitForSingleObject(raw, 0);
    if (final_wait == WAIT_OBJECT_0) {
      *alive = false;
    } else if (final_wait != WAIT_TIMEOUT) {
      return fail(error, win32_error("recheck helper parent process", GetLastError()));
    }
  }
  return true;
}

}  // namespace

bool encode_portable_helper_recovery_record(
    const PortableHelperServiceSpec& spec, std::vector<uint8_t>* encoded,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (encoded == nullptr) return fail(error, "missing helper recovery output");
  encoded->clear();
  PortableHelperServiceSpec canonical;
  if (!make_portable_helper_service_spec(spec.instance_id, spec.executable_path,
                                         spec.parent, &canonical, error) ||
      !specs_match(spec, canonical))
    return fail(error, "portable helper recovery spec is not canonical");

  encoded->insert(encoded->end(), kMagic.begin(), kMagic.end());
  put32(encoded, kVersion);
  encoded->insert(encoded->end(), spec.instance_id.begin(), spec.instance_id.end());
  put32(encoded, spec.parent.process_id);
  put32(encoded, spec.parent.session_id);
  put64(encoded, spec.parent.creation_time);
  if (!put_string(encoded, spec.parent.image_path, error) ||
      !put_string(encoded, spec.parent.user_sid, error) ||
      !put_string(encoded, spec.parent.logon_sid, error)) {
    encoded->clear();
    return false;
  }
  if (encoded->size() > kMaximumRecordBytes) {
    encoded->clear();
    return fail(error, "portable helper recovery record exceeds its size limit");
  }
  return true;
}

bool decode_portable_helper_recovery_record(
    const std::vector<uint8_t>& encoded, PortableHelperServiceSpec* spec,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (spec == nullptr) return fail(error, "missing helper recovery spec output");
  *spec = {};
  if (encoded.size() < kMagic.size() + sizeof(uint32_t) +
          kPortableHelperInstanceIdBytes + 2 * sizeof(uint32_t) + sizeof(uint64_t) ||
      encoded.size() > kMaximumRecordBytes)
    return fail(error, "portable helper recovery record length is invalid");
  Reader reader(encoded);
  std::array<uint8_t, kMagic.size()> magic{};
  uint32_t version = 0;
  PortableHelperInstanceId instance{};
  PortableHelperProcessIdentity parent;
  if (!reader.take(magic.data(), magic.size()) || magic != kMagic ||
      !reader.get32(&version) || version != kVersion ||
      !reader.take(instance.data(), instance.size()) ||
      !reader.get32(&parent.process_id) ||
      !reader.get32(&parent.session_id) ||
      !reader.get64(&parent.creation_time) ||
      !reader.get_string(&parent.image_path) ||
      !reader.get_string(&parent.user_sid) ||
      !reader.get_string(&parent.logon_sid) || !reader.done())
    return fail(error, "portable helper recovery record is malformed or unsupported");

  std::wstring staging_directory, executable_path;
  if (!portable_helper_staging_paths(instance, &staging_directory,
                                     &executable_path, error))
    return false;
  return make_portable_helper_service_spec(instance, executable_path, parent,
                                           spec, error);
}

bool portable_helper_recovery_record_path(
    const PortableHelperServiceSpec& spec, std::wstring* path,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (path == nullptr) return fail(error, "missing helper recovery path output");
  path->clear();
  std::vector<uint8_t> ignored;
  if (!encode_portable_helper_recovery_record(spec, &ignored, error)) return false;
  *path = (std::filesystem::path(spec.staging_directory) / kRecordName).wstring();
  return true;
}

bool write_portable_helper_recovery_record(
    const PortableHelperServiceSpec& spec, std::string* error) {
  if (error != nullptr) error->clear();
  if (!trusted_parent_directories(spec, error)) return false;
  std::vector<uint8_t> encoded;
  if (!encode_portable_helper_recovery_record(spec, &encoded, error)) return false;
  std::wstring final_path;
  if (!portable_helper_recovery_record_path(spec, &final_path, error)) return false;
  const auto temporary_path = (std::filesystem::path(spec.staging_directory) /
                               kTemporaryName).wstring();
  ScopedHandle file(CreateFileW(temporary_path.c_str(),
      GENERIC_WRITE | READ_CONTROL | WRITE_DAC | DELETE, FILE_SHARE_READ,
      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file.valid())
    return fail(error, win32_error("CreateFile(helper recovery temporary)", GetLastError()));
  if (!write_all(file.get(), encoded, error) ||
      !chast::harden_machine_state_file(temporary_path, error)) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(file.get(), FileDispositionInfo,
                               &disposition, sizeof(disposition));
    return false;
  }
  file = ScopedHandle();
  if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const DWORD code = GetLastError();
    remove_file_by_path_if_present(temporary_path, nullptr);
    return fail(error, win32_error("MoveFileEx(helper recovery record)", code));
  }
  if (!chast::machine_state_file_trusted(final_path, error)) {
    remove_file_by_path_if_present(final_path, nullptr);
    return false;
  }
  return true;
}

bool read_portable_helper_recovery_record(
    const PortableHelperServiceSpec& expected,
    PortableHelperServiceSpec* actual, std::string* error) {
  if (error != nullptr) error->clear();
  if (actual == nullptr) return fail(error, "missing helper recovery spec output");
  *actual = {};
  if (!read_portable_helper_recovery_record_for_instance(
          expected.instance_id, actual, error))
    return false;
  PortableHelperServiceSpec canonical_expected;
  if (!make_portable_helper_service_spec(expected.instance_id,
          expected.executable_path, expected.parent, &canonical_expected, error) ||
      !specs_match(*actual, canonical_expected)) {
    *actual = {};
    return fail(error, "helper recovery record does not match the expected instance");
  }
  return true;
}

bool read_portable_helper_recovery_record_for_instance(
    const PortableHelperInstanceId& instance_id,
    PortableHelperServiceSpec* actual, std::string* error) {
  if (error != nullptr) error->clear();
  if (actual == nullptr) return fail(error, "missing helper recovery spec output");
  *actual = {};
  std::wstring staging_directory, executable_path;
  if (!portable_helper_staging_paths(instance_id, &staging_directory,
                                     &executable_path, error))
    return false;
  // The parent portion is intentionally absent here; the instance ID alone
  // derives the directory and filename. The file itself supplies the parent
  // identity, which is validated by the decoder before returning.
  const auto path = (std::filesystem::path(staging_directory) / kRecordName).wstring();
  const std::filesystem::path instance(staging_directory);
  if (!chast::machine_state_directory_trusted(staging_directory, error) ||
      !chast::machine_state_directory_trusted(instance.parent_path().wstring(), error) ||
      !chast::machine_state_directory_trusted(
          instance.parent_path().parent_path().wstring(), error))
    return false;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return fail(error, win32_error("GetFileAttributes(helper recovery record)", GetLastError()));
  if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    return fail(error, "helper recovery record is not a regular file");
  if (!chast::machine_state_file_trusted(path, error)) return false;

  ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!file.valid())
    return fail(error, win32_error("OpenFile(helper recovery record)", GetLastError()));
  FILE_ATTRIBUTE_TAG_INFO tag{};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo,
                                    &tag, sizeof(tag)) ||
      (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
      !GetFileInformationByHandle(file.get(), &info) || info.nNumberOfLinks != 1)
    return fail(error, "helper recovery record has an unsafe file type or link count");
  const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                        info.nFileSizeLow;
  if (size == 0 || size > kMaximumRecordBytes)
    return fail(error, "helper recovery record size is invalid");
  std::vector<uint8_t> encoded(static_cast<size_t>(size));
  size_t offset = 0;
  while (offset < encoded.size()) {
    DWORD read = 0;
    const DWORD request = static_cast<DWORD>(std::min<size_t>(
        encoded.size() - offset, std::numeric_limits<DWORD>::max()));
    if (!ReadFile(file.get(), encoded.data() + offset, request, &read, nullptr))
      return fail(error, win32_error("ReadFile(helper recovery record)", GetLastError()));
    if (read == 0) return fail(error, "helper recovery record was truncated");
    offset += read;
  }
  if (!decode_portable_helper_recovery_record(encoded, actual, error)) return false;
  if (actual->instance_id != instance_id ||
      _wcsicmp(actual->staging_directory.c_str(), staging_directory.c_str()) != 0 ||
      _wcsicmp(actual->executable_path.c_str(), executable_path.c_str()) != 0) {
    *actual = {};
    return fail(error, "helper recovery record path does not match its instance ID");
  }
  return true;
}

bool remove_portable_helper_recovery_record(
    const PortableHelperServiceSpec& expected, std::string* error) {
  PortableHelperServiceSpec actual;
  if (!read_portable_helper_recovery_record(expected, &actual, error)) return false;
  std::wstring path;
  if (!portable_helper_recovery_record_path(expected, &path, error)) return false;
  return remove_file_by_path_if_present(path, error);
}

bool recover_portable_helper_records(
    const std::vector<PortableHelperServiceSpec>& records,
    const PortableHelperParentLivenessCheck& parent_is_alive,
    PortableHelperServiceBackend& backend,
    PortableHelperRecoveryReport* report, std::string* error,
    const PortableHelperRecoveryContinueCheck& should_continue) {
  if (error != nullptr) error->clear();
  if (report == nullptr) return fail(error, "missing portable helper recovery report");
  *report = {};
  if (!parent_is_alive)
    return fail(error, "missing portable helper parent liveness checker");

  const auto stop_requested = [&] {
    if (!should_continue || should_continue()) return false;
    report->interrupted = true;
    return true;
  };
  for (const auto& spec : records) {
    if (stop_requested()) break;
    ++report->inspected;
    std::vector<uint8_t> canonical;
    std::string detail;
    if (!encode_portable_helper_recovery_record(spec, &canonical, &detail)) {
      ++report->invalid;
      recovery_detail(report, spec.service_name, detail);
      continue;
    }

    bool alive = false;
    detail.clear();
    if (!parent_is_alive(spec.parent, &alive, &detail)) {
      ++report->pending;
      recovery_detail(report, spec.service_name,
          detail.empty() ? "cannot establish the parent process identity" : detail);
      continue;
    }
    if (alive) {
      ++report->active;
      continue;
    }

    if (!backend.Stop(spec, &detail)) {
      ++report->pending;
      recovery_detail(report, spec.service_name,
          detail.empty() ? "helper service could not be stopped" : detail);
      continue;
    }
    if (stop_requested()) break;
    detail.clear();
    if (!backend.DeleteIfOwned(spec, &detail)) {
      ++report->pending;
      recovery_detail(report, spec.service_name,
          detail.empty() ? "helper service could not be removed" : detail);
      continue;
    }
    if (stop_requested()) break;
    detail.clear();
    if (!backend.RemoveStagedImage(spec, &detail)) {
      ++report->pending;
      recovery_detail(report, spec.service_name,
          detail.empty() ? "helper staged image could not be removed" : detail);
      continue;
    }
    ++report->cleaned;
  }
  return true;
}

bool scan_and_recover_portable_helper_instances(
    PortableHelperServiceBackend& backend,
    PortableHelperRecoveryReport* report, std::string* error,
    const PortableHelperRecoveryContinueCheck& should_continue) {
  if (error != nullptr) error->clear();
  if (report == nullptr) return fail(error, "missing portable helper recovery report");
  *report = {};
  if (!chast::process_can_harden_machine_state())
    return fail(error, "portable helper recovery requires an elevated administrator or LocalSystem token");

  PWSTR raw_program_data = nullptr;
  const HRESULT known_folder = SHGetKnownFolderPath(
      FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw_program_data);
  if (FAILED(known_folder) || raw_program_data == nullptr)
    return fail(error, "cannot resolve ProgramData for helper recovery");
  const std::filesystem::path program_data(raw_program_data);
  CoTaskMemFree(raw_program_data);
  const auto chast_root = program_data / L"Chast";
  const auto helper_root = chast_root / L"PortableHelper";

  const DWORD chast_attributes = GetFileAttributesW(chast_root.c_str());
  if (chast_attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    return fail(error, win32_error("GetFileAttributes(Chast state root)", code));
  }
  if ((chast_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
      FILE_ATTRIBUTE_DIRECTORY)
    return fail(error, "Chast machine state root is not a plain directory");
  if (!chast::machine_state_directory_trusted(chast_root.wstring(), error)) return false;

  const DWORD helper_attributes = GetFileAttributesW(helper_root.c_str());
  if (helper_attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    return fail(error, win32_error("GetFileAttributes(helper recovery root)", code));
  }
  if ((helper_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
      FILE_ATTRIBUTE_DIRECTORY)
    return fail(error, "portable helper recovery root is not a plain directory");
  if (!chast::machine_state_directory_trusted(helper_root.wstring(), error)) return false;

  const std::wstring pattern = (helper_root / L"*").wstring();
  WIN32_FIND_DATAW entry{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) return true;
    return fail(error, win32_error("FindFirstFile(helper recovery root)", code));
  }

  std::vector<PortableHelperServiceSpec> records;
  PortableHelperRecoveryReport scan_report;
  bool enumeration_ok = true;
  bool interrupted = false;
  DWORD enumeration_error = ERROR_SUCCESS;
  do {
    if (should_continue && !should_continue()) {
      interrupted = true;
      break;
    }
    if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0)
      continue;
    if (records.size() + scan_report.invalid >= kMaximumRecoveryCandidates) {
      enumeration_ok = false;
      enumeration_error = ERROR_TOO_MANY_NAMES;
      break;
    }
    PortableHelperInstanceId instance_id{};
    if (!instance_id_from_directory_name(entry.cFileName, &instance_id)) {
      ++scan_report.invalid;
      recovery_detail(&scan_report, entry.cFileName,
                     "unexpected entry in the helper recovery root");
      continue;
    }
    if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
        FILE_ATTRIBUTE_DIRECTORY) {
      ++scan_report.invalid;
      recovery_detail(&scan_report, entry.cFileName,
                     "helper instance path is not a plain directory");
      continue;
    }

    PortableHelperServiceSpec spec;
    std::string detail;
    if (!read_portable_helper_recovery_record_for_instance(
            instance_id, &spec, &detail)) {
      ++scan_report.invalid;
      recovery_detail(&scan_report, entry.cFileName,
          detail.empty() ? "helper recovery record is invalid" : detail);
      continue;
    }
    const auto expected_directory = (helper_root / entry.cFileName).wstring();
    if (_wcsicmp(spec.staging_directory.c_str(), expected_directory.c_str()) != 0) {
      ++scan_report.invalid;
      recovery_detail(&scan_report, entry.cFileName,
                     "record directory does not match the enumerated instance name");
      continue;
    }
    records.push_back(std::move(spec));
  } while (FindNextFileW(find, &entry));
  if (enumeration_ok && !interrupted) {
    enumeration_error = GetLastError();
    enumeration_ok = enumeration_error == ERROR_NO_MORE_FILES;
  }
  FindClose(find);
  if (!enumeration_ok)
    return fail(error, win32_error("enumerate helper recovery root", enumeration_error));

  if (interrupted) {
    report->invalid = scan_report.invalid;
    report->details = std::move(scan_report.details);
    report->interrupted = true;
    return true;
  }

  PortableHelperRecoveryReport recovery_report;
  if (!recover_portable_helper_records(records, parent_process_is_alive,
                                       backend, &recovery_report, error,
                                       should_continue))
    return false;
  report->inspected = recovery_report.inspected;
  report->active = recovery_report.active;
  report->cleaned = recovery_report.cleaned;
  report->pending = recovery_report.pending;
  report->invalid = scan_report.invalid + recovery_report.invalid;
  report->details = std::move(scan_report.details);
  report->details.insert(report->details.end(), recovery_report.details.begin(),
                         recovery_report.details.end());
  report->interrupted = recovery_report.interrupted;
  return true;
}

}  // namespace chast::host
