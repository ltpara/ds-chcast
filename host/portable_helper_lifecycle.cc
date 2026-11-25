#define NOMINMAX
#include "portable_helper_lifecycle.h"

#include <windows.h>
#include <winsvc.h>
#include <shlobj.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>

#include "chast/machine_state_security.h"
#include "chast/security.h"
#include "portable_helper_recovery.h"
#include "portable_helper_service_channel.h"
#include "portable_helper_staging.h"

namespace chast::host {
namespace {

bool nonzero_instance(const PortableHelperInstanceId& instance_id) {
  return std::any_of(instance_id.begin(), instance_id.end(),
                     [](uint8_t value) { return value != 0; });
}

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

bool absolute_windows_path(const std::wstring& path) {
  if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/'))
    return true;
  return path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\';
}

bool valid_sid_string(const std::wstring& value) {
  if (value.empty() || value.find(L'\0') != std::wstring::npos) return false;
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(value.c_str(), &sid)) return false;
  const bool valid = IsValidSid(sid) != FALSE;
  LocalFree(sid);
  return valid;
}

bool normalize_image_path(const std::wstring& input, std::wstring* output,
                          std::string* error) {
  if (output == nullptr || input.empty() || input.find(L'\0') != std::wstring::npos ||
      input.find(L'"') != std::wstring::npos ||
      input.find(L'\r') != std::wstring::npos ||
      input.find(L'\n') != std::wstring::npos || !absolute_windows_path(input))
    return fail(error, "portable helper image path must be an absolute Windows path");

  const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (required == 0) return fail(error, "cannot normalize portable helper image path");
  std::wstring normalized(required, L'\0');
  const DWORD written = GetFullPathNameW(input.c_str(), required, normalized.data(),
                                         nullptr);
  if (written == 0 || written >= required)
    return fail(error, "cannot normalize portable helper image path");
  normalized.resize(written);
  if (!absolute_windows_path(normalized))
    return fail(error, "normalized portable helper image path is not absolute");
  *output = std::move(normalized);
  return true;
}

bool valid_spec(const PortableHelperServiceSpec& spec) {
  std::wstring expected_directory, expected_executable;
  std::string ignored;
  if (!portable_helper_staging_paths(spec.instance_id, &expected_directory,
                                     &expected_executable, &ignored))
    return false;
  return nonzero_instance(spec.instance_id) &&
      spec.service_name == portable_helper_service_name(spec.instance_id) &&
      _wcsicmp(spec.staging_directory.c_str(), expected_directory.c_str()) == 0 &&
      _wcsicmp(spec.executable_path.c_str(), expected_executable.c_str()) == 0 &&
      absolute_windows_path(spec.executable_path) &&
      spec.executable_path.find(L'\0') == std::wstring::npos &&
      spec.executable_path.find(L'"') == std::wstring::npos &&
      spec.parent.process_id != 0 && spec.parent.session_id != 0 &&
      spec.parent.creation_time != 0 &&
      absolute_windows_path(spec.parent.image_path) &&
      spec.parent.image_path.size() < 32768 &&
      spec.parent.image_path.find(L'\0') == std::wstring::npos &&
      valid_sid_string(spec.parent.user_sid) &&
      valid_sid_string(spec.parent.logon_sid);
}

struct ServiceHandle {
  SC_HANDLE value = nullptr;
  explicit ServiceHandle(SC_HANDLE handle = nullptr) : value(handle) {}
  ~ServiceHandle() {
    if (value != nullptr) CloseServiceHandle(value);
  }
  ServiceHandle(const ServiceHandle&) = delete;
  ServiceHandle& operator=(const ServiceHandle&) = delete;
};

struct FileHandle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
  ~FileHandle() {
    if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value);
  }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
};

std::string scm_error(const char* operation, DWORD code) {
  return std::string(operation) + " failed (Win32 error " +
      std::to_string(code) + ")";
}

std::wstring instance_hex(const PortableHelperInstanceId& instance_id) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring value;
  value.reserve(instance_id.size() * 2);
  for (const uint8_t byte : instance_id) {
    value.push_back(digits[byte >> 4]);
    value.push_back(digits[byte & 0x0f]);
  }
  return value;
}

std::wstring service_image_command(const PortableHelperServiceSpec& spec) {
  return L"\"" + spec.executable_path + L"\" --service-name " +
         spec.service_name;
}

