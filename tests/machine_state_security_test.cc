#include "chast/machine_state_security.h"
#include "machine_state_security_policy.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>

#include <array>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
bool incomplete = false;

void check(bool condition, const char* message);

void test_owner_policy() {
    using chast::host::detail::machine_state_owner_is_system_or_admin;
    using chast::host::detail::machine_state_owner_trusted_for_migration;
    const std::wstring_view installer = L"S-1-5-21-1000";
    check(machine_state_owner_trusted_for_migration(L"S-1-5-18", installer, false),
        "SYSTEM is trusted as a migration owner without an elevated installer");
    check(machine_state_owner_trusted_for_migration(L"S-1-5-32-544", installer, false),
        "Administrators is trusted as a migration owner");
    check(machine_state_owner_trusted_for_migration(installer, installer, true),
        "the elevated installer is trusted to migrate its own machine state");
    check(!machine_state_owner_trusted_for_migration(installer, installer, false),
        "a non-elevated installer is not trusted to migrate its own machine state");
    check(!machine_state_owner_trusted_for_migration(L"S-1-5-32-545", installer, true),
        "BUILTIN Users is rejected as a migration owner");
    check(machine_state_owner_is_system_or_admin(L"S-1-5-18") &&
              machine_state_owner_is_system_or_admin(L"S-1-5-32-544") &&
              !machine_state_owner_is_system_or_admin(L"S-1-5-21-1000"),
        "read-only trust accepts only SYSTEM or Administrators as the owner");
}

class ScopedPrivilege {
  public:
    explicit ScopedPrivilege(const wchar_t* name) {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                              &token_)) {
            error_ = GetLastError();
            return;
        }
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
            error_ = GetLastError();
            return;
        }
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        requested.Privileges[0].Luid = luid;
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        const BOOL adjusted = AdjustTokenPrivileges(token_, FALSE, &requested,
            sizeof(requested), &previous_, &previous_size_);
        error_ = GetLastError();
        enabled_ = adjusted != FALSE && error_ == ERROR_SUCCESS;
    }

    ~ScopedPrivilege() {
        if (enabled_) AdjustTokenPrivileges(token_, FALSE, &previous_, 0, nullptr, nullptr);
        if (token_ != nullptr) CloseHandle(token_);
    }

    ScopedPrivilege(const ScopedPrivilege&) = delete;
    ScopedPrivilege& operator=(const ScopedPrivilege&) = delete;
    bool enabled() const { return enabled_; }
    DWORD error() const { return error_; }

  private:
    HANDLE token_ = nullptr;
    TOKEN_PRIVILEGES previous_{};
    DWORD previous_size_ = sizeof(previous_);
    bool enabled_ = false;
    DWORD error_ = ERROR_SUCCESS;
};

bool token_privilege_state(const wchar_t* name, bool* assigned, DWORD* attributes) {
    if (assigned == nullptr || attributes == nullptr) return false;
    *assigned = false;
    *attributes = 0;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
        CloseHandle(token);
        return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    const bool read = bytes != 0 &&
        GetTokenInformation(token, TokenPrivileges, buffer.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!read) return false;
    const auto* privileges = reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.data());
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
        const LUID current = privileges->Privileges[i].Luid;
        if (current.LowPart == luid.LowPart && current.HighPart == luid.HighPart) {
            *assigned = true;
            *attributes = privileges->Privileges[i].Attributes;
            break;
        }
    }
    return true;
}

bool current_user_sid(std::vector<unsigned char>* buffer, PSID* sid) {
    if (buffer == nullptr || sid == nullptr) return false;
    *sid = nullptr;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    buffer->resize(bytes);
    const bool read = bytes != 0 &&
        GetTokenInformation(token, TokenUser, buffer->data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!read) return false;
    *sid = reinterpret_cast<TOKEN_USER*>(buffer->data())->User.Sid;
    return *sid != nullptr;
}

void check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() = default;
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&& other) noexcept : path(std::move(other.path)) {
        other.path.clear();
    }
    TempDirectory& operator=(TempDirectory&& other) noexcept {
        if (this != &other) {
            path = std::move(other.path);
            other.path.clear();
        }
        return *this;
    }
    ~TempDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

