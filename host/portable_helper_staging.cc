#define NOMINMAX
#include "portable_helper_staging.h"

#include <windows.h>
#include <aclapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chast/machine_state_security.h"
#include "portable_helper_recovery.h"
#include "portable_helper_protocol.h"

namespace chast::host {
namespace {

constexpr uint64_t kMaximumHelperImageBytes = 512ull * 1024 * 1024;
constexpr DWORD kCopyBufferBytes = 64 * 1024;
#ifndef CHAST_PORTABLE_HELPER_PUBLISHER_CERT_SHA256
#define CHAST_PORTABLE_HELPER_PUBLISHER_CERT_SHA256 ""
#endif
constexpr char kConfiguredPublisherThumbprint[] =
    CHAST_PORTABLE_HELPER_PUBLISHER_CERT_SHA256;

class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
  ~ScopedHandle() {
    if (valid()) CloseHandle(value_);
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& other) noexcept
      : value_(other.release()) {}
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      if (valid()) CloseHandle(value_);
      value_ = other.release();
    }
    return *this;
  }
  HANDLE get() const { return value_; }
  bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
  HANDLE release() { return std::exchange(value_, INVALID_HANDLE_VALUE); }

 private:
  HANDLE value_;
};

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

std::string win32_error(const char* operation, DWORD code) {
  return std::string(operation) + " failed (Win32 error " +
      std::to_string(code) + ")";
}

bool plain_directory(const std::wstring& path, bool* exists,
                     std::string* error) {
  if (exists == nullptr) return fail(error, "missing directory state output");
  *exists = false;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    return fail(error, win32_error("GetFileAttributes(directory)", code));
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    return fail(error, "portable helper staging path is not a plain directory");
  *exists = true;
  return true;
}

bool create_machine_state_directory(const std::wstring& path,
                                    std::string* error) {
  bool exists = false;
  if (!plain_directory(path, &exists, error)) return false;
  if (exists) return chast::machine_state_directory_trusted(path, error);

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  constexpr wchar_t kDirectoryDacl[] =
      L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kDirectoryDacl, SDDL_REVISION_1, &descriptor, nullptr))
    return fail(error, win32_error("build staging directory DACL", GetLastError()));
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  LocalFree(descriptor);
  if (!created) {
    if (create_error != ERROR_ALREADY_EXISTS)
      return fail(error, win32_error("CreateDirectory(staging root)", create_error));
    // Another authorized creator may have won the race. It must finish
    // hardening before this process accepts the shared root.
    return chast::machine_state_directory_trusted(path, error);
  }

  // The directory was just created empty with a protected DACL, so the
  // recursive migration helper cannot traverse or alter another instance.
  if (!chast::harden_machine_state_directory(path, error)) {
    RemoveDirectoryW(path.c_str());
    return false;
  }
  return chast::machine_state_directory_trusted(path, error);
}

bool create_exclusive_instance_directory(const std::wstring& path,
                                         std::string* error) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  constexpr wchar_t kDirectoryDacl[] =
      L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kDirectoryDacl, SDDL_REVISION_1, &descriptor, nullptr))
    return fail(error, win32_error("build helper instance DACL", GetLastError()));
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  LocalFree(descriptor);
  if (!created) {
    return fail(error, create_error == ERROR_ALREADY_EXISTS
        ? "portable helper instance directory already exists"
        : win32_error("CreateDirectory(helper instance)", create_error));
  }
  if (!chast::harden_machine_state_directory(path, error)) {
    RemoveDirectoryW(path.c_str());
    return false;
  }
  return chast::machine_state_directory_trusted(path, error);
}

void append_cleanup_error(std::string* error, const std::string& cleanup) {
  if (error == nullptr || cleanup.empty()) return;
  if (!error->empty()) *error += "; ";
  *error += cleanup;
}

bool remove_created_instance(const std::wstring& executable_path,
                             const std::wstring& staging_directory,
                             bool file_was_created, bool directory_was_created,
                             std::string* error) {
  bool ok = true;
  if (file_was_created && !DeleteFileW(executable_path.c_str())) {
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
      append_cleanup_error(error, win32_error("DeleteFile(helper image)", code));
      ok = false;
    }
  }
  if (directory_was_created && !RemoveDirectoryW(staging_directory.c_str())) {
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
      append_cleanup_error(error,
                           win32_error("RemoveDirectory(helper instance)", code));
      ok = false;
    }
  }
  return ok;
}