bool staged_image_is_trusted(const PortableHelperServiceSpec& spec,
                             std::string* error) {
  const DWORD attributes = GetFileAttributesW(spec.executable_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return fail(error, "portable helper image is missing, a directory, or a reparse point");
  }
  return chast::machine_state_file_trusted(spec.executable_path, error) &&
      verify_configured_portable_helper_publisher_signature(
          spec.executable_path, error);
}

bool staging_directory_has_only_owned_files(
    const PortableHelperServiceSpec& spec, std::string* error) {
  const std::wstring pattern =
      (std::filesystem::path(spec.staging_directory) / L"*").wstring();
  WIN32_FIND_DATAW entry{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE)
    return fail(error, scm_error("FindFirstFile(helper staging)", GetLastError()));
  bool ok = true;
  do {
    if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0)
      continue;
    if (_wcsicmp(entry.cFileName, L"chast-portable-helper-service.exe") != 0 &&
        _wcsicmp(entry.cFileName, L"recovery.dat") != 0) {
      if (error != nullptr)
        *error = "unexpected file exists in the portable helper instance directory";
      ok = false;
      break;
    }
    if ((entry.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      if (error != nullptr)
        *error = "unexpected directory or reparse point exists in the helper instance";
      ok = false;
      break;
    }
  } while (FindNextFileW(find, &entry));
  const DWORD end_error = GetLastError();
  FindClose(find);
  if (ok && end_error != ERROR_NO_MORE_FILES)
    return fail(error, scm_error("FindNextFile(helper staging)", end_error));
  return ok;
}