TempDirectory make_temp_directory(const wchar_t* name) {
    wchar_t base[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(base)), base);
    if (length == 0 || length >= std::size(base)) return {};
    TempDirectory result;
    result.path = std::filesystem::path(base) /
        (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(result.path.c_str(), nullptr)) result.path.clear();
    return result;
}

bool set_dacl(const std::filesystem::path& path, const wchar_t* sddl,
              bool protect = true) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) return false;
    BOOL present = FALSE;
    PACL acl = nullptr;
    BOOL defaulted = FALSE;
    const bool read = GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) &&
                      present != FALSE;
    const DWORD result = read
        ? SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION |
                  (protect ? PROTECTED_DACL_SECURITY_INFORMATION
                           : UNPROTECTED_DACL_SECURITY_INFORMATION),
              nullptr, nullptr, acl, nullptr)
        : ERROR_INVALID_ACL;
    LocalFree(descriptor);
    return result == ERROR_SUCCESS;
}

bool create_directory_junction(const std::filesystem::path& link,
                               const std::filesystem::path& target,
                               DWORD* error_out) {
    if (error_out != nullptr) *error_out = ERROR_SUCCESS;
    if (!CreateDirectoryW(link.c_str(), nullptr)) {
        if (error_out != nullptr) *error_out = GetLastError();
        return false;
    }
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        if (error_out != nullptr) *error_out = GetLastError();
        return false;
    }

    const std::wstring print_name = target.wstring();
    const std::wstring substitute_name = L"\\??\\" + print_name;
    const size_t substitute_bytes = substitute_name.size() * sizeof(wchar_t);
    const size_t print_bytes = print_name.size() * sizeof(wchar_t);
    const size_t path_bytes = substitute_bytes + sizeof(wchar_t) +
                              print_bytes + sizeof(wchar_t);
    const size_t data_length = 4 * sizeof(USHORT) + path_bytes;
    const size_t buffer_size = sizeof(ULONG) + 2 * sizeof(USHORT) + data_length;
    std::vector<unsigned char> buffer(buffer_size, 0);

    auto* header = reinterpret_cast<ULONG*>(buffer.data());
    auto* data_length_field = reinterpret_cast<USHORT*>(buffer.data() + sizeof(ULONG));
    auto* substitute_offset = reinterpret_cast<USHORT*>(buffer.data() + 8);
    auto* substitute_length = reinterpret_cast<USHORT*>(buffer.data() + 10);
    auto* print_offset = reinterpret_cast<USHORT*>(buffer.data() + 12);
    auto* print_length = reinterpret_cast<USHORT*>(buffer.data() + 14);
    auto* path_buffer = reinterpret_cast<wchar_t*>(buffer.data() + 16);
    *header = IO_REPARSE_TAG_MOUNT_POINT;
    *data_length_field = static_cast<USHORT>(data_length);
    *substitute_offset = 0;
    *substitute_length = static_cast<USHORT>(substitute_bytes);
    *print_offset = static_cast<USHORT>(substitute_bytes + sizeof(wchar_t));
    *print_length = static_cast<USHORT>(print_bytes);
    std::memcpy(path_buffer, substitute_name.c_str(), substitute_bytes + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<unsigned char*>(path_buffer) + substitute_bytes + sizeof(wchar_t),
                print_name.c_str(), print_bytes + sizeof(wchar_t));

    DWORD returned = 0;
    const BOOL linked = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    const DWORD link_error = linked ? ERROR_SUCCESS : GetLastError();
    CloseHandle(directory);
    if (error_out != nullptr) *error_out = link_error;
    return linked != FALSE;
}

