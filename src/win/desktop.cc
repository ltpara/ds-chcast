#include "chast/desktop.h"

#include <windows.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>

#include <string>

namespace chast {

namespace {

// RustDesk opens the input desktop with this mask; DESKTOP_SWITCHDESKTOP and
// GENERIC_WRITE are what a service needs to move onto Winlogon. A plain
// DESKTOP_READOBJECTS handle is enough for identity, but the same handle can
// then be used for the switch, so ask once.
constexpr DWORD kInputDesktopAccess =
    DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL |
    DESKTOP_WRITEOBJECTS | DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | GENERIC_WRITE;

std::string utf8_from_wide(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::string win32_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length != 0 ? std::wstring(buffer, length)
                                    : L"error " + std::to_wstring(code);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    return utf8_from_wide(text);
}

HDESK open_input_desktop() {
    return OpenInputDesktop(0, FALSE, kInputDesktopAccess);
}

// Desktop names are short ("Default", "Winlogon", "Disconnect"); a fixed
// buffer avoids a sizing round trip. The handle must not be closed here: the
// one from GetThreadDesktop belongs to the thread.
std::wstring desktop_name(HDESK desktop) {
    if (desktop == nullptr) {
        return {};
    }
    wchar_t buffer[256]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, buffer, sizeof(buffer), &needed) ||
        needed < sizeof(wchar_t)) {
        return {};
    }
    return std::wstring(buffer, needed / sizeof(wchar_t));
}

bool is_default_desktop(const std::wstring& name) {
    return _wcsicmp(name.c_str(), L"Default") == 0;
}

bool session_locked() {
    PWTSINFOEXW info = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                     WTSSessionInfoEx, reinterpret_cast<LPWSTR*>(&info),
                                     &bytes) ||
        info == nullptr) {
        // A host that cannot read the session information still tries the
        // desktop name; a failed query is not evidence of a lock.
        return false;
    }
    bool locked = false;
    if (bytes >= sizeof(WTSINFOEXW) && info->Level == 1) {
        // SessionFlags, not the connection SessionState: this field is the
        // lock toggle and is documented reversed on Windows 7 only.
        locked = info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    }
    WTSFreeMemory(info);
    return locked;
}

// Ok with an empty name means the session has no signed-in user; Failed means
// the query itself did not work, which must not be read as "no user".
enum class SessionUserQuery { Ok, Empty, Failed };

SessionUserQuery session_user(std::wstring* user) {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSUserName,
                                     &buffer, &bytes) ||
        buffer == nullptr) {
        return SessionUserQuery::Failed;
    }
    std::wstring name(buffer, bytes / sizeof(wchar_t));
    WTSFreeMemory(buffer);
    while (!name.empty() && name.back() == L'\0') {
        name.pop_back();
    }
    *user = name;
    return name.empty() ? SessionUserQuery::Empty : SessionUserQuery::Ok;
}

}  // namespace