bool remove_staged_image_files(const PortableHelperServiceSpec& spec,
                               std::string* error) {
  PortableHelperServiceSpec recorded;
  if (!read_portable_helper_recovery_record(spec, &recorded, error)) return false;
  if (!staging_directory_has_only_owned_files(spec, error)) return false;
  const DWORD image_attributes = GetFileAttributesW(spec.executable_path.c_str());
  if (image_attributes != INVALID_FILE_ATTRIBUTES) {
    if ((image_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
      return fail(error, "refusing to remove a non-regular helper image");
    if (!chast::machine_state_file_trusted(spec.executable_path, error)) return false;
    FileHandle image(CreateFileW(spec.executable_path.c_str(),
        DELETE | READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (image.value == INVALID_HANDLE_VALUE)
      return fail(error, scm_error("OpenFile(helper image for cleanup)",
                                   GetLastError()));
    const std::filesystem::path root(spec.staging_directory);
    if (!chast::machine_state_directory_trusted(spec.staging_directory, error) ||
        !chast::machine_state_directory_trusted(root.parent_path().wstring(), error) ||
        !chast::machine_state_directory_trusted(root.parent_path().parent_path().wstring(), error))
      return false;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(image.value, FileAttributeTagInfo,
                                      &tag, sizeof(tag)) ||
        (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
      return fail(error, "helper image changed type before cleanup");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(image.value, &information) ||
        information.nNumberOfLinks != 1)
      return fail(error, "helper image is not a single-link regular file");
    if (!chast::machine_state_file_trusted(spec.executable_path, error)) return false;
    BY_HANDLE_FILE_INFORMATION current_information{};
    if (!GetFileInformationByHandle(image.value, &current_information) ||
        current_information.dwVolumeSerialNumber != information.dwVolumeSerialNumber ||
        current_information.nFileIndexHigh != information.nFileIndexHigh ||
        current_information.nFileIndexLow != information.nFileIndexLow ||
        current_information.nNumberOfLinks != 1)
      return fail(error, "helper image changed before cleanup");
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(image.value, FileDispositionInfo,
                                    &disposition, sizeof(disposition)))
      return fail(error, scm_error("delete helper image by handle", GetLastError()));
  } else {
    const DWORD image_error = GetLastError();
    if (image_error != ERROR_FILE_NOT_FOUND && image_error != ERROR_PATH_NOT_FOUND)
      return fail(error, scm_error("GetFileAttributes(helper image)", image_error));
  }

  const DWORD directory_attributes = GetFileAttributesW(spec.staging_directory.c_str());
  if (directory_attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD directory_error = GetLastError();
    if (directory_error == ERROR_FILE_NOT_FOUND || directory_error == ERROR_PATH_NOT_FOUND)
      return true;
    return fail(error, scm_error("GetFileAttributes(helper staging directory)",
                                 directory_error));
  }
  if ((directory_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (directory_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    return fail(error, "refusing to remove a non-directory helper staging path");
  const std::filesystem::path root(spec.staging_directory);
  if (!chast::machine_state_directory_trusted(spec.staging_directory, error) ||
      !chast::machine_state_directory_trusted(root.parent_path().wstring(), error) ||
      !chast::machine_state_directory_trusted(root.parent_path().parent_path().wstring(), error))
    return false;
  if (!remove_portable_helper_recovery_record(spec, error)) return false;
  const DWORD current_attributes = GetFileAttributesW(spec.staging_directory.c_str());
  if (current_attributes == INVALID_FILE_ATTRIBUTES ||
      (current_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
          FILE_ATTRIBUTE_DIRECTORY)
    return fail(error, "helper staging directory changed before cleanup");
  if (!RemoveDirectoryW(spec.staging_directory.c_str()))
    return fail(error, scm_error("RemoveDirectory(helper staging)", GetLastError()));
  return true;
}

bool current_process_matches_parent(const PortableHelperProcessIdentity& expected,
                                    std::string* error) {
  PortableHelperProcessIdentity actual;
  if (!current_portable_helper_process_identity(&actual, error)) return false;
  if (actual.process_id != expected.process_id ||
      actual.session_id != expected.session_id ||
      actual.creation_time != expected.creation_time ||
      _wcsicmp(actual.image_path.c_str(), expected.image_path.c_str()) != 0 ||
      actual.user_sid != expected.user_sid ||
      actual.logon_sid != expected.logon_sid) {
    return fail(error, "portable helper parent process identity changed");
  }
  return true;
}

bool read_service_config(SC_HANDLE service, QUERY_SERVICE_CONFIGW** config,
                         std::vector<uint8_t>* storage,
                         std::string* error) {
  DWORD needed = 0;
  QueryServiceConfigW(service, nullptr, 0, &needed);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0)
    return fail(error, scm_error("QueryServiceConfig", GetLastError()).c_str());
  storage->resize(needed);
  auto* result = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage->data());
  if (!QueryServiceConfigW(service, result, needed, &needed))
    return fail(error, scm_error("QueryServiceConfig", GetLastError()).c_str());
  *config = result;
  return true;
}

bool verify_service_config(SC_HANDLE service,
                           const PortableHelperServiceSpec& spec,
                           std::string* error) {
  std::vector<uint8_t> storage;
  QUERY_SERVICE_CONFIGW* config = nullptr;
  if (!read_service_config(service, &config, &storage, error)) return false;
  if (config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
      config->dwStartType != SERVICE_DEMAND_START ||
      config->dwErrorControl != SERVICE_ERROR_NORMAL ||
      config->lpBinaryPathName == nullptr ||
      _wcsicmp(config->lpBinaryPathName,
               service_image_command(spec).c_str()) != 0 ||
      config->lpServiceStartName == nullptr ||
      _wcsicmp(config->lpServiceStartName, L"LocalSystem") != 0) {
    return fail(error, "SCM service configuration does not match this helper instance");
  }
  return true;
}

bool verify_service_control_acl(SC_HANDLE service, std::string* error) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD result = GetSecurityInfo(
      service, SE_SERVICE,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &dacl, nullptr, &descriptor);
  if (result != ERROR_SUCCESS) {
    if (descriptor != nullptr) LocalFree(descriptor);
    return fail(error, scm_error("GetSecurityInfo(service)", result));
  }
  const bool trusted_owner = owner != nullptr &&
      (IsWellKnownSid(owner, WinLocalSystemSid) ||
       IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
  const bool has_dacl = dacl != nullptr && IsValidAcl(dacl) != FALSE;
  if (!trusted_owner || !has_dacl) {
    LocalFree(descriptor);
    return fail(error, "SCM helper service has no trusted owner or valid DACL");
  }

  constexpr DWORD kSensitiveRights = SERVICE_CHANGE_CONFIG | SERVICE_START |
      SERVICE_STOP | SERVICE_PAUSE_CONTINUE | SERVICE_USER_DEFINED_CONTROL |
      DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_READ | GENERIC_WRITE |
      GENERIC_EXECUTE | GENERIC_ALL;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (!GetAce(dacl, index, &raw_ace)) {
      LocalFree(descriptor);
      return fail(error, "cannot inspect the SCM helper service DACL");
    }
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType == ACCESS_DENIED_ACE_TYPE ||
        header->AceType == SYSTEM_AUDIT_ACE_TYPE)
      continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      LocalFree(descriptor);
      return fail(error, "SCM helper service DACL contains an unsupported ACE type");
    }
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    const bool trusted = IsWellKnownSid(sid, WinLocalSystemSid) ||
                         IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
    if (!trusted && (ace->Mask & kSensitiveRights) != 0) {
      LocalFree(descriptor);
      return fail(error, "SCM helper service grants control rights to an untrusted SID");
    }
  }
  LocalFree(descriptor);
  return true;
}

bool query_service_status(SC_HANDLE service, SERVICE_STATUS_PROCESS* status,
                          std::string* error) {
  DWORD needed = 0;
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                            reinterpret_cast<BYTE*>(status), sizeof(*status),
                            &needed))
    return fail(error, scm_error("QueryServiceStatusEx", GetLastError()).c_str());
  return true;
}