bool has_sid_right(const std::filesystem::path& path, WELL_KNOWN_SID_TYPE sid_type,
                   bool write, bool* protected_dacl = nullptr) {
    constexpr DWORD write_mask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
        FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
        GENERIC_WRITE | GENERIC_ALL;
    constexpr DWORD read_mask = FILE_READ_DATA | FILE_LIST_DIRECTORY | FILE_READ_EA |
        FILE_READ_ATTRIBUTES | READ_CONTROL | GENERIC_READ | GENERIC_ALL;
    const DWORD mask = write ? write_mask : read_mask;
    DWORD sid_size = SECURITY_MAX_SID_SIZE;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> sid_buffer{};
    if (!CreateWellKnownSid(sid_type, nullptr, sid_buffer.data(), &sid_size)) return false;
    PSID expected = sid_buffer.data();

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    const DWORD read = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor);
    if (read != ERROR_SUCCESS) return false;
    if (protected_dacl != nullptr) {
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        *protected_dacl = GetSecurityDescriptorControl(descriptor, &control, &revision) &&
                          (control & SE_DACL_PROTECTED) != 0;
    }
    bool found = false;
    ACL_SIZE_INFORMATION info{};
    if (acl != nullptr && GetAclInformation(acl, &info, sizeof(info), AclSizeInformation)) {
        for (DWORD i = 0; i < info.AceCount; ++i) {
            LPVOID raw = nullptr;
            if (!GetAce(acl, i, &raw) || raw == nullptr) continue;
            const auto* header = static_cast<const ACE_HEADER*>(raw);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            if (EqualSid(expected, const_cast<DWORD*>(&ace->SidStart)) &&
                (ace->Mask & mask) != 0) {
                found = true;
                break;
            }
        }
    }
    LocalFree(descriptor);
    return found;
}

bool write_text(const std::filesystem::path& path, const char* text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    return output.good();
}

