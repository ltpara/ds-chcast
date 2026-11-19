#include "chast/machine_state_security.h"
#include "machine_state_security_policy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

namespace chast {
namespace {

constexpr DWORD kWriteMask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                             FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE |
                             WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

class ScopedHandle {
  public:
    explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~ScopedHandle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }

  private:
    HANDLE value_;
};

class ScopedFindHandle {
  public:
    explicit ScopedFindHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~ScopedFindHandle() {
        if (value_ != INVALID_HANDLE_VALUE) FindClose(value_);
    }
    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_;
};

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

bool sid_text(PSID sid, std::wstring* result) {
    LPWSTR text = nullptr;
    if (sid == nullptr || !ConvertSidToStringSidW(sid, &text)) return false;
    *result = text;
    LocalFree(text);
    return true;
}

std::wstring current_user_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
    ScopedHandle owned(raw);
    DWORD bytes = 0;
    GetTokenInformation(raw, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) return {};
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(raw, TokenUser, buffer.data(), bytes, &bytes)) return {};
    std::wstring result;
    sid_text(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &result);
    return result;
}

class ScopedRestorePrivilege {
  public:
    ScopedRestorePrivilege() = default;

    ~ScopedRestorePrivilege() {
        (void)restore();
        if (token_ != nullptr) CloseHandle(token_);
    }

    ScopedRestorePrivilege(const ScopedRestorePrivilege&) = delete;
    ScopedRestorePrivilege& operator=(const ScopedRestorePrivilege&) = delete;

    bool enable() {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                              &token_)) {
            error_ = GetLastError();
            return false;
        }
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, L"SeRestorePrivilege", &luid)) {
            error_ = GetLastError();
            return false;
        }
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        requested.Privileges[0].Luid = luid;
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD previous_size = sizeof(previous_);
        SetLastError(ERROR_SUCCESS);
        const BOOL adjusted = AdjustTokenPrivileges(token_, FALSE, &requested,
                                                    sizeof(previous_), &previous_, &previous_size);
        error_ = GetLastError();
        enabled_ = adjusted != FALSE && error_ == ERROR_SUCCESS;
        return enabled_;
    }

    DWORD restore() {
        if (!enabled_) return ERROR_SUCCESS;
        SetLastError(ERROR_SUCCESS);
        if (!AdjustTokenPrivileges(token_, FALSE, &previous_, 0, nullptr, nullptr)) {
            error_ = GetLastError();
            return error_;
        }
        error_ = GetLastError();
        if (error_ != ERROR_SUCCESS) return error_;
        enabled_ = false;
        return ERROR_SUCCESS;
    }

    DWORD error() const { return error_; }

  private:
    HANDLE token_ = nullptr;
    TOKEN_PRIVILEGES previous_{};
    bool enabled_ = false;
    DWORD error_ = ERROR_SUCCESS;
};

bool owner_is_system_or_admin(const std::wstring& owner) {
    return host::detail::machine_state_owner_is_system_or_admin(owner);
}

bool read_owner_acl(HANDLE object, PSID* owner, PACL* acl, PSECURITY_DESCRIPTOR* descriptor,
                    DWORD* result) {
    *owner = nullptr;
    *acl = nullptr;
    *descriptor = nullptr;
    *result = GetSecurityInfo(object, SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                              owner, nullptr, acl, nullptr, descriptor);
    return *result == ERROR_SUCCESS && *owner != nullptr && *acl != nullptr;
}

bool dacl_is_protected(PSECURITY_DESCRIPTOR descriptor) {
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    return descriptor != nullptr &&
           GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE &&
           (control & SE_DACL_PROTECTED) != 0;
}

bool handle_dacl_is_protected(HANDLE object) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetSecurityInfo(object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, nullptr, nullptr, &descriptor);
    const bool protected_dacl = result == ERROR_SUCCESS && dacl_is_protected(descriptor);
    if (descriptor != nullptr) LocalFree(descriptor);
    return protected_dacl;
}