bool wait_for_service_state(SC_HANDLE service, DWORD wanted_state,
                             DWORD timeout_ms, std::string* error) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    SERVICE_STATUS_PROCESS status{};
    if (!query_service_status(service, &status, error)) return false;
    if (status.dwCurrentState == wanted_state) return true;
    if (wanted_state == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
      return fail(error, "helper service stopped before reaching SERVICE_RUNNING (service exit " +
          std::to_string(status.dwServiceSpecificExitCode != 0
                             ? status.dwServiceSpecificExitCode
                             : status.dwWin32ExitCode) + ")");
    }
    if (Clock::now() >= deadline)
      return fail(error, wanted_state == SERVICE_RUNNING
          ? "timed out waiting for the helper service to start"
          : "timed out waiting for the helper service to stop");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

ServiceHandle open_service(SC_HANDLE manager,
                           const PortableHelperServiceSpec& spec,
                           DWORD access, DWORD* open_error) {
  SC_HANDLE service = OpenServiceW(manager, spec.service_name.c_str(), access);
  if (service == nullptr && open_error != nullptr) *open_error = GetLastError();
  return ServiceHandle(service);
}

std::vector<std::wstring> service_start_arguments(
    const PortableHelperServiceSpec& spec) {
  return {
      L"--portable-helper-instance", instance_hex(spec.instance_id),
      L"--parent-pid", std::to_wstring(spec.parent.process_id),
      L"--parent-created", std::to_wstring(spec.parent.creation_time),
      L"--parent-session", std::to_wstring(spec.parent.session_id),
      L"--parent-image", spec.parent.image_path,
      L"--parent-user-sid", spec.parent.user_sid,
      L"--parent-logon-sid", spec.parent.logon_sid,
  };
}

}  // namespace

std::wstring portable_helper_service_name(
    const PortableHelperInstanceId& instance_id) {
  if (!nonzero_instance(instance_id)) return {};
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring name = L"ChastPortableHelper-";
  name.reserve(name.size() + instance_id.size() * 2);
  for (const uint8_t byte : instance_id) {
    name.push_back(digits[byte >> 4]);
    name.push_back(digits[byte & 0x0f]);
  }
  return name;
}

bool generate_portable_helper_instance_id(
    PortableHelperInstanceId* instance_id, std::string* error) {
  if (instance_id == nullptr)
    return fail(error, "missing portable helper instance ID output");
  *instance_id = {};
  if (!secure_random(instance_id->data(), instance_id->size()))
    return fail(error, "cannot generate a random portable helper instance ID");
  if (!nonzero_instance(*instance_id))
    return fail(error, "random portable helper instance ID was empty");
  return true;
}

bool portable_helper_staging_paths(
    const PortableHelperInstanceId& instance_id,
    std::wstring* staging_directory, std::wstring* executable_path,
    std::string* error) {
  if (staging_directory == nullptr || executable_path == nullptr)
    return fail(error, "missing portable helper staging path output");
  staging_directory->clear();
  executable_path->clear();
  if (!nonzero_instance(instance_id))
    return fail(error, "empty portable helper instance id");
  PWSTR program_data = nullptr;
  const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData,
                                               KF_FLAG_DEFAULT, nullptr,
                                               &program_data);
  if (FAILED(result) || program_data == nullptr)
    return fail(error, "cannot resolve the machine ProgramData folder");
  const std::filesystem::path directory =
      std::filesystem::path(program_data) / L"Chast" / L"PortableHelper" /
      instance_hex(instance_id);
  CoTaskMemFree(program_data);
  *staging_directory = directory.wstring();
  *executable_path = (directory / L"chast-portable-helper-service.exe").wstring();
  return true;
}

