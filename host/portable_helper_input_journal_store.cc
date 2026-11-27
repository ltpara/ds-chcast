#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include "portable_helper_input_journal_store.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "chast/machine_state_security.h"
#include "portable_helper_lifecycle.h"

namespace chast::host {
namespace {

constexpr size_t kMaximumRecordBytes = 64 * 1024;
constexpr DWORD kLockWaitMilliseconds = 2000;

class ScopedHandle final {
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
  bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
  HANDLE get() const { return value_; }
  HANDLE release() { return std::exchange(value_, INVALID_HANDLE_VALUE); }

 private:
  HANDLE value_;
};

class ScopedFileLock final {
 public:
  ScopedFileLock(HANDLE file, OVERLAPPED overlapped)
      : file_(file), overlapped_(overlapped), locked_(true) {}
  ~ScopedFileLock() {
    if (locked_) UnlockFileEx(file_, 0, 1, 0, &overlapped_);
  }
  ScopedFileLock(const ScopedFileLock&) = delete;
  ScopedFileLock& operator=(const ScopedFileLock&) = delete;

 private:
  HANDLE file_;
  OVERLAPPED overlapped_{};
  bool locked_ = false;
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

bool nonzero(const PortableHelperInstanceId& id) {
  return std::any_of(id.begin(), id.end(),
                     [](uint8_t value) { return value != 0; });
}

bool valid_scope(const PortableHelperInputJournalScope& scope) {
  return nonzero(scope.instance_id) && nonzero(scope.connection_id) &&
      scope.windows_session_id != 0 && scope.generation != 0;
}

std::wstring hex_bytes(const PortableHelperInstanceId& id) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result;
  result.reserve(id.size() * 2);
  for (uint8_t byte : id) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

std::wstring scope_stem(const PortableHelperInputJournalScope& scope) {
  wchar_t suffix[48]{};
  swprintf_s(suffix, L"-s%08x-g%016llx",
      static_cast<unsigned>(scope.windows_session_id),
      static_cast<unsigned long long>(scope.generation));
  return L"input-" + hex_bytes(scope.instance_id) + L"-" +
      hex_bytes(scope.connection_id) + suffix;
}

int lower_hex_value(wchar_t character) {
  if (character >= L'0' && character <= L'9') return character - L'0';
  if (character >= L'a' && character <= L'f') return character - L'a' + 10;
  return -1;
}

template <size_t Size>
bool parse_lower_hex(std::wstring_view text, std::array<uint8_t, Size>* value) {
  if (value == nullptr || text.size() != Size * 2) return false;
  value->fill(0);
  for (size_t index = 0; index < Size; ++index) {
    const int high = lower_hex_value(text[index * 2]);
    const int low = lower_hex_value(text[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    (*value)[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool parse_lower_hex_number(std::wstring_view text, uint64_t* value) {
  if (value == nullptr || text.empty()) return false;
  uint64_t parsed = 0;
  for (wchar_t character : text) {
    const int digit = lower_hex_value(character);
    if (digit < 0 || parsed >
          (std::numeric_limits<uint64_t>::max() -
           static_cast<uint64_t>(digit)) / 16)
      return false;
    parsed = parsed * 16 + static_cast<uint64_t>(digit);
  }
  *value = parsed;
  return true;
}

struct JournalArtifacts final {
  PortableHelperInputJournalScope scope;
  bool record = false;
  bool temporary = false;
  bool lock = false;
};

bool parse_journal_artifact_name(
    const std::wstring& name, const PortableHelperInstanceId& instance_id,
    std::wstring* stem, std::wstring* extension,
    PortableHelperInputJournalScope* scope) {
  if (stem == nullptr || extension == nullptr || scope == nullptr) return false;
  const std::wstring prefix = L"input-" + hex_bytes(instance_id) + L"-";
  const size_t dot = name.rfind(L'.');
  if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
      dot == std::wstring::npos || dot + 1 >= name.size())
    return false;
  const std::wstring_view body(name.data() + prefix.size(), dot - prefix.size());
  constexpr size_t kConnectionHex = 32;
  constexpr size_t kSessionHex = 8;
  constexpr size_t kGenerationHex = 16;
  if (body.size() != kConnectionHex + 2 + kSessionHex + 2 + kGenerationHex ||
      body.substr(kConnectionHex, 2) != L"-s" ||
      body.substr(kConnectionHex + 2 + kSessionHex, 2) != L"-g")
    return false;

  PortableHelperInputJournalScope parsed;
  parsed.instance_id = instance_id;
  uint64_t session = 0;
  if (!parse_lower_hex(body.substr(0, kConnectionHex), &parsed.connection_id) ||
      !nonzero(parsed.connection_id) ||
      !parse_lower_hex_number(body.substr(kConnectionHex + 2, kSessionHex),
                              &session) ||
      session == 0 || session > std::numeric_limits<uint32_t>::max() ||
      !parse_lower_hex_number(
          body.substr(kConnectionHex + 2 + kSessionHex + 2,
                      kGenerationHex),
          &parsed.generation) || parsed.generation == 0)
    return false;
  parsed.windows_session_id = static_cast<uint32_t>(session);

  const std::wstring parsed_extension = name.substr(dot + 1);
  if (parsed_extension != L"dat" && parsed_extension != L"tmp" &&
      parsed_extension != L"lck")
    return false;
  *stem = name.substr(0, dot);
  *extension = parsed_extension;
  *scope = parsed;
  return true;
}

bool state_equal(const PortableHelperInputJournalState& left,
                 const PortableHelperInputJournalState& right) {
  if (left.mouse_buttons != right.mouse_buttons ||
      left.pen_down != right.pen_down || left.keys.size() != right.keys.size() ||
      left.touches.size() != right.touches.size())
    return false;
  for (size_t i = 0; i < left.keys.size(); ++i) {
    const auto& a = left.keys[i];
    const auto& b = right.keys[i];
    if (a.virtual_key != b.virtual_key || a.scan_code != b.scan_code ||
        a.flags != b.flags || a.time != b.time || a.extra_info != b.extra_info)
      return false;
  }
  for (size_t i = 0; i < left.touches.size(); ++i) {
    const auto& a = left.touches[i];
    const auto& b = right.touches[i];
    if (a.pointer_id != b.pointer_id || a.x != b.x || a.y != b.y ||
        a.pressure != b.pressure)
      return false;
  }
  if (left.pen_down) {
    const auto& a = left.pen;
    const auto& b = right.pen;
    return a.x == b.x && a.y == b.y && a.pressure == b.pressure &&
        a.flags == b.flags && a.tilt_x == b.tilt_x && a.tilt_y == b.tilt_y;
  }
  return true;
}

bool state_empty(const PortableHelperInputJournalState& state) {
  return state.keys.empty() && state.mouse_buttons == 0 &&
      state.touches.empty() && !state.pen_down;
}

bool state_is_superset(const PortableHelperInputJournalState& possible,
                       const PortableHelperInputJournalState& committed) {
  if ((possible.mouse_buttons & committed.mouse_buttons) !=
      committed.mouse_buttons)
    return false;
  for (const auto& key : committed.keys) {
    if (std::none_of(possible.keys.begin(), possible.keys.end(),
        [&key](const auto& item) {
          return item.virtual_key == key.virtual_key &&
              item.scan_code == key.scan_code && item.flags == key.flags &&
              item.extra_info == key.extra_info;
        }))
      return false;
  }
  for (const auto& touch : committed.touches) {
    if (std::none_of(possible.touches.begin(), possible.touches.end(),
        [&touch](const auto& item) { return item.pointer_id == touch.pointer_id; }))
      return false;
  }
  return !committed.pen_down || possible.pen_down;
}

bool image_equal(const PortableHelperInputJournalImage& left,
                 const PortableHelperInputJournalImage& right) {
  return left.scope.instance_id == right.scope.instance_id &&
      left.scope.connection_id == right.scope.connection_id &&
      left.scope.windows_session_id == right.scope.windows_session_id &&
      left.scope.generation == right.scope.generation &&
      left.input_desktop_name == right.input_desktop_name &&
      left.last_sequence == right.last_sequence &&
      left.pending == right.pending &&
      left.released == right.released &&
      left.pending_sequence == right.pending_sequence &&
      left.pending_api == right.pending_api &&
      state_equal(left.committed, right.committed) &&
      state_equal(left.possible, right.possible);
}

bool legal_transition(const PortableHelperInputJournalImage& old_image,
                      const PortableHelperInputJournalImage& new_image) {
  if (old_image.released) return false;
  if (new_image.released) {
    return !new_image.pending &&
        new_image.last_sequence == old_image.last_sequence &&
        new_image.input_desktop_name.empty() &&
        state_empty(new_image.committed) && state_empty(new_image.possible);
  }
  if (!old_image.pending && new_image.pending) {
    return new_image.last_sequence == old_image.last_sequence &&
        new_image.pending_sequence > old_image.last_sequence &&
        state_equal(new_image.committed, old_image.committed) &&
        !new_image.input_desktop_name.empty() &&
        (state_empty(old_image.committed) ||
         old_image.input_desktop_name == new_image.input_desktop_name) &&
        state_is_superset(new_image.possible, old_image.committed);
  }
  if (old_image.pending && !new_image.pending) {
    const bool result_commit =
        new_image.last_sequence == old_image.pending_sequence &&
        state_is_superset(old_image.possible, new_image.committed) &&
        (state_empty(new_image.committed)
             ? new_image.input_desktop_name.empty()
             : new_image.input_desktop_name == old_image.input_desktop_name);
    const bool confirmed_release =
        new_image.last_sequence == old_image.last_sequence &&
        new_image.input_desktop_name.empty() &&
        state_empty(new_image.committed) && state_empty(new_image.possible);
    return result_commit || confirmed_release;
  }
  return false;
}

bool file_is_regular_single_link(HANDLE file, uint64_t* size,
                                 std::string* error) {
  if (size != nullptr) *size = 0;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo,
                                    &tag, sizeof(tag)) ||
      !GetFileInformationByHandle(file, &info))
    return fail(error, win32_error("inspect input journal file", GetLastError()));
  if ((tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                             FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
      info.nNumberOfLinks != 1)
    return fail(error, "input journal path is not a regular single-link file");
  if (size != nullptr) {
    *size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
        info.nFileSizeLow;
  }
  return true;
}

bool path_missing_error(DWORD code) {
  return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

bool read_image_file(const std::wstring& path,
                     const PortableHelperInputJournalScope& expected_scope,
                     PortableHelperInputJournalImage* image, bool* found,
                     std::string* error) {
  if (image == nullptr || found == nullptr)
    return fail(error, "missing input journal load output");
  *image = {};
  *found = false;
  ScopedHandle file(CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES |
      READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!file.valid()) {
    const DWORD code = GetLastError();
    if (path_missing_error(code)) return true;
    return fail(error, win32_error("open input journal record", code));
  }
  uint64_t size = 0;
  if (!file_is_regular_single_link(file.get(), &size, error)) return false;
  if (size == 0 || size > kMaximumRecordBytes)
    return fail(error, "input journal record type or size is invalid");
  if (!chast::machine_state_file_trusted(path, error)) return false;
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD read = 0;
    const DWORD request = static_cast<DWORD>(std::min<size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    if (!ReadFile(file.get(), bytes.data() + offset, request, &read, nullptr))
      return fail(error, win32_error("read input journal record", GetLastError()));
    if (read == 0) return fail(error, "input journal record was truncated");
    offset += read;
  }
  PortableHelperInputJournalImage decoded;
  if (!decode_portable_helper_input_journal(bytes, &decoded, error)) return false;
  if (decoded.scope.instance_id != expected_scope.instance_id ||
      decoded.scope.connection_id != expected_scope.connection_id ||
      decoded.scope.windows_session_id != expected_scope.windows_session_id ||
      decoded.scope.generation != expected_scope.generation)
    return fail(error, "input journal record belongs to a different scope");
  *image = std::move(decoded);
  *found = true;
  return true;
}

bool delete_regular_file(const std::wstring& path, std::string* error) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (path_missing_error(code)) return true;
    return fail(error, win32_error("inspect stale journal temporary", code));
  }
  if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    return fail(error, "refusing to delete a journal path that is not a regular file");
  if (!chast::machine_state_file_trusted(path, error)) return false;
  ScopedHandle file(CreateFileW(path.c_str(), DELETE | READ_CONTROL |
      FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid()) {
    const DWORD code = GetLastError();
    if (path_missing_error(code)) return true;
    return fail(error, win32_error("open stale journal temporary", code));
  }
  if (!file_is_regular_single_link(file.get(), nullptr, error)) return false;
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(file.get(), FileDispositionInfo,
                                  &disposition, sizeof(disposition)))
    return fail(error, win32_error("delete stale journal temporary", GetLastError()));
  return true;
}

bool write_all(HANDLE file, const std::vector<uint8_t>& bytes,
               std::string* error) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD request = static_cast<DWORD>(std::min<size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + offset, request, &written, nullptr))
      return fail(error, win32_error("write input journal temporary", GetLastError()));
    if (written == 0) return fail(error, "input journal write made no progress");
    offset += written;
  }
  if (!FlushFileBuffers(file))
    return fail(error, win32_error("flush input journal temporary", GetLastError()));
  return true;
}

bool secure_lock_path(const std::wstring& path, std::string* error) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  constexpr wchar_t kLockDacl[] = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kLockDacl, SDDL_REVISION_1, &descriptor, nullptr))
    return fail(error, win32_error("build input journal lock DACL", GetLastError()));
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  ScopedHandle created(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE |
      READ_CONTROL | WRITE_DAC | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &attributes, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  const DWORD create_error = created.valid() ? ERROR_SUCCESS : GetLastError();
  LocalFree(descriptor);
  if (created.valid()) {
    created = ScopedHandle();
  } else if (create_error != ERROR_FILE_EXISTS &&
             create_error != ERROR_ALREADY_EXISTS) {
    return fail(error, win32_error("create input journal lock", create_error));
  }
  // Open with symmetric read/write sharing so another live store's persistent
  // lock handle does not prevent this validation from querying its DACL.
  ScopedHandle verify(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (verify.valid() &&
      chast::machine_state_file_handle_trusted(verify.get(), nullptr))
    return true;
  verify = ScopedHandle();
  std::string harden_error;
  if (!chast::harden_machine_state_file(path, &harden_error)) {
    ScopedHandle retry(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (retry.valid() &&
        chast::machine_state_file_handle_trusted(retry.get(), nullptr))
      return true;
    return fail(error, harden_error);
  }
  return chast::machine_state_file_trusted(path, error);
}

}  // namespace

PortableHelperInputJournalFileStore::PortableHelperInputJournalFileStore(
    PortableHelperInputJournalScope scope, std::wstring directory,
    std::wstring stem, void* lock_file)
    : scope_(std::move(scope)), directory_(std::move(directory)),
      stem_(std::move(stem)),
      record_path_((std::filesystem::path(directory_) /
                    (stem_ + L".dat")).wstring()),
      temporary_path_((std::filesystem::path(directory_) /
                       (stem_ + L".tmp")).wstring()),
      lock_file_(lock_file) {}

PortableHelperInputJournalFileStore::~PortableHelperInputJournalFileStore() {
  HANDLE lock = static_cast<HANDLE>(lock_file_);
  if (lock != nullptr && lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
}

std::unique_ptr<PortableHelperInputJournalFileStore>
PortableHelperInputJournalFileStore::OpenForInstance(
    const PortableHelperInputJournalScope& scope, std::string* error) {
  if (error != nullptr) error->clear();
  if (!valid_scope(scope)) {
    fail(error, "input journal production scope is invalid");
    return nullptr;
  }
  std::wstring directory, executable;
  if (!portable_helper_staging_paths(scope.instance_id, &directory,
                                     &executable, error))
    return nullptr;
  const std::filesystem::path instance(directory);
  if (!chast::machine_state_directory_trusted(directory, error) ||
      !chast::machine_state_directory_trusted(
          instance.parent_path().wstring(), error) ||
      !chast::machine_state_directory_trusted(
          instance.parent_path().parent_path().wstring(), error))
    return nullptr;
  return OpenAtDirectory(scope, directory, error);
}

bool PortableHelperInputJournalFileStore::RemoveReleasedForInstance(
    const PortableHelperInstanceId& instance_id, std::string* error) {
  if (error != nullptr) error->clear();
  if (!nonzero(instance_id))
    return fail(error, "input journal cleanup requires a nonzero instance ID");
  std::wstring directory, executable;
  if (!portable_helper_staging_paths(instance_id, &directory, &executable,
                                     error))
    return false;
  const std::filesystem::path instance(directory);
  if (!chast::machine_state_directory_trusted(directory, error) ||
      !chast::machine_state_directory_trusted(
          instance.parent_path().wstring(), error) ||
      !chast::machine_state_directory_trusted(
          instance.parent_path().parent_path().wstring(), error))
    return false;
  return RemoveReleasedAtDirectory(instance_id, directory, error);
}

bool PortableHelperInputJournalFileStore::RemoveReleasedAtDirectory(
    const PortableHelperInstanceId& instance_id,
    const std::wstring& directory, std::string* error) {
  if (error != nullptr) error->clear();
  if (!nonzero(instance_id) ||
      !chast::machine_state_directory_trusted(directory, error))
    return fail(error, "input journal cleanup directory or instance is invalid");

  const std::wstring pattern = (std::filesystem::path(directory) /
      (L"input-" + hex_bytes(instance_id) + L"-*")).wstring();
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    return fail(error, win32_error("enumerate input journal artifacts", code));
  }

  std::map<std::wstring, JournalArtifacts> artifacts;
  bool enumerated = true;
  do {
    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
      continue;
    if ((data.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      enumerated = fail(error,
          "input journal cleanup found a directory or reparse artifact");
      break;
    }
    std::wstring stem, extension;
    PortableHelperInputJournalScope scope;
    if (!parse_journal_artifact_name(data.cFileName, instance_id, &stem,
                                     &extension, &scope)) {
      enumerated = fail(error,
          "input journal cleanup found an unknown instance artifact");
      break;
    }
    JournalArtifacts& entry = artifacts[stem];
    if ((entry.record && extension == L"dat") ||
        (entry.temporary && extension == L"tmp") ||
        (entry.lock && extension == L"lck")) {
      enumerated = fail(error, "input journal cleanup found a duplicate artifact");
      break;
    }
    if (entry.scope.generation != 0 &&
        (entry.scope.connection_id != scope.connection_id ||
         entry.scope.windows_session_id != scope.windows_session_id ||
         entry.scope.generation != scope.generation)) {
      enumerated = fail(error, "input journal artifact scope is inconsistent");
      break;
    }
    entry.scope = scope;
    entry.record = entry.record || extension == L"dat";
    entry.temporary = entry.temporary || extension == L"tmp";
    entry.lock = entry.lock || extension == L"lck";
  } while (FindNextFileW(find, &data));
  const DWORD enumeration_error = GetLastError();
  FindClose(find);
  if (!enumerated) return false;
  if (enumeration_error != ERROR_NO_MORE_FILES)
    return fail(error, win32_error("finish enumerating input journal artifacts",
                                   enumeration_error));

  // First prove that every installed generation reached a durable terminal
  // tombstone. Do not begin deleting any generation while one may still hold
  // input, so a pending or malformed record keeps the complete instance intact.
  for (const auto& [stem, entry] : artifacts) {
    if (!entry.record) continue;
    if (!entry.lock)
      return fail(error, "input journal record has no protected lock artifact");
    auto store = OpenAtDirectory(entry.scope, directory, error);
    PortableHelperInputJournalImage image;
    bool found = false;
    if (!store || !store->Load(&image, &found, error) || !found ||
        !image.released || image.pending || !state_empty(image.committed) ||
        !state_empty(image.possible) || !image.input_desktop_name.empty())
      return fail(error,
          "input journal cleanup requires every installed record to be released");
  }

  // The service is already gone at this call site. Open handles still prevent
  // deletion of the lock file, which makes a leftover live store fail closed.
  for (const auto& [stem, entry] : artifacts) {
    const auto path = [&](const wchar_t* extension) {
      return (std::filesystem::path(directory) /
          (stem + L"." + extension)).wstring();
    };
    if (entry.record && !delete_regular_file(path(L"dat"), error)) return false;
    if (entry.temporary && !delete_regular_file(path(L"tmp"), error)) return false;
    if (entry.lock && !delete_regular_file(path(L"lck"), error)) return false;
  }
  return true;
}

std::unique_ptr<PortableHelperInputJournalFileStore>
PortableHelperInputJournalFileStore::OpenAtDirectory(
    const PortableHelperInputJournalScope& scope,
    const std::wstring& directory, std::string* error) {
  if (error != nullptr) error->clear();
  PortableHelperInputJournalImage empty;
  empty.scope = scope;
  std::vector<uint8_t> validation;
  if (!valid_scope(scope) ||
      !encode_portable_helper_input_journal(empty, &validation, error))
    return nullptr;
  if (!chast::machine_state_directory_trusted(directory, error)) return nullptr;

  const std::wstring stem = scope_stem(scope);
  const auto lock_path = (std::filesystem::path(directory) /
                          (stem + L".lck")).wstring();
  if (!secure_lock_path(lock_path, error)) return nullptr;
  ScopedHandle lock(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE |
      READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!lock.valid()) {
    fail(error, win32_error("open input journal lock", GetLastError()));
    return nullptr;
  }
  uint64_t lock_size = 0;
  if (!file_is_regular_single_link(lock.get(), &lock_size, error) ||
      lock_size != 0 ||
      !chast::machine_state_file_handle_trusted(lock.get(), error))
    return nullptr;
  HANDLE lock_handle = lock.release();
  return std::unique_ptr<PortableHelperInputJournalFileStore>(
      new PortableHelperInputJournalFileStore(scope, directory, stem,
                                               lock_handle));
}

bool PortableHelperInputJournalFileStore::Load(
    PortableHelperInputJournalImage* image, bool* found,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (image == nullptr || found == nullptr)
    return fail(error, "missing input journal load output");
  *image = {};
  *found = false;
  std::lock_guard<std::mutex> local_lock(mutex_);
  OVERLAPPED overlapped{};
  const ULONGLONG deadline = GetTickCount64() + kLockWaitMilliseconds;
  for (;;) {
    if (LockFileEx(static_cast<HANDLE>(lock_file_),
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, 1, 0, &overlapped))
      break;
    const DWORD code = GetLastError();
    if (code != ERROR_LOCK_VIOLATION || GetTickCount64() >= deadline)
      return fail(error, win32_error("lock input journal", code));
    Sleep(5);
  }
  ScopedFileLock file_lock(static_cast<HANDLE>(lock_file_), overlapped);
  return LoadLocked(image, found, error);
}

bool PortableHelperInputJournalFileStore::LoadLocked(
    PortableHelperInputJournalImage* image, bool* found,
    std::string* error) {
  if (!delete_regular_file(temporary_path_, error)) return false;
  if (!read_image_file(record_path_, scope_, image, found, error)) return false;
  if (!*found && record_loaded_)
    return fail(error, "loaded input journal disappeared during recovery");
  if (*found) record_loaded_ = true;
  return true;
}

bool PortableHelperInputJournalFileStore::ResumeLoadedRecord(
    const PortableHelperInputJournal& recovered_journal, std::string* error) {
  if (error != nullptr) error->clear();
  std::lock_guard<std::mutex> local_lock(mutex_);
  OVERLAPPED overlapped{};
  const ULONGLONG deadline = GetTickCount64() + kLockWaitMilliseconds;
  for (;;) {
    if (LockFileEx(static_cast<HANDLE>(lock_file_),
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, 1, 0, &overlapped))
      break;
    const DWORD code = GetLastError();
    if (code != ERROR_LOCK_VIOLATION || GetTickCount64() >= deadline)
      return fail(error, win32_error("lock input journal", code));
    Sleep(5);
  }
  ScopedFileLock file_lock(static_cast<HANDLE>(lock_file_), overlapped);
  PortableHelperInputJournalImage current;
  bool found = false;
  const PortableHelperInputJournalImage image = recovered_journal.image();
  if (!record_loaded_ || !read_image_file(record_path_, scope_, &current,
                                           &found, error) ||
      !found || !recovered_journal.recovery_only() ||
      !image_equal(current, image))
    return fail(error, "recovery image does not match the loaded input journal");
  updates_enabled_ = true;
  return true;
}

bool PortableHelperInputJournalFileStore::Persist(
    const PortableHelperInputJournalImage& image, std::string* error) {
  if (error != nullptr) error->clear();
  std::vector<uint8_t> encoded;
  if (image.scope.instance_id != scope_.instance_id ||
      image.scope.connection_id != scope_.connection_id ||
      image.scope.windows_session_id != scope_.windows_session_id ||
      image.scope.generation != scope_.generation ||
      !encode_portable_helper_input_journal(image, &encoded, error))
    return fail(error, "input journal file store rejected a mismatched or invalid image");
  std::lock_guard<std::mutex> local_lock(mutex_);
  OVERLAPPED overlapped{};
  const ULONGLONG deadline = GetTickCount64() + kLockWaitMilliseconds;
  for (;;) {
    if (LockFileEx(static_cast<HANDLE>(lock_file_),
            LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
            0, 1, 0, &overlapped))
      break;
    const DWORD code = GetLastError();
    if (code != ERROR_LOCK_VIOLATION || GetTickCount64() >= deadline)
      return fail(error, win32_error("lock input journal", code));
    Sleep(5);
  }
  ScopedFileLock file_lock(static_cast<HANDLE>(lock_file_), overlapped);
  return PersistLocked(image, error);
}

bool PortableHelperInputJournalFileStore::PersistLocked(
    const PortableHelperInputJournalImage& image, std::string* error) {
  if (!delete_regular_file(temporary_path_, error)) return false;
  PortableHelperInputJournalImage current;
  bool found = false;
  if (!read_image_file(record_path_, scope_, &current, &found, error)) return false;
  if (found && (!record_loaded_ || !updates_enabled_))
    return fail(error, "refusing to overwrite an input journal not loaded by this store");
  if (!found && record_loaded_)
    return fail(error, "loaded input journal disappeared before update");

  if (!found) {
    if (image.pending || image.released || image.last_sequence != 0 ||
        !state_empty(image.committed) || !state_empty(image.possible))
      return fail(error, "a new input journal must begin with an empty initial image");
  } else if (!legal_transition(current, image)) {
    return fail(error, "input journal file update is stale, replayed, or out of order");
  }

  std::vector<uint8_t> encoded;
  if (!encode_portable_helper_input_journal(image, &encoded, error)) return false;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  constexpr wchar_t kRecordDacl[] = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kRecordDacl, SDDL_REVISION_1, &descriptor, nullptr))
    return fail(error, win32_error("build input journal record DACL", GetLastError()));
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  ScopedHandle temporary(CreateFileW(temporary_path_.c_str(),
      GENERIC_WRITE | READ_CONTROL | WRITE_DAC | WRITE_OWNER,
      FILE_SHARE_READ, &attributes, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_WRITE_THROUGH, nullptr));
  const DWORD create_error = temporary.valid() ? ERROR_SUCCESS : GetLastError();
  LocalFree(descriptor);
  if (!temporary.valid())
    return fail(error, win32_error("create input journal temporary", create_error));
  if (!write_all(temporary.get(), encoded, error)) {
    temporary = ScopedHandle();
    delete_regular_file(temporary_path_, nullptr);
    return false;
  }
  temporary = ScopedHandle();
  if (!chast::harden_machine_state_file(temporary_path_, error) ||
      !chast::machine_state_file_trusted(temporary_path_, error)) {
    delete_regular_file(temporary_path_, nullptr);
    return false;
  }
  {
    ScopedHandle flushed(CreateFileW(temporary_path_.c_str(),
        GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!flushed.valid()) {
      const DWORD code = GetLastError();
      delete_regular_file(temporary_path_, nullptr);
      return fail(error, win32_error("reopen hardened input journal temporary", code));
    }
    uint64_t size = 0;
    if (!file_is_regular_single_link(flushed.get(), &size, error)) {
      flushed = ScopedHandle();
      delete_regular_file(temporary_path_, nullptr);
      return false;
    }
    if (size != encoded.size()) {
      flushed = ScopedHandle();
      delete_regular_file(temporary_path_, nullptr);
      return fail(error, "hardened input journal temporary changed size");
    }
    if (!FlushFileBuffers(flushed.get())) {
      const DWORD code = GetLastError();
      flushed = ScopedHandle();
      delete_regular_file(temporary_path_, nullptr);
      return fail(error, win32_error("flush hardened input journal temporary", code));
    }
  }

  const DWORD move_flags = MOVEFILE_WRITE_THROUGH |
      (found ? static_cast<DWORD>(MOVEFILE_REPLACE_EXISTING) : DWORD{0});
  if (!MoveFileExW(temporary_path_.c_str(), record_path_.c_str(), move_flags)) {
    const DWORD code = GetLastError();
    delete_regular_file(temporary_path_, nullptr);
    return fail(error, win32_error("atomically install input journal image", code));
  }
  record_loaded_ = true;
  updates_enabled_ = true;
  return true;
}

namespace detail {

std::unique_ptr<PortableHelperInputJournalFileStore>
open_portable_helper_input_journal_file_store_for_test(
    const PortableHelperInputJournalScope& scope,
    const std::wstring& trusted_instance_directory, std::string* error) {
  return PortableHelperInputJournalFileStore::OpenAtDirectory(
      scope, trusted_instance_directory, error);
}

bool remove_released_portable_helper_input_journals_for_test(
    const PortableHelperInstanceId& instance_id,
    const std::wstring& trusted_instance_directory, std::string* error) {
  return PortableHelperInputJournalFileStore::RemoveReleasedAtDirectory(
      instance_id, trusted_instance_directory, error);
}

}  // namespace detail
}  // namespace chast::host