bool owner_trusted_for_migration(PSID owner, std::wstring* owner_sid,
                                 std::string* detail) {
    if (!sid_text(owner, owner_sid)) {
        if (detail != nullptr) *detail = "owner is unavailable";
        return false;
    }
    const std::wstring user_sid = current_user_sid();
    if (host::detail::machine_state_owner_trusted_for_migration(
            *owner_sid, user_sid, process_token_is_elevated())) {
        return true;
    }
    if (detail != nullptr) {
        *detail = "owner is not SYSTEM, Administrators, or the elevated installer";
    }
    return false;
}

bool acl_trusted(PSID owner, PACL acl, std::string* detail) {
    std::wstring owner_sid;
    if (!sid_text(owner, &owner_sid) || acl == nullptr) {
        if (detail != nullptr) *detail = "owner or DACL is unavailable";
        return false;
    }
    if (!owner_is_system_or_admin(owner_sid)) {
        if (detail != nullptr) *detail = "owner is not SYSTEM or Administrators";
        return false;
    }

    ACL_SIZE_INFORMATION size{};
    if (!GetAclInformation(acl, &size, sizeof(size), AclSizeInformation)) {
        if (detail != nullptr) *detail = "DACL could not be inspected";
        return false;
    }
    for (DWORD i = 0; i < size.AceCount; ++i) {
        LPVOID raw = nullptr;
        if (!GetAce(acl, i, &raw) || raw == nullptr) {
            if (detail != nullptr) *detail = "DACL contains an unreadable ACE";
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            if ((header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
                 header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                 header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) &&
                (reinterpret_cast<const ACCESS_ALLOWED_ACE*>(raw)->Mask & kWriteMask) != 0) {
                if (detail != nullptr) *detail = "an object ACE grants untrusted write access";
                return false;
            }
            continue;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        if ((ace->Mask & kWriteMask) == 0) continue;
        std::wstring sid;
        if (!sid_text(const_cast<PSID>(reinterpret_cast<const void*>(&ace->SidStart)), &sid)) {
            if (detail != nullptr) *detail = "an allowed ACE has an invalid SID";
            return false;
        }
        if (!owner_is_system_or_admin(sid)) {
            if (detail != nullptr) *detail = "DACL grants write access to " + to_utf8(sid) +
                " through an allowed ACE";
            return false;
        }
    }
    return true;
}

bool object_is_reparse_or_wrong_type(HANDLE handle, bool directory, std::string* error) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        if (error != nullptr) *error = "cannot inspect file attributes (error " +
            std::to_string(GetLastError()) + ")";
        return true;
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (error != nullptr) *error = "reparse points are not allowed in machine state";
        return true;
    }
    const bool is_directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_directory != directory) {
        if (error != nullptr) *error = directory ? "expected a directory" : "expected a regular file";
        return true;
    }
    if (!directory) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1) {
            if (error != nullptr) *error = "hard-linked files are not allowed in machine state";
            return true;
        }
    }
    return false;
}

HANDLE open_state_object(const std::wstring& path, bool directory, DWORD access) {
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                        (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL);
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
}