bool Win32PortableHelperServiceBackend::CreateExclusive(
    const PortableHelperServiceSpec& spec, std::string* error) {
  if (!valid_spec(spec))
    return fail(error, "invalid portable helper service instance specification");
  if (!staged_image_is_trusted(spec, error)) return false;
  ServiceHandle manager(OpenSCManagerW(nullptr, nullptr,
                                       SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
  if (manager.value == nullptr)
    return fail(error, scm_error("OpenSCManager", GetLastError()).c_str());

  const std::wstring image = service_image_command(spec);
  const std::wstring display_name = L"Chast Portable Helper [" +
                                    instance_hex(spec.instance_id) + L"]";
  ServiceHandle service(CreateServiceW(
      manager.value, spec.service_name.c_str(), display_name.c_str(),
      SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | SERVICE_START |
          SERVICE_STOP | DELETE,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
      image.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
  if (service.value == nullptr)
    return fail(error, scm_error("CreateService", GetLastError()).c_str());
  return true;
}

bool Win32PortableHelperServiceBackend::Start(
    const PortableHelperServiceSpec& spec, std::string* error) {
  if (!valid_spec(spec))
    return fail(error, "invalid portable helper service instance specification");
  if (!current_process_matches_parent(spec.parent, error) ||
      !staged_image_is_trusted(spec, error))
    return false;
  ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (manager.value == nullptr)
    return fail(error, scm_error("OpenSCManager", GetLastError()).c_str());
  DWORD open_error = ERROR_SUCCESS;
  ServiceHandle service = open_service(
      manager.value, spec, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG |
          SERVICE_START, &open_error);
  if (service.value == nullptr)
    return fail(error, scm_error("OpenService", open_error).c_str());
  if (!verify_service_config(service.value, spec, error)) return false;
  if (!verify_service_control_acl(service.value, error)) return false;

  auto arguments = service_start_arguments(spec);
  std::vector<LPCWSTR> argument_pointers;
  argument_pointers.reserve(arguments.size());
  for (const auto& argument : arguments)
    argument_pointers.push_back(argument.c_str());
  if (!StartServiceW(service.value,
                     static_cast<DWORD>(argument_pointers.size()),
                     argument_pointers.data())) {
    const DWORD start_error = GetLastError();
    return fail(error, scm_error("StartService", start_error).c_str());
  }
  return wait_for_service_state(service.value, SERVICE_RUNNING, 20000, error);
}

bool Win32PortableHelperServiceBackend::Stop(
    const PortableHelperServiceSpec& spec, std::string* error) {
  if (!valid_spec(spec))
    return fail(error, "invalid portable helper service instance specification");
  ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (manager.value == nullptr)
    return fail(error, scm_error("OpenSCManager", GetLastError()).c_str());
  DWORD open_error = ERROR_SUCCESS;
  ServiceHandle service = open_service(
      manager.value, spec, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG |
          SERVICE_STOP, &open_error);
  if (service.value == nullptr) {
    if (open_error == ERROR_SERVICE_DOES_NOT_EXIST ||
        open_error == ERROR_SERVICE_MARKED_FOR_DELETE)
      return true;
    return fail(error, scm_error("OpenService", open_error).c_str());
  }
  if (!verify_service_config(service.value, spec, error)) return false;
  if (!verify_service_control_acl(service.value, error)) return false;

  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  for (;;) {
    SERVICE_STATUS_PROCESS status{};
    if (!query_service_status(service.value, &status, error)) return false;
    if (status.dwCurrentState == SERVICE_STOPPED) return true;
    if (status.dwCurrentState == SERVICE_STOP_PENDING) {
      if (Clock::now() >= deadline)
        return fail(error, "timed out waiting for the helper service to stop");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    if (status.dwCurrentState == SERVICE_START_PENDING ||
        status.dwCurrentState == SERVICE_CONTINUE_PENDING ||
        status.dwCurrentState == SERVICE_PAUSE_PENDING) {
      if (Clock::now() >= deadline)
        return fail(error, "timed out waiting for the helper service transition");
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    SERVICE_STATUS ignored{};
    if (!ControlService(service.value, SERVICE_CONTROL_STOP, &ignored)) {
      const DWORD stop_error = GetLastError();
      if (stop_error == ERROR_SERVICE_NOT_ACTIVE) continue;
      if (stop_error == ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
        if (Clock::now() >= deadline)
          return fail(error, scm_error("ControlService(STOP)", stop_error).c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      return fail(error, scm_error("ControlService(STOP)", stop_error).c_str());
    }
  }
}

bool Win32PortableHelperServiceBackend::DeleteIfOwned(
    const PortableHelperServiceSpec& spec, std::string* error) {
  if (!valid_spec(spec))
    return fail(error, "invalid portable helper service instance specification");
  ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (manager.value == nullptr)
    return fail(error, scm_error("OpenSCManager", GetLastError()).c_str());
  DWORD open_error = ERROR_SUCCESS;
  ServiceHandle service = open_service(
      manager.value, spec, DELETE | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
      &open_error);
  if (service.value == nullptr) {
    if (open_error == ERROR_SERVICE_DOES_NOT_EXIST ||
        open_error == ERROR_SERVICE_MARKED_FOR_DELETE)
      return true;
    return fail(error, scm_error("OpenService", open_error).c_str());
  }
  if (!verify_service_config(service.value, spec, error)) return false;
  if (!verify_service_control_acl(service.value, error)) return false;
  SERVICE_STATUS_PROCESS status{};
  if (!query_service_status(service.value, &status, error)) return false;
  if (status.dwCurrentState != SERVICE_STOPPED)
    return fail(error, "refusing to delete a helper service that is not stopped");
  if (!DeleteService(service.value)) {
    const DWORD delete_error = GetLastError();
    if (delete_error != ERROR_SERVICE_MARKED_FOR_DELETE)
      return fail(error, scm_error("DeleteService", delete_error).c_str());
  }
  return true;
}

bool Win32PortableHelperServiceBackend::RemoveStagedImage(
    const PortableHelperServiceSpec& spec, std::string* error) {
  if (!valid_spec(spec))
    return fail(error, "invalid portable helper service instance specification");
  ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (manager.value == nullptr)
    return fail(error, scm_error("OpenSCManager", GetLastError()).c_str());
  DWORD open_error = ERROR_SUCCESS;
  ServiceHandle service = open_service(
      manager.value, spec, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG,
      &open_error);
  if (service.value != nullptr)
    return fail(error, "helper service is still registered; keeping its image");
  if (open_error == ERROR_SERVICE_MARKED_FOR_DELETE)
    return fail(error, "helper service is still marked for deletion; keeping its image");
  if (open_error != ERROR_SERVICE_DOES_NOT_EXIST)
    return fail(error, scm_error("OpenService(helper cleanup check)", open_error));
  return remove_staged_image_files(spec, error);
}

bool make_portable_helper_service_spec(
    const PortableHelperInstanceId& instance_id,
    const std::wstring& executable_path,
    const PortableHelperProcessIdentity& parent,
    PortableHelperServiceSpec* spec, std::string* error) {
  if (spec == nullptr) return fail(error, "missing portable helper service spec output");
  *spec = {};
  if (!nonzero_instance(instance_id))
    return fail(error, "empty portable helper instance id");
  if (parent.process_id == 0 || parent.session_id == 0 ||
      parent.creation_time == 0 ||
      !absolute_windows_path(parent.image_path) ||
      parent.image_path.size() >= 32768 ||
      parent.image_path.find(L'\0') != std::wstring::npos ||
      !valid_sid_string(parent.user_sid) || !valid_sid_string(parent.logon_sid))
    return fail(error, "portable helper parent identity is incomplete or non-interactive");

  std::wstring expected_directory, expected_executable;
  if (!portable_helper_staging_paths(instance_id, &expected_directory,
                                     &expected_executable, error))
    return false;
  std::wstring normalized_path;
  if (!normalize_image_path(executable_path, &normalized_path, error)) return false;
  if (_wcsicmp(normalized_path.c_str(), expected_executable.c_str()) != 0)
    return fail(error, "portable helper image must use its instance-specific staging path");
  spec->instance_id = instance_id;
  spec->service_name = portable_helper_service_name(instance_id);
  spec->staging_directory = std::move(expected_directory);
  spec->executable_path = std::move(normalized_path);
  spec->parent = parent;
  return true;
}

bool parse_portable_helper_service_start_arguments(
    const std::wstring& service_name,
    const std::vector<std::wstring>& arguments,
    const std::wstring& executable_path,
    PortableHelperServiceSpec* spec, std::string* error) {
  if (error != nullptr) error->clear();
  if (spec == nullptr)
    return fail(error, "missing portable helper service spec output");
  *spec = {};
  if (arguments.size() != 14)
    return fail(error, "portable helper service start arguments are incomplete");

  size_t cursor = 0;
  auto value_for = [&](const wchar_t* flag, std::wstring* value) {
    if (cursor + 1 >= arguments.size() || arguments[cursor] != flag ||
        arguments[cursor + 1].empty())
      return false;
    *value = arguments[cursor + 1];
    cursor += 2;
    return true;
  };
  auto parse_unsigned = [](const std::wstring& text, uint64_t maximum,
                           uint64_t* value) {
    if (value == nullptr || text.empty()) return false;
    if (!std::all_of(text.begin(), text.end(),
                     [](wchar_t digit) { return digit >= L'0' && digit <= L'9'; }))
      return false;
    errno = 0;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != L'\0' ||
        parsed == 0 || parsed > maximum)
      return false;
    *value = static_cast<uint64_t>(parsed);
    return true;
  };

  std::wstring instance_text;
  if (!value_for(L"--portable-helper-instance", &instance_text) ||
      instance_text.size() != kPortableHelperInstanceIdBytes * 2)
    return fail(error, "portable helper instance argument is invalid");
  auto hex_digit = [](wchar_t value, uint8_t* digit) {
    if (value >= L'0' && value <= L'9') {
      *digit = static_cast<uint8_t>(value - L'0');
      return true;
    }
    if (value >= L'a' && value <= L'f') {
      *digit = static_cast<uint8_t>(value - L'a' + 10);
      return true;
    }
    if (value >= L'A' && value <= L'F') {
      *digit = static_cast<uint8_t>(value - L'A' + 10);
      return true;
    }
    return false;
  };
  PortableHelperInstanceId instance_id{};
  for (size_t index = 0; index < instance_id.size(); ++index) {
    uint8_t high = 0, low = 0;
    if (!hex_digit(instance_text[index * 2], &high) ||
        !hex_digit(instance_text[index * 2 + 1], &low))
      return fail(error, "portable helper instance argument is not hexadecimal");
    instance_id[index] = static_cast<uint8_t>((high << 4) | low);
  }

  PortableHelperProcessIdentity parent;
  std::wstring text;
  uint64_t number = 0;
  if (!value_for(L"--parent-pid", &text) ||
      !parse_unsigned(text, std::numeric_limits<uint32_t>::max(), &number))
    return fail(error, "portable helper parent PID argument is invalid");
  parent.process_id = static_cast<uint32_t>(number);
  if (!value_for(L"--parent-created", &text) ||
      !parse_unsigned(text, std::numeric_limits<uint64_t>::max(), &number))
    return fail(error, "portable helper parent creation time argument is invalid");
  parent.creation_time = number;
  if (!value_for(L"--parent-session", &text) ||
      !parse_unsigned(text, std::numeric_limits<uint32_t>::max(), &number))
    return fail(error, "portable helper parent session argument is invalid");
  parent.session_id = static_cast<uint32_t>(number);
  if (!value_for(L"--parent-image", &parent.image_path) ||
      !value_for(L"--parent-user-sid", &parent.user_sid) ||
      !value_for(L"--parent-logon-sid", &parent.logon_sid) ||
      cursor != arguments.size())
    return fail(error, "portable helper parent identity arguments are invalid");

  PortableHelperServiceSpec parsed;
  if (!make_portable_helper_service_spec(instance_id, executable_path, parent,
                                         &parsed, error))
    return false;
  if (service_name != parsed.service_name)
    return fail(error, "portable helper service name does not match its instance ID");
  *spec = std::move(parsed);
  return true;
}

PortableHelperServiceLifecycle::PortableHelperServiceLifecycle(
    PortableHelperServiceSpec spec, PortableHelperServiceBackend& backend)
    : spec_(std::move(spec)), backend_(backend) {}

PortableHelperServiceLifecycle::~PortableHelperServiceLifecycle() = default;

void PortableHelperServiceLifecycle::set_error_locked(
    const std::string& error, std::string* output) {
  last_error_ = error.empty() ? "portable helper service operation failed" : error;
  if (output != nullptr) *output = last_error_;
}

bool PortableHelperServiceLifecycle::CreateAndStart(std::string* error) {
  std::lock_guard lock(mutex_);
  if (error != nullptr) error->clear();
  if (!valid_spec(spec_)) {
    set_error_locked("invalid portable helper service instance specification",
                     error);
    return false;
  }
  if (state_ == PortableHelperServiceState::Running) return true;
  if (state_ == PortableHelperServiceState::CleanupPending ||
      state_ == PortableHelperServiceState::Removed) {
    set_error_locked("portable helper service is pending cleanup or was removed",
                     error);
    return false;
  }
  if (!staged_) {
    set_error_locked("portable helper staging image was already removed", error);
    return false;
  }

  if (!created_) {
    std::string backend_error;
    if (!backend_.CreateExclusive(spec_, &backend_error)) {
      const std::string primary_error = backend_error.empty()
          ? "portable helper service could not be created"
          : backend_error;
      std::string cleanup_error;
      if (backend_.RemoveStagedImage(spec_, &cleanup_error)) {
        staged_ = false;
        set_error_locked(primary_error, error);
      } else {
        state_ = PortableHelperServiceState::CleanupPending;
        set_error_locked(primary_error + "; staged image cleanup pending: " +
                             (cleanup_error.empty()
                                  ? "portable helper image could not be removed"
                                  : cleanup_error),
                         error);
      }
      return false;
    }
    created_ = true;
    state_ = PortableHelperServiceState::Created;
  }

  // Start may time out after SCM has accepted the request. Treat the service
  // as potentially running until Stop confirms otherwise.
  running_ = true;
  std::string backend_error;
  if (!backend_.Start(spec_, &backend_error)) {
    const std::string primary_error = backend_error.empty()
        ? "portable helper service failed to start"
        : backend_error;
    std::string cleanup_error;
    const bool cleaned = cleanup_locked(&cleanup_error);
    if (!cleaned)
      set_error_locked(primary_error + "; cleanup pending: " + cleanup_error,
                       error);
    else
      set_error_locked(primary_error, error);
    return false;
  }

  auto connected = std::make_unique<PortableHelperServiceConnection>();
  backend_error.clear();
  if (!backend_.Connect(spec_, 15000, &connected, &backend_error) ||
      !connected) {
    const std::string primary_error = backend_error.empty()
        ? "portable helper authenticated control channel failed"
        : backend_error;
    std::string cleanup_error;
    const bool cleaned = cleanup_locked(&cleanup_error);
    if (!cleaned)
      set_error_locked(primary_error + "; cleanup pending: " + cleanup_error,
                       error);
    else
      set_error_locked(primary_error, error);
    return false;
  }
  connection_ = std::move(connected);
  state_ = PortableHelperServiceState::Running;
  last_error_.clear();
  return true;
}

bool PortableHelperServiceLifecycle::stop_locked(std::string* error) {
  if (!created_ || !running_) return true;
  std::string control_error;
  if (connection_ != nullptr && connection_->service_process != nullptr)
    (void)request_portable_helper_service_stop(*connection_, 5000,
                                              &control_error);
  std::string backend_error;
  if (!backend_.Stop(spec_, &backend_error)) {
    state_ = PortableHelperServiceState::CleanupPending;
    if (!control_error.empty())
      set_error_locked(backend_error + "; control-channel stop: " +
                           control_error,
                       error);
    else
      set_error_locked(backend_error, error);
    return false;
  }
  connection_.reset();
  running_ = false;
  state_ = PortableHelperServiceState::Created;
  return true;
}

bool PortableHelperServiceLifecycle::Stop(std::string* error) {
  std::lock_guard lock(mutex_);
  if (error != nullptr) error->clear();
  if (!stop_locked(error)) return false;
  if (state_ != PortableHelperServiceState::CleanupPending)
    last_error_.clear();
  return true;
}

bool PortableHelperServiceLifecycle::cleanup_locked(std::string* error) {
  if (created_) {
    if (!stop_locked(error)) return false;

    std::string backend_error;
    if (!backend_.DeleteIfOwned(spec_, &backend_error)) {
      state_ = PortableHelperServiceState::CleanupPending;
      set_error_locked(backend_error, error);
      return false;
    }
  }

  if (staged_) {
    std::string backend_error;
    if (!backend_.RemoveStagedImage(spec_, &backend_error)) {
      state_ = PortableHelperServiceState::CleanupPending;
      set_error_locked(backend_error, error);
      return false;
    }
    staged_ = false;
  }
  created_ = false;
  running_ = false;
  state_ = PortableHelperServiceState::Removed;
  last_error_.clear();
  return true;
}

bool PortableHelperServiceLifecycle::Cleanup(std::string* error) {
  std::lock_guard lock(mutex_);
  if (error != nullptr) error->clear();
  if (!cleanup_locked(error)) return false;
  if (state_ != PortableHelperServiceState::Removed)
    state_ = PortableHelperServiceState::NotCreated;
  last_error_.clear();
  return true;
}

PortableHelperServiceState PortableHelperServiceLifecycle::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

std::string PortableHelperServiceLifecycle::last_error() const {
  std::lock_guard lock(mutex_);
  return last_error_;
}

PortableHelperServiceConnection*
PortableHelperServiceLifecycle::connection() const {
  std::lock_guard lock(mutex_);
  return connection_.get();
}

}  // namespace chast::host