bool read_exact_at(HANDLE file, uint64_t offset, void* buffer, DWORD length,
                   std::string* error) {
  if (offset > static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max()))
    return fail(error, "portable helper image header offset is invalid");
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
    return fail(error, win32_error("SetFilePointerEx(helper image)", GetLastError()));
  auto* bytes = static_cast<uint8_t*>(buffer);
  DWORD total = 0;
  while (total < length) {
    DWORD read = 0;
    if (!ReadFile(file, bytes + total, length - total, &read, nullptr))
      return fail(error, win32_error("ReadFile(helper image header)", GetLastError()));
    if (read == 0) return fail(error, "portable helper image header is truncated");
    total += read;
  }
  return true;
}

bool validate_executable_image(HANDLE source, uint64_t size,
                               std::string* error) {
  if (size < sizeof(IMAGE_DOS_HEADER) || size > kMaximumHelperImageBytes)
    return fail(error, "portable helper image size is outside the accepted range");
  IMAGE_DOS_HEADER dos{};
  if (!read_exact_at(source, 0, &dos, sizeof(dos), error)) return false;
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos))
    return fail(error, "portable helper source is not a PE executable");
  const uint64_t nt_offset = static_cast<uint32_t>(dos.e_lfanew);
  const uint64_t minimum_end = nt_offset + sizeof(DWORD) +
      sizeof(IMAGE_FILE_HEADER) + sizeof(WORD);
  if (minimum_end > size)
    return fail(error, "portable helper PE headers are truncated");

  DWORD signature = 0;
  IMAGE_FILE_HEADER header{};
  WORD optional_magic = 0;
  if (!read_exact_at(source, nt_offset, &signature, sizeof(signature), error) ||
      !read_exact_at(source, nt_offset + sizeof(signature), &header,
                     sizeof(header), error) ||
      !read_exact_at(source, nt_offset + sizeof(signature) + sizeof(header),
                     &optional_magic, sizeof(optional_magic), error))
    return false;
  if (signature != IMAGE_NT_SIGNATURE ||
      (header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0 ||
      (optional_magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
       optional_magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC))
    return fail(error, "portable helper source has invalid PE image headers");
  return true;
}

bool normalize_thumbprint(std::string_view value, std::string* normalized,
                          std::string* error) {
  if (normalized == nullptr)
    return fail(error, "missing helper publisher thumbprint output");
  normalized->clear();
  for (const unsigned char character : value) {
    if (character == ':' || character == ' ' || character == '\t') continue;
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F')))
      return fail(error, "helper publisher thumbprint must be hexadecimal");
    normalized->push_back(static_cast<char>(std::toupper(character)));
  }
  if (normalized->size() != 64)
    return fail(error, "helper publisher thumbprint must contain 32 bytes");
  return true;
}

bool verify_authenticode_publisher(
    const std::wstring& path, const std::string& expected_thumbprint,
    std::string* error) {
  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = path.c_str();
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_DATA trust{};
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &file;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL |
                      WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
  const LONG status = WinVerifyTrust(nullptr, &action, &trust);
  std::string actual_thumbprint;
  if (status == ERROR_SUCCESS) {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer = provider == nullptr
        ? nullptr : WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
    PCCERT_CONTEXT certificate = signer == nullptr || signer->csCertChain == 0
        ? nullptr : signer->pasCertChain[0].pCert;
    DWORD hash_size = 0;
    if (certificate == nullptr ||
        !CertGetCertificateContextProperty(
            certificate, CERT_SHA256_HASH_PROP_ID, nullptr, &hash_size) ||
        hash_size != 32) {
      trust.dwStateAction = WTD_STATEACTION_CLOSE;
      WinVerifyTrust(nullptr, &action, &trust);
      return fail(error, "cannot identify the Authenticode publisher certificate");
    }
    std::array<uint8_t, 32> hash{};
    if (!CertGetCertificateContextProperty(
            certificate, CERT_SHA256_HASH_PROP_ID, hash.data(), &hash_size)) {
      const DWORD code = GetLastError();
      trust.dwStateAction = WTD_STATEACTION_CLOSE;
      WinVerifyTrust(nullptr, &action, &trust);
      return fail(error, win32_error("read publisher certificate thumbprint",
                                     code));
    }
    constexpr char digits[] = "0123456789ABCDEF";
    actual_thumbprint.reserve(hash.size() * 2);
    for (const uint8_t byte : hash) {
      actual_thumbprint.push_back(digits[byte >> 4]);
      actual_thumbprint.push_back(digits[byte & 0x0f]);
    }
  }
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &action, &trust);
  if (status != ERROR_SUCCESS) {
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lx",
                  static_cast<unsigned long>(status));
    return fail(error, "portable helper Authenticode validation failed (" +
                       std::string(code) + ")");
  }
  if (actual_thumbprint != expected_thumbprint)
    return fail(error, "portable helper signer does not match the pinned publisher certificate");
  return true;
}