bool set_object_security(HANDLE object, bool directory, bool public_directory,
                         bool public_file, std::string* error) {
    PSID owner = nullptr;
    PACL old_acl = nullptr;
    PSECURITY_DESCRIPTOR old_descriptor = nullptr;
    DWORD read_error = ERROR_SUCCESS;
    const bool read = read_owner_acl(object, &owner, &old_acl, &old_descriptor, &read_error);
    std::wstring owner_sid;
    const bool owner_read = read && sid_text(owner, &owner_sid);
    if (old_descriptor != nullptr) LocalFree(old_descriptor);
    if (!owner_read) {
        if (error != nullptr) *error = "cannot read machine-state security (error " +
            std::to_string(read_error) + ")";
        return false;
    }
    const wchar_t* sddl = directory
        ? (public_directory
            ? L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;GR;;;BU)"
            : L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")
        : (public_file
            ? L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)"
            : L"D:P(A;;FA;;;SY)(A;;FA;;;BA)");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                               &descriptor, nullptr)) {
        if (error != nullptr) *error = "cannot build the machine-state DACL";
        return false;
    }
    BOOL present = FALSE;
    PACL acl = nullptr;
    BOOL defaulted = FALSE;
    bool ok = GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admins{};
    DWORD admins_size = static_cast<DWORD>(admins.size());
    ok = ok && CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins.data(), &admins_size);
    if (ok) {
        ScopedRestorePrivilege restore_privilege;
        SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
        const bool change_owner = !owner_is_system_or_admin(owner_sid);
        if (change_owner) info |= OWNER_SECURITY_INFORMATION;
        if (change_owner && !restore_privilege.enable()) {
            ok = false;
            if (error != nullptr) *error = "cannot enable SeRestorePrivilege to set the owner (error " +
                std::to_string(restore_privilege.error()) + ")";
        } else {
            const DWORD result = SetSecurityInfo(object, SE_FILE_OBJECT, info,
                change_owner ? admins.data() : nullptr, nullptr, acl, nullptr);
            if (result != ERROR_SUCCESS) {
                ok = false;
                if (error != nullptr) *error = "cannot apply the protected owner/DACL (error " +
                    std::to_string(result) + ")";
            }
        }
        const DWORD restore_error = restore_privilege.restore();
        if (restore_error != ERROR_SUCCESS) {
            ok = false;
            if (error != nullptr) *error = "cannot restore SeRestorePrivilege state (error " +
                std::to_string(restore_error) + ")";
        }
    } else if (error != nullptr) {
        *error = "cannot read the protected DACL";
    }
    LocalFree(descriptor);
    return ok;
}

bool secure_file_handle(HANDLE file, const std::wstring& path, bool public_file,
                        std::string* error) {
    std::string detail;
    if (object_is_reparse_or_wrong_type(file, false, &detail)) {
        if (error != nullptr) *error = to_utf8(path) + ": " + detail;
        return false;
    }
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD result = ERROR_SUCCESS;
    if (!read_owner_acl(file, &owner, &acl, &descriptor, &result)) {
        if (descriptor != nullptr) LocalFree(descriptor);
        if (error != nullptr) *error = to_utf8(path) + ": cannot read owner/DACL (error " +
            std::to_string(result) + ")";
        return false;
    }
    std::wstring owner_sid;
    const bool trusted = owner_trusted_for_migration(owner, &owner_sid, &detail);
    LocalFree(descriptor);
    if (!trusted) {
        if (error != nullptr) *error = to_utf8(path) + ": " + detail;
        return false;
    }
    if (!set_object_security(file, false, false, public_file, error)) return false;
    if (!read_owner_acl(file, &owner, &acl, &descriptor, &result)) {
        if (descriptor != nullptr) LocalFree(descriptor);
        if (error != nullptr) *error = to_utf8(path) + ": cannot verify owner/DACL (error " +
            std::to_string(result) + ")";
        return false;
    }
    const bool protected_dacl = dacl_is_protected(descriptor);
    const bool verified = protected_dacl && acl_trusted(owner, acl, &detail);
    LocalFree(descriptor);
    if (!verified && error != nullptr) *error = to_utf8(path) + ": " +
        (detail.empty() ? "the DACL is not protected from inheritance" : detail);
    return verified;
}