bool enable_physical_desktop_coordinates() {
    // RustDesk calls SetProcessDpiAwareness(2) at start-up for the same
    // reason: DXGI reports output rects in physical pixels, so every Win32
    // API that feeds or consumes those coordinates must too. Processes with
    // an awareness manifest already set fail this call; read the effective
    // state instead of trusting the return value.
    if (FAILED(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
        // Older systems without shcore.dll fall back to system awareness,
        // which still keeps the primary display in physical pixels.
        SetProcessDPIAware();
    }
    PROCESS_DPI_AWARENESS awareness = PROCESS_DPI_UNAWARE;
    if (FAILED(GetProcessDpiAwareness(nullptr, &awareness))) {
        return false;
    }
    return awareness == PROCESS_PER_MONITOR_DPI_AWARE;
}

InputDesktopStatus input_desktop_status() {
    InputDesktopStatus status;
    status.locked = session_locked();

    HDESK input = open_input_desktop();
    if (input == nullptr) {
        const DWORD code = GetLastError();
        status.error = "OpenInputDesktop: " + win32_error(code);
        // A locked session whose desktop this process may not open is still
        // known to be on a secure desktop; anything else is unqueryable.
        status.kind = status.locked ? InputDesktopKind::Secure : InputDesktopKind::Unavailable;
        return status;
    }
    const std::wstring name = desktop_name(input);
    const DWORD name_error = name.empty() ? GetLastError() : ERROR_SUCCESS;
    CloseDesktop(input);
    if (name.empty()) {
        status.kind = status.locked ? InputDesktopKind::Secure : InputDesktopKind::Unavailable;
        status.error = "desktop name: " + win32_error(name_error);
        return status;
    }
    status.name = utf8_from_wide(name);
    status.kind = is_default_desktop(name) ? InputDesktopKind::Default : InputDesktopKind::Secure;
    return status;
}

OsLoginStatus os_login_status() {
    OsLoginStatus status;
    std::wstring user;
    const SessionUserQuery query = session_user(&user);
    if (query == SessionUserQuery::Failed) {
        status.error = "WTSUserName: " + win32_error(GetLastError());
        return status;
    }
    // "SYSTEM" means the session is running before any user signed in; a
    // UAC prompt in a signed-in session keeps the user name and is not this.
    const bool prelogin =
        query == SessionUserQuery::Empty || _wcsicmp(user.c_str(), L"SYSTEM") == 0;
    const bool locked = session_locked();
    if (!prelogin && !locked) {
        status.state = OsLoginState::Unlocked;
        return status;
    }
    // The state alone is not enough: only a process that can open the secure
    // desktop can actually type there. The tray application cannot, so it
    // fails here instead of claiming a login it cannot deliver.
    HDESK input = open_input_desktop();
    if (input == nullptr) {
        status.error = "OpenInputDesktop: " + win32_error(GetLastError());
        return status;
    }
    const std::wstring name = desktop_name(input);
    const DWORD name_error = name.empty() ? GetLastError() : ERROR_SUCCESS;
    CloseDesktop(input);
    if (name.empty()) {
        status.error = "desktop name: " + win32_error(name_error);
        return status;
    }
    if (is_default_desktop(name)) {
        // The session reports a logon-like state but input still reaches the
        // ordinary desktop: this is not a prompt we can type into.
        status.error = "input desktop is Default";
        return status;
    }
    status.state = locked ? OsLoginState::Locked : OsLoginState::Logon;
    return status;
}

bool on_input_desktop() {
    HDESK input = open_input_desktop();
    if (input == nullptr) {
        return false;
    }
    const std::wstring input_name = desktop_name(input);
    CloseDesktop(input);
    if (input_name.empty()) {
        return false;
    }
    const std::wstring current_name = desktop_name(GetThreadDesktop(GetCurrentThreadId()));
    return !current_name.empty() && _wcsicmp(input_name.c_str(), current_name.c_str()) == 0;
}

bool follow_input_desktop(std::string* error, bool* switched) {
    if (switched != nullptr) {
        *switched = false;
    }
    HDESK input = open_input_desktop();
    if (input == nullptr) {
        *error = "OpenInputDesktop: " + win32_error(GetLastError());
        return false;
    }
    const std::wstring input_name = desktop_name(input);
    const std::wstring current_name = desktop_name(GetThreadDesktop(GetCurrentThreadId()));
    if (!input_name.empty() && _wcsicmp(input_name.c_str(), current_name.c_str()) == 0) {
        CloseDesktop(input);
        return true;
    }
    if (!SetThreadDesktop(input)) {
        const DWORD code = GetLastError();
        const std::string name = utf8_from_wide(input_name);
        CloseDesktop(input);
        *error = "SetThreadDesktop(" + (name.empty() ? std::string("input") : name) +
                 "): " + win32_error(code);
        return false;
    }
    // The thread keeps its own reference to the desktop; the handle from
    // OpenInputDesktop is no longer needed.
    CloseDesktop(input);
    if (switched != nullptr) {
        *switched = true;
    }
    return true;
}

}  // namespace chast