bool open_valid_source(const std::wstring& path, ScopedHandle* source,
                       uint64_t* size, std::string* error) {
  if (source == nullptr || size == nullptr)
    return fail(error, "missing helper source file output");
  HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (raw == INVALID_HANDLE_VALUE)
    return fail(error, win32_error("OpenFile(helper source)", GetLastError()));
  ScopedHandle opened(raw);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &tag, sizeof(tag)))
    return fail(error, win32_error("inspect helper source", GetLastError()));
  if ((tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
    return fail(error, "portable helper source is a directory or reparse point");
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(raw, &information))
    return fail(error, win32_error("GetFileInformation(helper source)", GetLastError()));
  if (information.nNumberOfLinks != 1)
    return fail(error, "portable helper source must not have additional hard links");
  *size = (static_cast<uint64_t>(information.nFileSizeHigh) << 32) |
          information.nFileSizeLow;
  if (!validate_executable_image(raw, *size, error)) return false;
  *source = std::move(opened);
  return true;
}

bool copy_bytes(HANDLE source, HANDLE target, uint64_t size,
                std::string* error) {
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(source, beginning, nullptr, FILE_BEGIN))
    return fail(error, win32_error("rewind helper source", GetLastError()));
  std::array<uint8_t, kCopyBufferBytes> buffer{};
  uint64_t remaining = size;
  while (remaining != 0) {
    const DWORD request = static_cast<DWORD>(std::min<uint64_t>(
        remaining, static_cast<uint64_t>(buffer.size())));
    DWORD read = 0;
    if (!ReadFile(source, buffer.data(), request, &read, nullptr))
      return fail(error, win32_error("ReadFile(helper source)", GetLastError()));
    if (read == 0) return fail(error, "portable helper source ended during copy");
    DWORD written_total = 0;
    while (written_total < read) {
      DWORD written = 0;
      if (!WriteFile(target, buffer.data() + written_total,
                     read - written_total, &written, nullptr))
        return fail(error, win32_error("WriteFile(helper staging)", GetLastError()));
      if (written == 0) return fail(error, "portable helper copy made no progress");
      written_total += written;
    }
    remaining -= read;
  }
  if (!FlushFileBuffers(target))
    return fail(error, win32_error("FlushFileBuffers(helper staging)", GetLastError()));
  return true;
}

bool ensure_staging_roots(const std::wstring& instance_directory,
                          std::string* error) {
  const std::filesystem::path instance(instance_directory);
  const auto helper_root = instance.parent_path();
  const auto chast_root = helper_root.parent_path();
  const auto program_data = chast_root.parent_path();
  bool program_data_exists = false;
  if (!plain_directory(program_data.wstring(), &program_data_exists, error) ||
      !program_data_exists)
    return fail(error, "the ProgramData staging root is unavailable");
  return create_machine_state_directory(chast_root.wstring(), error) &&
         create_machine_state_directory(helper_root.wstring(), error);
}

bool copy_helper_image_file(const std::wstring& source_path,
                            const std::wstring& target_path,
                            const std::string* expected_thumbprint,
                            std::string* error) {
  if (error != nullptr) error->clear();
  if (source_path.empty() || target_path.empty() ||
      _wcsicmp(source_path.c_str(), target_path.c_str()) == 0)
    return fail(error, "portable helper source and staging paths are invalid");
  ScopedHandle source;
  uint64_t source_size = 0;
  if (!open_valid_source(source_path, &source, &source_size, error)) return false;
  // The source handle denies write/delete sharing while WinVerifyTrust opens
  // the same path, preventing a path replacement between verification/copy.
  if (expected_thumbprint != nullptr &&
      !verify_authenticode_publisher(source_path, *expected_thumbprint, error))
    return false;

  HANDLE raw_target = CreateFileW(target_path.c_str(),
      GENERIC_WRITE | FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC | DELETE,
      FILE_SHARE_READ, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (raw_target == INVALID_HANDLE_VALUE)
    return fail(error, win32_error("CreateFile(helper staging)", GetLastError()));
  ScopedHandle target(raw_target);
  if (!copy_bytes(source.get(), target.get(), source_size, error)) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(target.get(), FileDispositionInfo,
                                    &disposition, sizeof(disposition)))
      append_cleanup_error(error,
          win32_error("delete incomplete helper staging file", GetLastError()));
    return false;
  }
  return true;
}

}  // namespace