bool secure_tree(const std::wstring& path, bool root, size_t depth, std::string* error) {
    if (depth > 64) {
        if (error != nullptr) *error = "machine-state directory nesting exceeds 64 levels";
        return false;
    }
    HANDLE raw = open_state_object(path, true,
        READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    ScopedHandle directory(raw);
    if (!directory.valid()) {
        if (error != nullptr) *error = "cannot open " + to_utf8(path) +
            " without following links (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    std::string detail;
    if (object_is_reparse_or_wrong_type(directory.get(), true, &detail)) {
        if (error != nullptr) *error = to_utf8(path) + ": " + detail;
        return false;
    }
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD result = ERROR_SUCCESS;
    if (!read_owner_acl(directory.get(), &owner, &acl, &descriptor, &result)) {
        if (descriptor != nullptr) LocalFree(descriptor);
        if (error != nullptr) *error = to_utf8(path) + ": cannot read owner/DACL (error " +
            std::to_string(result) + ")";
        return false;
    }
    std::wstring owner_sid;
    const bool trusted = owner_trusted_for_migration(owner, &owner_sid, &detail);
    LocalFree(descriptor);
    if (!trusted) {
        if (error != nullptr) *error = to_utf8(path) + ": " + detail;
        return false;
    }
    if (!set_object_security(directory.get(), true, root, false, error)) return false;
    if (!handle_dacl_is_protected(directory.get())) {
        if (error != nullptr) *error = to_utf8(path) + ": the DACL is not protected from inheritance";
        return false;
    }

    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = (std::filesystem::path(path) / L"*").wstring();
    HANDLE find_raw = FindFirstFileW(pattern.c_str(), &entry);
    if (find_raw == INVALID_HANDLE_VALUE) {
        const DWORD find_error = GetLastError();
        if (find_error == ERROR_FILE_NOT_FOUND) return true;
        if (error != nullptr) *error = "cannot enumerate " + to_utf8(path) +
            " (error " + std::to_string(find_error) + ")";
        return false;
    }
    ScopedFindHandle find(find_raw);
    do {
        if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
        const std::wstring child = (std::filesystem::path(path) / entry.cFileName).wstring();
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (error != nullptr) *error = to_utf8(child) + ": reparse points are not allowed in machine state";
            return false;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!secure_tree(child, false, depth + 1, error)) return false;
            continue;
        }
        HANDLE file_raw = open_state_object(child, false,
            GENERIC_READ | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES);
        ScopedHandle file(file_raw);
        if (!file.valid()) {
            if (error != nullptr) *error = "cannot open " + to_utf8(child) +
                " without write/delete sharing (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        const bool public_file = root &&
            (_wcsicmp(entry.cFileName, L"virtual-display.json") == 0 ||
             _wcsicmp(entry.cFileName, L"vdisplay.log") == 0);
        if (!secure_file_handle(file.get(), child, public_file, error)) return false;
    } while (FindNextFileW(find_raw, &entry));

    const DWORD end_error = GetLastError();
    if (end_error != ERROR_NO_MORE_FILES) {
        if (error != nullptr) *error = "cannot finish enumerating " + to_utf8(path) +
            " (error " + std::to_string(end_error) + ")";
        return false;
    }
    return true;
}

}  // namespace

bool process_token_is_elevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    ScopedHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &size) &&
           elevation.TokenIsElevated != 0;
}

bool process_is_elevated_administrator() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    ScopedHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &size) ||
        elevation.TokenIsElevated == 0) return false;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> admins{};
    DWORD admins_size = static_cast<DWORD>(admins.size());
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins.data(), &admins_size)) {
        return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(raw, TokenGroups, nullptr, 0, &bytes);
    if (bytes == 0) return false;
    std::vector<unsigned char> groups_buffer(bytes);
    if (!GetTokenInformation(raw, TokenGroups, groups_buffer.data(), bytes, &bytes)) return false;
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groups_buffer.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if (EqualSid(admins.data(), groups->Groups[i].Sid) &&
            (groups->Groups[i].Attributes & SE_GROUP_ENABLED) != 0 &&
            (groups->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0) return true;
    }
    return false;
}

bool process_can_harden_machine_state() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    ScopedHandle token(raw);
    DWORD bytes = 0;
    GetTokenInformation(raw, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) return false;
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(raw, TokenUser, buffer.data(), bytes, &bytes)) return false;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system{};
    DWORD system_size = static_cast<DWORD>(system.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_size)) return false;
    if (EqualSid(system.data(), reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid)) return true;
    return process_is_elevated_administrator();
}

