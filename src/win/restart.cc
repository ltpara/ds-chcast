#include "chast/restart.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

namespace chast {

namespace {

// The reboot needs SeShutdownPrivilege, which is disabled in the process token
// until a caller enables it. AdjustTokenPrivileges succeeds even when the
// token does not hold the privilege, so ERROR_NOT_ALL_ASSIGNED is the real
// verdict.
bool enable_shutdown_privilege(std::string* error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) {
        *error = "OpenProcessToken failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &luid)) {
        *error = "LookupPrivilegeValue failed (" + std::to_string(GetLastError()) + ")";
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const BOOL adjusted =
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    const DWORD status = GetLastError();
    CloseHandle(token);
    if (!adjusted) {
        *error = "AdjustTokenPrivileges failed (" + std::to_string(status) + ")";
        return false;
    }
    if (status == ERROR_NOT_ALL_ASSIGNED) {
        *error = "the process does not hold SeShutdownPrivilege";
        return false;
    }
    return true;
}

}  // namespace

bool schedule_restart(std::string* error) {
    if (!enable_shutdown_privilege(error)) {
        return false;
    }
    // Five seconds leave room for the RestartResult to reach the viewer.
    wchar_t message[] = L"chast: remote restart";
    if (!InitiateSystemShutdownExW(
            nullptr, message, 5, TRUE, TRUE,
            SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE |
                SHTDN_REASON_FLAG_PLANNED)) {
        *error = "InitiateSystemShutdownEx failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

}  // namespace chast