namespace detail {

bool copy_portable_helper_image_file(const std::wstring& source_path,
                                    const std::wstring& target_path,
                                    std::string* error) {
  return copy_helper_image_file(source_path, target_path, nullptr, error);
}

}  // namespace detail

bool verify_portable_helper_publisher_signature(
    const std::wstring& image_path, std::string_view expected_thumbprint,
    std::string* error) {
  if (error != nullptr) error->clear();
  if (image_path.empty()) return fail(error, "portable helper image path is empty");
  std::string normalized;
  if (!normalize_thumbprint(expected_thumbprint, &normalized, error)) return false;
  return verify_authenticode_publisher(image_path, normalized, error);
}

bool verify_configured_portable_helper_publisher_signature(
    const std::wstring& image_path, std::string* error) {
  if (error != nullptr) error->clear();
  if (kConfiguredPublisherThumbprint[0] == '\0')
    return fail(error, "portable helper staging is disabled: configure the Authenticode publisher SHA-256 thumbprint");
  if (image_path.empty()) return fail(error, "portable helper image path is empty");
  std::string normalized;
  if (!normalize_thumbprint(kConfiguredPublisherThumbprint,
                            &normalized, error)) return false;
  return verify_authenticode_publisher(image_path, normalized, error);
}

bool stage_portable_helper_service_image(
    PortableHelperServiceSpec* spec, std::string* error) {
  if (error != nullptr) error->clear();
  if (spec == nullptr) return fail(error, "missing portable helper service spec output");
  *spec = {};
  std::string expected_publisher;
  if (kConfiguredPublisherThumbprint[0] == '\0')
    return fail(error, "portable helper staging is disabled: configure the Authenticode publisher SHA-256 thumbprint");
  if (!normalize_thumbprint(kConfiguredPublisherThumbprint,
                            &expected_publisher, error))
    return false;
  PortableHelperInstanceId instance_id{};
  if (!generate_portable_helper_instance_id(&instance_id, error)) return false;
  std::wstring staging_directory, executable_path;
  if (!portable_helper_staging_paths(instance_id, &staging_directory,
                                     &executable_path, error))
    return false;
  if (!process_is_elevated_administrator())
    return fail(error, "portable helper staging requires a locally elevated administrator");

  PortableHelperProcessIdentity parent;
  if (!current_portable_helper_process_identity(&parent, error)) return false;
  if (parent.session_id == 0 || parent.user_sid == L"S-1-5-18")
    return fail(error, "portable helper staging requires an interactive elevated parent");

  if (!ensure_staging_roots(staging_directory, error)) return false;

  if (!create_exclusive_instance_directory(staging_directory, error)) return false;
  bool directory_created = true;
  const auto cleanup = [&](bool file_created) {
    const bool removed = remove_created_instance(executable_path,
        staging_directory, file_created, directory_created, error);
    directory_created = false;
    return removed;
  };

  if (!copy_helper_image_file(parent.image_path, executable_path,
                              &expected_publisher, error)) {
    cleanup(false);
    return false;
  }
  const bool file_created = true;
  if (!chast::harden_machine_state_file(executable_path, error) ||
      !chast::machine_state_file_trusted(executable_path, error)) {
    cleanup(file_created);
    return false;
  }
  if (!verify_authenticode_publisher(executable_path, expected_publisher,
                                     error)) {
    cleanup(file_created);
    return false;
  }
  if (!make_portable_helper_service_spec(instance_id, executable_path, parent,
                                         spec, error)) {
    cleanup(file_created);
    return false;
  }
  if (!write_portable_helper_recovery_record(*spec, error)) {
    cleanup(file_created);
    *spec = {};
    return false;
  }
  return true;
}

}  // namespace chast::host