bool machine_state_directory_trusted(const std::wstring& path, std::string* error) {
    if (error != nullptr) error->clear();
    HANDLE raw = open_state_object(path, true, READ_CONTROL | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
    ScopedHandle directory(raw);
    if (!directory.valid()) {
        if (error != nullptr) *error = "cannot open " + to_utf8(path) +
            " without following links (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    std::string detail;
    if (object_is_reparse_or_wrong_type(directory.get(), true, &detail)) {
        if (error != nullptr) *error = to_utf8(path) + ": " + detail;
        return false;
    }
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD result = ERROR_SUCCESS;
    if (!read_owner_acl(directory.get(), &owner, &acl, &descriptor, &result)) {
        if (descriptor != nullptr) LocalFree(descriptor);
        if (error != nullptr) *error = "cannot read owner/DACL (error " +
            std::to_string(result) + ")";
        return false;
    }
    const bool protected_dacl = dacl_is_protected(descriptor);
    const bool trusted = protected_dacl && acl_trusted(owner, acl, &detail);
    LocalFree(descriptor);
    if (!trusted && error != nullptr) *error = detail.empty()
        ? "the DACL is not protected from inheritance" : detail;
    return trusted;
}

bool harden_machine_state_file(const std::wstring& path, std::string* error) {
    if (error != nullptr) error->clear();
    const std::wstring parent = std::filesystem::path(path).parent_path().wstring();
    if (!machine_state_directory_trusted(parent, error)) return false;
    HANDLE raw = open_state_object(path, false,
        GENERIC_READ | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES);
    ScopedHandle file(raw);
    if (!file.valid()) {
        const DWORD open_error = GetLastError();
        if (open_error == ERROR_FILE_NOT_FOUND || open_error == ERROR_PATH_NOT_FOUND) return true;
        if (error != nullptr) *error = "cannot open " + to_utf8(path) +
            " without following links (error " + std::to_string(open_error) + ")";
        return false;
    }
    return secure_file_handle(file.get(), path, false, error);
}

bool machine_state_file_trusted(const std::wstring& path, std::string* error) {
    if (error != nullptr) error->clear();
    const std::wstring parent = std::filesystem::path(path).parent_path().wstring();
    if (!machine_state_directory_trusted(parent, error)) return false;
    HANDLE raw = open_state_object(path, false,
        GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES);
    ScopedHandle file(raw);
    if (!file.valid()) {
        const DWORD open_error = GetLastError();
        if (open_error == ERROR_FILE_NOT_FOUND || open_error == ERROR_PATH_NOT_FOUND) return true;
        if (error != nullptr) *error = "cannot open " + to_utf8(path) +
            " without following links (error " + std::to_string(open_error) + ")";
        return false;
    }
    std::string detail;
    if (object_is_reparse_or_wrong_type(file.get(), false, &detail)) {
        if (error != nullptr) *error = to_utf8(path) + ": " + detail;
        return false;
    }
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD result = ERROR_SUCCESS;
    if (!read_owner_acl(file.get(), &owner, &acl, &descriptor, &result)) {
        if (descriptor != nullptr) LocalFree(descriptor);
        if (error != nullptr) *error = to_utf8(path) + ": cannot read owner/DACL (error " +
            std::to_string(result) + ")";
        return false;
    }
    const bool protected_dacl = dacl_is_protected(descriptor);
    const bool trusted = protected_dacl && acl_trusted(owner, acl, &detail);
    LocalFree(descriptor);
    if (!trusted && error != nullptr) *error = to_utf8(path) + ": " +
        (detail.empty() ? "the DACL is not protected from inheritance" : detail);
    return trusted;
}

bool harden_machine_state_directory(const std::wstring& path, std::string* error) {
    if (error != nullptr) error->clear();
    if (!process_can_harden_machine_state()) {
        if (error != nullptr) *error = "an elevated administrator or LocalSystem token is required";
        return false;
    }
    if (path.empty()) {
        if (error != nullptr) *error = "machine-state directory path is empty";
        return false;
    }
    const wchar_t* sddl = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;GR;;;BU)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                               &descriptor, nullptr)) {
        if (error != nullptr) *error = "cannot build the initial machine-state DACL";
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!created && create_error != ERROR_ALREADY_EXISTS) {
        if (error != nullptr) *error = "cannot create " + to_utf8(path) + " (error " +
            std::to_string(create_error) + ")";
        return false;
    }
    if (!secure_tree(path, true, 0, error)) return false;
    return machine_state_directory_trusted(path, error);
}

}  // namespace chast