void test_trust_checks_do_not_create_missing_state() {
    TempDirectory temp = make_temp_directory(L"chast-state-readonly");
    check(!temp.path.empty(), "temporary read-only fixture is created");
    if (temp.path.empty()) return;

    const auto missing_directory = temp.path / L"missing" / L"chast";
    std::string error;
    check(!chast::machine_state_directory_trusted(missing_directory.wstring(), &error),
        "a missing machine-state directory is not trusted");
    check(!std::filesystem::exists(missing_directory),
        "checking a missing machine-state directory does not create it");

    const auto state = temp.path / L"chast";
    check(chast::harden_machine_state_directory(state.wstring(), &error),
        "a secure directory is created for the missing-file fixture");
    if (!std::filesystem::exists(state)) return;
    const auto missing_file = state / L"host.ini";
    check(chast::machine_state_file_trusted(missing_file.wstring(), &error),
        "a missing file under a trusted directory is allowed for first write");
    check(!std::filesystem::exists(missing_file),
        "checking a missing machine-state file does not create it");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void test_legacy_inherited_acl_migrates() {
    TempDirectory temp = make_temp_directory(L"chast-state-legacy");
    check(!temp.path.empty(), "temporary legacy fixture is created");
    if (temp.path.empty()) return;

    const auto parent = temp.path / L"ProgramData";
    const auto state = parent / L"chast";
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    check(!ec, "legacy ProgramData fixture is created");
    if (ec) return;
    check(set_dacl(parent,
        L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GW;;;BU)(A;OICI;FA;;;CO)", false),
        "legacy parent grants inherited Users and Creator Owner write access");
    check(CreateDirectoryW(state.c_str(), nullptr) != FALSE,
        "legacy machine-state directory is created under the unsafe parent");
    const auto settings = state / L"host.ini";
    check(write_text(settings, "access_token=kept-token\n"),
        "legacy machine settings are written");

    std::string error;
    const bool migrated = chast::harden_machine_state_directory(state.wstring(), &error);
    if (!migrated) std::printf("migration detail: %s\n", error.c_str());
    check(migrated,
        "inherited Users write permissions migrate to a protected state tree");
    const bool trusted = chast::machine_state_directory_trusted(state.wstring(), &error);
    if (!trusted) std::printf("trust detail: %s\n", error.c_str());
    check(trusted,
        "migrated machine-state directory is trusted");
    check(read_text(settings) == "access_token=kept-token\n",
        "migration preserves existing settings bytes");
    bool directory_dacl_protected = false;
    check(!has_sid_right(state, WinBuiltinUsersSid, true,
                         &directory_dacl_protected) && directory_dacl_protected,
        "the hardened directory has a protected DACL without Users write");
    bool file_dacl_protected = false;
    check(!has_sid_right(settings, WinBuiltinUsersSid, false,
                         &file_dacl_protected) && file_dacl_protected &&
              !has_sid_right(settings, WinBuiltinUsersSid, true),
        "machine settings are unreadable and unwritable by Users");

    const auto display_state = state / L"virtual-display.json";
    check(write_text(display_state, "{}\n"), "virtual-display fixture is written");
    check(chast::harden_machine_state_directory(state.wstring(), &error),
        "repeat hardening protects newly created state files");
    check(has_sid_right(display_state, WinBuiltinUsersSid, false),
        "virtual-display status remains readable by Users");
    check(!has_sid_right(display_state, WinBuiltinUsersSid, true),
        "virtual-display status does not grant Users write access");
}

void test_untrusted_entries_fail_closed() {
    TempDirectory temp = make_temp_directory(L"chast-state-untrusted");
    check(!temp.path.empty(), "temporary untrusted fixture is created");
    if (temp.path.empty()) return;
    const auto state = temp.path / L"chast";
    check(chast::harden_machine_state_directory(state.wstring(), nullptr),
        "secure state directory is created for the untrusted-entry fixture");
    const auto settings = state / L"host.ini";
    check(write_text(settings, "sentinel\n"), "untrusted settings fixture is written");
    check(set_dacl(settings,
        L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GW;;;BU)"),
        "explicit Users-write grant is planted on the settings file");
    std::string error;
    check(chast::harden_machine_state_directory(state.wstring(), &error),
        "an explicit Users-write grant is removed from a trusted-owner file");
    check(read_text(settings) == "sentinel\n",
        "hardening preserves the trusted-owner file contents");
    check(!has_sid_right(settings, WinBuiltinUsersSid, true),
        "hardening removes Users write from the planted file DACL");

}

void test_restore_privilege_is_restored_after_owner_migration(
    const std::filesystem::path& parent) {
    if (!chast::process_is_elevated_administrator()) {
        incomplete = true;
        std::puts("SKIP privilege-scope fixture requires an elevated administrator");
        return;
    }
    const auto state = parent / L"installer-owned";
    if (!CreateDirectoryW(state.c_str(), nullptr)) {
        check(false, "installer-owned privilege fixture is created");
        return;
    }

    std::vector<unsigned char> user_buffer;
    PSID user_sid = nullptr;
    check(current_user_sid(&user_buffer, &user_sid),
          "the installer SID is available for privilege-scope testing");
    if (user_sid == nullptr) return;
    const DWORD owner_result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(state.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
        user_sid, nullptr, nullptr, nullptr);
    if (owner_result != ERROR_SUCCESS) {
        incomplete = true;
        std::printf("SKIP cannot set installer-owned fixture (error %lu)\n",
                    static_cast<unsigned long>(owner_result));
        return;
    }

    {
        ScopedPrivilege capability(L"SeRestorePrivilege");
        if (!capability.enabled()) {
            incomplete = true;
            std::printf("SKIP cannot exercise privilege restoration (error %lu)\n",
                        static_cast<unsigned long>(capability.error()));
            return;
        }
    }

    bool assigned_before = false;
    DWORD attributes_before = 0;
    check(token_privilege_state(L"SeRestorePrivilege", &assigned_before,
                                &attributes_before),
          "the prior SeRestorePrivilege state is readable");
    if (!assigned_before) {
        incomplete = true;
        std::puts("SKIP SeRestorePrivilege is not assigned to this token");
        return;
    }

    std::string error;
    const bool hardened = chast::harden_machine_state_directory(state.wstring(), &error);
    if (!hardened) std::printf("privilege-scope detail: %s\n", error.c_str());
    check(hardened, "an elevated installer-owned state directory can be migrated");
    bool assigned_after = false;
    DWORD attributes_after = 0;
    check(token_privilege_state(L"SeRestorePrivilege", &assigned_after,
                                &attributes_after) &&
              assigned_after == assigned_before && attributes_after == attributes_before,
          "owner migration restores the process token's original SeRestorePrivilege state");
}

void test_untrusted_owner_fails_closed() {
    TempDirectory temp = make_temp_directory(L"chast-state-hostile-owner");
    check(!temp.path.empty(), "temporary hostile-owner fixture is created");
    if (temp.path.empty()) return;
    const auto state = temp.path / L"chast";
    check(chast::harden_machine_state_directory(state.wstring(), nullptr),
        "secure state directory is created for the hostile-owner fixture");
    const auto settings = state / L"host.ini";
    check(write_text(settings, "sentinel\n"), "hostile-owner settings fixture is written");
    test_restore_privilege_is_restored_after_owner_migration(temp.path);

    unsigned char users_buffer[SECURITY_MAX_SID_SIZE]{};
    DWORD users_size = sizeof(users_buffer);
    check(CreateWellKnownSid(WinBuiltinUsersSid, nullptr, users_buffer, &users_size),
        "Users SID is available for the hostile-owner fixture");
    DWORD owner_result = ERROR_SUCCESS;
    {
        ScopedPrivilege restore(L"SeRestorePrivilege");
        if (!restore.enabled()) {
            incomplete = true;
            std::printf("SKIP cannot enable SeRestorePrivilege (error %lu)\n",
                        static_cast<unsigned long>(restore.error()));
            return;
        }
        owner_result = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(settings.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            users_buffer, nullptr, nullptr, nullptr);
    }
    if (owner_result != ERROR_SUCCESS) {
        incomplete = true;
        std::printf("SKIP could not assign the hostile owner (error %lu)\n",
                    static_cast<unsigned long>(owner_result));
        return;
    }
    check(true, "the settings file is assigned an untrusted owner");
    std::string error;
    check(!chast::harden_machine_state_file(settings.wstring(), &error),
        "a machine-state file owned by Users is rejected");
    check(read_text(settings) == "sentinel\n",
        "rejected untrusted owner leaves the settings bytes unchanged");
}

void test_reparse_points_are_rejected() {
    TempDirectory temp = make_temp_directory(L"chast-state-reparse");
    check(!temp.path.empty(), "temporary reparse fixture is created");
    if (temp.path.empty()) return;
    const auto state = temp.path / L"chast";
    check(chast::harden_machine_state_directory(state.wstring(), nullptr),
        "secure state directory is created for the reparse fixture");
    const auto target = temp.path / L"target";
    const auto target_file = target / L"sentinel.txt";
    const auto link = state / L"junction";
    check(CreateDirectoryW(target.c_str(), nullptr) != FALSE,
        "reparse target directory is created outside state directory");
    if (!std::filesystem::exists(target)) return;
    check(write_text(target_file, "outside\n"), "reparse target sentinel is written");
    DWORD junction_error = ERROR_SUCCESS;
    if (!create_directory_junction(link, target, &junction_error)) {
        incomplete = true;
        std::printf("SKIP directory junction unavailable (error %lu)\n",
                    static_cast<unsigned long>(junction_error));
        return;
    }
    std::string error;
    check(!chast::harden_machine_state_directory(state.wstring(), &error) &&
              error.find("reparse") != std::string::npos,
        "a planted reparse point is rejected without following it");
    check(read_text(target_file) == "outside\n", "reparse target remains unchanged");
}

}  // namespace

int main(int argc, char** argv) {
    if (!chast::process_token_is_elevated()) {
        std::puts("SKIP machine-state ACL tests require an elevated token");
        return 77;
    }
    if (argc == 2 && std::strcmp(argv[1], "--untrusted-owner") == 0) {
        test_untrusted_owner_fails_closed();
        if (failures != 0) return 1;
        return incomplete ? 77 : 0;
    }
    if (argc != 1) return 2;
    test_owner_policy();
    test_trust_checks_do_not_create_missing_state();
    test_legacy_inherited_acl_migrates();
    test_untrusted_entries_fail_closed();
    test_reparse_points_are_rejected();
    if (failures != 0) return 1;
    return incomplete ? 77 : 0;
}
