#include "service.h"

#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "chast/host_settings.h"
#include "chast/machine_state_security.h"
#include "file_snapshot.h"
#include "platform.h"
#include "portable_helper_recovery.h"
#include "rollback_stack.h"
#include "service_install.h"
#include "service_log.h"
#include "service_manager_startup.h"
#include "service_stop.h"
#include "service_uninstall.h"
#include "windows_sessions.h"

namespace chast::host {

namespace {

constexpr wchar_t kServiceDescription[] =
    L"chast 远程桌面主机。服务在活动控制台会话内运行采集与输入，托盘不启动时也可注册到连接服务。";

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_service_stop = nullptr;
HANDLE g_session_wake = nullptr;
HANDLE g_worker_stop = nullptr;
HANDLE g_worker_switch = nullptr;
HANDLE g_job = nullptr;
HANDLE g_worker_process = nullptr;
HANDLE g_helper_recovery_thread = nullptr;
DWORD g_worker_session = 0;
uint64_t g_worker_logon = 0;
// A viewer-selected session outranks the console while it stays active. It is
// cleared when that logon goes away, so automatic console/RDP selection resumes.
WindowsSessionTarget g_selected_session;
std::string g_log_path;
std::mutex g_logoff_mutex;
std::vector<DWORD> g_logged_off_sessions;

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool ok =
        GetTokenInformation(token, TokenElevation, &elevation, size, &size) != FALSE &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return ok;
}

bool query_service_state(DWORD* state) {
    if (state != nullptr) {
        *state = SERVICE_STOPPED;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status{};
    const bool ok = QueryServiceStatus(service, &status) != FALSE;
    if (ok && state != nullptr) {
        *state = status.dwCurrentState;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

bool wait_for_state(SC_HANDLE service, DWORD desired, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        SERVICE_STATUS status{};
        if (!QueryServiceStatus(service, &status)) {
            return false;
        }
        if (status.dwCurrentState == desired) {
            return true;
        }
        if (desired != SERVICE_STOPPED && status.dwCurrentState == SERVICE_STOPPED) {
            return false;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(200);
    }
}

class ServiceConfigSnapshot {
public:
    DWORD capture(SC_HANDLE service) {
        DWORD bytes = 0;
        SetLastError(ERROR_SUCCESS);
        if (QueryServiceConfigW(service, nullptr, 0, &bytes)) return ERROR_INVALID_DATA;
        const DWORD query_error = GetLastError();
        if (query_error != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
            return query_error;
        }
        std::vector<BYTE> buffer(bytes);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (!QueryServiceConfigW(service, config, bytes, &bytes)) return GetLastError();
        if (config->lpBinaryPathName == nullptr) return ERROR_INVALID_DATA;
        start_type_ = config->dwStartType;
        binary_path_ = config->lpBinaryPathName;
        captured_ = true;
        return ERROR_SUCCESS;
    }

    bool matches_target_configuration(const std::wstring& binary_path) const {
        return captured_ && detail::service_configuration_matches_target(
                                start_type_, binary_path_, SERVICE_AUTO_START, binary_path);
    }

    DWORD capture_runtime_configuration(SC_HANDLE service) {
        DWORD error = query_config2(service, SERVICE_CONFIG_DESCRIPTION, description_buffer_);
        if (error != ERROR_SUCCESS) return error;
        const auto* description = reinterpret_cast<const SERVICE_DESCRIPTIONW*>(
            description_buffer_.data());
        description_ = description->lpDescription == nullptr ? L"" : description->lpDescription;

        error = query_config2(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                              delayed_buffer_);
        if (error != ERROR_SUCCESS) return error;
        delayed_start_ = reinterpret_cast<const SERVICE_DELAYED_AUTO_START_INFO*>(
                             delayed_buffer_.data())
                             ->fDelayedAutostart;

        error = query_config2(service, SERVICE_CONFIG_FAILURE_ACTIONS, failure_buffer_);
        if (error != ERROR_SUCCESS) return error;
        const auto* failure = reinterpret_cast<const SERVICE_FAILURE_ACTIONSW*>(
            failure_buffer_.data());
        reset_period_ = failure->dwResetPeriod;
        reboot_message_ = failure->lpRebootMsg == nullptr ? L"" : failure->lpRebootMsg;
        command_ = failure->lpCommand == nullptr ? L"" : failure->lpCommand;
        if (failure->cActions > 0 && failure->lpsaActions == nullptr) {
            return ERROR_INVALID_DATA;
        }
        if (failure->cActions > 0) {
            actions_.assign(failure->lpsaActions,
                            failure->lpsaActions + failure->cActions);
        }
        runtime_captured_ = true;
        return ERROR_SUCCESS;
    }

    DWORD restore(SC_HANDLE service) const {
        if (!captured_) return ERROR_INVALID_STATE;
        if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, start_type_, SERVICE_NO_CHANGE,
                                  binary_path_.c_str(), nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr)) {
            return GetLastError();
        }
        return ERROR_SUCCESS;
    }

    DWORD restore_runtime_configuration(SC_HANDLE service) const {
        if (!runtime_captured_) return ERROR_INVALID_STATE;
        SERVICE_DESCRIPTIONW description{};
        description.lpDescription = const_cast<LPWSTR>(description_.c_str());
        if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description)) {
            return GetLastError();
        }
        SERVICE_DELAYED_AUTO_START_INFO delayed{};
        delayed.fDelayedAutostart = delayed_start_;
        if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)) {
            return GetLastError();
        }
        SERVICE_FAILURE_ACTIONSW failure{};
        failure.dwResetPeriod = reset_period_;
        failure.lpRebootMsg = reboot_message_.empty()
                                  ? nullptr
                                  : const_cast<LPWSTR>(reboot_message_.c_str());
        failure.lpCommand = command_.empty() ? nullptr : const_cast<LPWSTR>(command_.c_str());
        failure.cActions = static_cast<DWORD>(actions_.size());
        failure.lpsaActions = actions_.empty()
                                  ? nullptr
                                  : const_cast<SC_ACTION*>(actions_.data());
        return ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure)
                   ? ERROR_SUCCESS
                   : GetLastError();
    }

    bool runtime_captured() const { return runtime_captured_; }

private:
    static DWORD query_config2(SC_HANDLE service, DWORD level, std::vector<BYTE>& buffer) {
        DWORD bytes = 0;
        SetLastError(ERROR_SUCCESS);
        if (QueryServiceConfig2W(service, level, nullptr, 0, &bytes)) return ERROR_INVALID_DATA;
        const DWORD query_error = GetLastError();
        if (query_error != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return query_error;
        buffer.resize(bytes);
        return QueryServiceConfig2W(service, level, buffer.data(), bytes, &bytes)
                   ? ERROR_SUCCESS
                   : GetLastError();
    }

    DWORD start_type_ = SERVICE_DEMAND_START;
    std::wstring binary_path_;
    bool captured_ = false;
    std::vector<BYTE> description_buffer_;
    std::vector<BYTE> delayed_buffer_;
    std::vector<BYTE> failure_buffer_;
    std::wstring description_;
    std::wstring reboot_message_;
    std::wstring command_;
    DWORD delayed_start_ = FALSE;
    DWORD reset_period_ = INFINITE;
    std::vector<SC_ACTION> actions_;
    bool runtime_captured_ = false;
};

DWORD stop_service_if_running(SC_HANDLE service) {
    return detail::stop_service_until_stopped(
        30000,
        [service](DWORD* state) -> DWORD {
            SERVICE_STATUS status{};
            if (!QueryServiceStatus(service, &status)) return GetLastError();
            *state = status.dwCurrentState;
            return ERROR_SUCCESS;
        },
        [service]() -> DWORD {
            SERVICE_STATUS ignored{};
            return ControlService(service, SERVICE_CONTROL_STOP, &ignored)
                       ? ERROR_SUCCESS
                       : GetLastError();
        },
        [] { return GetTickCount64(); },
        [](DWORD delay) { Sleep(delay); });
}

DWORD restore_service_state(SC_HANDLE service, DWORD previous_state) {
    return detail::restore_service_state_after_stop(
        previous_state,
        [service](DWORD* state) -> DWORD {
            SERVICE_STATUS status{};
            if (!QueryServiceStatus(service, &status)) return GetLastError();
            *state = status.dwCurrentState;
            return ERROR_SUCCESS;
        },
        [service]() -> DWORD {
            if (StartServiceW(service, 0, nullptr)) return ERROR_SUCCESS;
            const DWORD error = GetLastError();
            return error == ERROR_SERVICE_ALREADY_RUNNING ? ERROR_SUCCESS : error;
        },
        [service](DWORD desired, DWORD timeout) {
            return wait_for_state(service, desired, timeout);
        },
        [service] {
            SERVICE_STATUS ignored{};
            return ControlService(service, SERVICE_CONTROL_CONTINUE, &ignored)
                       ? ERROR_SUCCESS
                       : GetLastError();
        },
        [service] {
            SERVICE_STATUS ignored{};
            return ControlService(service, SERVICE_CONTROL_PAUSE, &ignored)
                       ? ERROR_SUCCESS
                       : GetLastError();
        });
}

DWORD delete_created_service(SC_HANDLE service) {
    const DWORD stopped = stop_service_if_running(service);
    if (stopped != ERROR_SUCCESS) return stopped;
    if (!DeleteService(service)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_MARKED_FOR_DELETE) return error;
    }
    return ERROR_SUCCESS;
}

uint16_t tunnel_port_of(const HostSettings& settings) {
    uint16_t port = 0;
    if (settings.tunnel) {
        (void)tunnel_listen_port(settings.tunnel_listen, port);
    }
    return port;
}

void fill_machine_identity(HostSettings& settings, bool* changed) {
    if (settings.access_token.empty()) {
        settings.access_token = generate_access_token();
        *changed = true;
    }
    if (settings.tunnel_id.empty()) {
        settings.tunnel_id = generate_tunnel_id();
        *changed = true;
    }
    if (settings.tunnel_key.empty()) {
        settings.tunnel_key = generate_access_token(16);
        *changed = true;
    }
    if (!settings.tunnel_old_id.empty() && settings.tunnel_old_id == settings.tunnel_id) {
        settings.tunnel_old_id.clear();
        *changed = true;
    }
}

void report_status(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint = wait_hint;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE)
            : 0;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        ++g_status.dwCheckPoint;
    } else {
        g_status.dwCheckPoint = 0;
    }
    if (g_status_handle != nullptr) {
        SetServiceStatus(g_status_handle, &g_status);
    }
}

bool enable_privilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
    const DWORD error = GetLastError();
    CloseHandle(token);
    return adjusted != FALSE && error == ERROR_SUCCESS;
}

DWORD find_process_in_session(const wchar_t* executable, DWORD session) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, executable) != 0) {
            continue;
        }
        DWORD process_session = 0;
        if (ProcessIdToSessionId(entry.th32ProcessID, &process_session) &&
            process_session == session) {
            found = entry.th32ProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return found;
}

// Consume one complete, versioned request bound to its worker and target logon.
bool take_session_switch_request(SessionSwitchRequest& request) {
    const std::wstring path = session_switch_path();
    if (path.empty()) return false;
    // Delete the file we opened, not a newer request atomically renamed over
    // its path while we read. Oversize/truncated IDs are consumed and rejected.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[96]{};
    LARGE_INTEGER size{};
    DWORD read = 0;
    const bool valid = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= sizeof(buffer) &&
        ReadFile(file, buffer, static_cast<DWORD>(size.QuadPart), &read, nullptr) && read == size.QuadPart;
    CloseHandle(file);
    return valid && parse_session_switch(std::string_view(buffer, read), request);
}

std::wstring worker_command_line() {
    return L"\"" + executable_path() + L"\" --worker";
}

bool start_worker(DWORD session, uint64_t logon, std::string* error) {
    if (g_worker_stop != nullptr) {
        ResetEvent(g_worker_stop);
    }
    const DWORD winlogon = find_process_in_session(L"winlogon.exe", session);
    if (winlogon == 0) {
        *error = "找不到会话 " + std::to_string(session) + " 的 winlogon.exe";
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogon);
    if (process == nullptr) {
        *error = "无法打开 winlogon.exe（错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY,
                          &token)) {
        *error = "无法读取 winlogon 令牌（错误 " + std::to_string(GetLastError()) + "）";
        CloseHandle(process);
        return false;
    }
    HANDLE primary = nullptr;
    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary,
                          &primary)) {
        *error = "无法复制 winlogon 令牌（错误 " + std::to_string(GetLastError()) + "）";
        CloseHandle(token);
        CloseHandle(process);
        return false;
    }
    CloseHandle(token);
    CloseHandle(process);

    DWORD token_session = 0;
    DWORD returned = 0;
    if (GetTokenInformation(primary, TokenSessionId, &token_session, sizeof(token_session),
                            &returned) &&
        token_session != session) {
        (void)SetTokenInformation(primary, TokenSessionId, &session, sizeof(session));
    }

    std::wstring command = worker_command_line();
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION info{};
    const std::wstring executable = executable_path();
    const BOOL started = CreateProcessAsUserW(
        primary, executable.c_str(), buffer.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    const DWORD code = GetLastError();
    CloseHandle(primary);
    if (!started) {
        *error = "无法在会话 " + std::to_string(session) + " 启动工作进程（错误 " +
                 std::to_string(code) + "）";
        return false;
    }
    if (g_job != nullptr && !AssignProcessToJobObject(g_job, info.hProcess)) {
        log_event(g_log_path, "worker.job_assign_failed",
                  {{"error", std::to_string(GetLastError())}});
    }
    CloseHandle(info.hThread);
    g_worker_process = info.hProcess;
    g_worker_session = session;
    g_worker_logon = logon;
    log_event(g_log_path, "worker.start",
              {{"pid", std::to_string(info.dwProcessId)},
               {"session", std::to_string(session)}});
    return true;
}

void stop_worker() {
    if (g_worker_process == nullptr) {
        return;
    }
    if (g_worker_stop != nullptr) {
        SetEvent(g_worker_stop);
    }
    const DWORD waited = WaitForSingleObject(g_worker_process, 20000);
    if (waited == WAIT_TIMEOUT) {
        log_event(g_log_path, "worker.stalled");
        TerminateProcess(g_worker_process, 1);
        WaitForSingleObject(g_worker_process, 2000);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(g_worker_process, &exit_code);
    log_event(g_log_path, "worker.exit",
              {{"pid", std::to_string(GetProcessId(g_worker_process))},
               {"code", std::to_string(exit_code)}});
    CloseHandle(g_worker_process);
    g_worker_process = nullptr;
    g_worker_session = 0;
    g_worker_logon = 0;
    if (g_worker_stop != nullptr) {
        ResetEvent(g_worker_stop);
    }
}

DWORD WINAPI recover_portable_helpers(void* cancellation_event) {
    const HANDLE cancellation = static_cast<HANDLE>(cancellation_event);
    Win32PortableHelperServiceBackend backend;
    PortableHelperRecoveryReport recovery;
    std::string error;
    const bool ok = scan_and_recover_portable_helper_instances(
        backend, &recovery, &error, [cancellation] {
            return cancellation == nullptr ||
                WaitForSingleObject(cancellation, 0) != WAIT_OBJECT_0;
        });
    if (!ok) {
        log_event(g_log_path, "portable_helper.recovery_failed",
                  {{"error", error}});
        return 1;
    }
    log_event(g_log_path, "portable_helper.recovery_summary",
              {{"inspected", std::to_string(recovery.inspected)},
               {"active", std::to_string(recovery.active)},
               {"cleaned", std::to_string(recovery.cleaned)},
               {"pending", std::to_string(recovery.pending)},
               {"invalid", std::to_string(recovery.invalid)},
               {"interrupted", recovery.interrupted ? "true" : "false"}});
    return 0;
}

void wait_for_helper_recovery() {
    if (g_helper_recovery_thread == nullptr) return;
    for (;;) {
        const DWORD waited = WaitForSingleObject(g_helper_recovery_thread, 5000);
        if (waited == WAIT_OBJECT_0) break;
        if (waited == WAIT_TIMEOUT) {
            report_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
            continue;
        }
        log_event(g_log_path, "portable_helper.recovery_wait_failed",
                  {{"error", std::to_string(GetLastError())}});
        report_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        Sleep(1000);
    }
    CloseHandle(g_helper_recovery_thread);
    g_helper_recovery_thread = nullptr;
}

DWORD WINAPI service_handler(DWORD control, DWORD event_type, void* event_data, void* context) {
    (void)context;
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            if (g_status.dwCurrentState == SERVICE_RUNNING) {
                report_status(SERVICE_STOP_PENDING, NO_ERROR, 60000);
                if (g_service_stop != nullptr) {
                    SetEvent(g_service_stop);
                }
            }
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            if (event_type == WTS_SESSION_LOGOFF && event_data) {
                std::lock_guard lock(g_logoff_mutex);
                g_logged_off_sessions.push_back(static_cast<WTSSESSION_NOTIFICATION*>(event_data)->dwSessionId);
            }
            if (g_session_wake != nullptr) {
                SetEvent(g_session_wake);
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            if (g_status_handle != nullptr) {
                SetServiceStatus(g_status_handle, &g_status);
            }
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void service_main(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, service_handler, nullptr);
    if (g_status_handle == nullptr) {
        return;
    }
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCheckPoint = 0;
    report_status(SERVICE_START_PENDING, NO_ERROR, 30000);
    std::string state_error;
    if (!harden_machine_state_directory(widen(machine_settings_directory()), &state_error) ||
        !harden_machine_state_file(widen(machine_settings_path()), &state_error)) {
        OutputDebugStringA(("chast-service: machine state security failed: " + state_error + "\n").c_str());
        report_status(SERVICE_STOPPED, ERROR_ACCESS_DENIED);
        return;
    }
    g_log_path = service_log_path("service.log");
    log_event(g_log_path, "service.start");

    HostSettings settings;
    std::string settings_path;
    std::string settings_warning;
    std::string settings_error;
    if (!load_machine_settings(settings, &settings_path, &settings_warning, &settings_error)) {
        log_event(g_log_path, "settings.error", {{"error", settings_error}});
    } else {
        log_event(g_log_path, "settings.loaded",
                  {{"path", settings_path}, {"port", std::to_string(settings.port)}});
    }
    if (!settings_warning.empty()) {
        log_event(g_log_path, "settings.warning", {{"text", settings_warning}});
    }

    int firewall_rollback = 0;
    const int firewall = apply_firewall_rule(settings.port, tunnel_port_of(settings),
                                             &firewall_rollback);
    if (firewall == 0) {
        log_event(g_log_path, "firewall.applied");
    } else {
        log_event(g_log_path, "firewall.failed",
                  {{"error", std::to_string(firewall)},
                   {"rollback_error", std::to_string(firewall_rollback)}});
    }

    g_service_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_session_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker_stop = CreateEventW(nullptr, TRUE, FALSE, kWorkerStopEvent);
    g_worker_switch = CreateEventW(nullptr, FALSE, FALSE, kWorkerSwitchEvent);
    // A request left by a crashed worker must not redirect this run's first
    // worker; the worker rewrites the file before every signal.
    (void)DeleteFileW(session_switch_path().c_str());
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            log_event(g_log_path, "job.configure_failed",
                      {{"error", std::to_string(GetLastError())}});
        }
    } else {
        log_event(g_log_path, "job.create_failed",
                  {{"error", std::to_string(GetLastError())}});
    }
    if (g_service_stop == nullptr || g_session_wake == nullptr || g_worker_stop == nullptr ||
        g_worker_switch == nullptr) {
        log_event(g_log_path, "events.create_failed",
                  {{"error", std::to_string(GetLastError())}});
        report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    const bool debug_privilege = enable_privilege(L"SeDebugPrivilege");
    (void)enable_privilege(L"SeAssignPrimaryTokenPrivilege");
    (void)enable_privilege(L"SeIncreaseQuotaPrivilege");
    log_event(g_log_path, "privilege.debug",
              {{"enabled", debug_privilege ? "true" : "false"}});

    report_status(SERVICE_RUNNING);
    log_event(g_log_path, "service.running");
    g_helper_recovery_thread = CreateThread(
        nullptr, 0, recover_portable_helpers, g_service_stop, 0, nullptr);
    if (g_helper_recovery_thread == nullptr) {
        log_event(g_log_path, "portable_helper.recovery_thread_failed",
                  {{"error", std::to_string(GetLastError())}});
    }

    ServiceManagerStartup managers(service_manager_executable());
    auto retry_at = std::chrono::steady_clock::now();
    std::string session_snapshot_error;
    for (;;) {
        if (WaitForSingleObject(g_service_stop, 0) == WAIT_OBJECT_0) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        std::vector<DWORD> logged_off;
        {
            std::lock_guard lock(g_logoff_mutex);
            logged_off.swap(g_logged_off_sessions);
        }
        for (auto ended : logged_off) {
            managers.logged_off(ended);
            if (ended == g_selected_session.id) g_selected_session = {};
            if (ended == g_worker_session) { stop_worker(); retry_at = now; }
        }
        std::vector<WindowsSession> sessions;
        std::string snapshot_error;
        const bool snapshot_ready = read_windows_sessions(sessions, &snapshot_error);
        if (!snapshot_ready && snapshot_error != session_snapshot_error)
            log_event(g_log_path, "session.snapshot_failed", {{"error", snapshot_error}});
        if (snapshot_ready && !session_snapshot_error.empty()) log_event(g_log_path, "session.snapshot_recovered");
        session_snapshot_error = snapshot_error;
        DWORD session = g_worker_session;
        uint64_t logon = g_worker_logon;
        if (snapshot_ready) {
            SessionSwitchRequest request;
            if (take_session_switch_request(request)) {
                const auto* requested = find_windows_session(sessions, request.session_id);
                if (!requested || requested->logon_id != request.logon_id || !g_worker_process ||
                    GetProcessId(g_worker_process) != request.worker_pid)
                    log_event(g_log_path, "session.switch_refused", {{"session", std::to_string(request.session_id)}});
                else {
                    g_selected_session = {request.session_id, requested->logon_id};
                    log_event(g_log_path, "session.switch_requested", {{"session", std::to_string(request.session_id)}});
                }
            }
            const auto previous_choice = g_selected_session.id;
            const auto target = choose_windows_session(sessions, WTSGetActiveConsoleSessionId(), g_worker_session, g_selected_session);
            if (previous_choice && !g_selected_session.id)
                log_event(g_log_path, "session.selected_lost", {{"session", std::to_string(previous_choice)}});
            session = target.id;
            logon = target.logon_id;
        }
        const bool logged_in = snapshot_ready && session != 0;
        if (logged_in) {
            const auto manager = managers.poll(session);
            if (manager.process_id) log_event(g_log_path, "manager.start",
                {{"session", std::to_string(session)}, {"pid", std::to_string(manager.process_id)}});
            if (!manager.error.empty()) log_event(g_log_path, "manager.start_failed",
                {{"session", std::to_string(session)}, {"error", manager.error}});
        }

        if (g_worker_process != nullptr) {
            if (snapshot_ready && (!logged_in || session != g_worker_session || logon != g_worker_logon)) {
                log_event(g_log_path, "session.changed",
                          {{"from", std::to_string(g_worker_session)},
                           {"to", std::to_string(session)}});
                stop_worker();
                retry_at = std::chrono::steady_clock::now();
            } else if (WaitForSingleObject(g_worker_process, 0) == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                GetExitCodeProcess(g_worker_process, &exit_code);
                log_event(g_log_path, "worker.exit",
                          {{"pid", std::to_string(GetProcessId(g_worker_process))},
                           {"code", std::to_string(exit_code)}});
                CloseHandle(g_worker_process);
                g_worker_process = nullptr;
                g_worker_session = 0;
                g_worker_logon = 0;
                retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }
        }

        if (g_worker_process == nullptr && logged_in &&
            std::chrono::steady_clock::now() >= retry_at) {
            std::string error;
            if (!start_worker(session, logon, &error)) {
                log_event(g_log_path, "worker.start_failed", {{"error", error}});
                retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
        }

        HANDLE waits[3] = {g_service_stop, g_session_wake, g_worker_switch};
        WaitForMultipleObjects(3, waits, FALSE, 1000);
    }

    wait_for_helper_recovery();
    stop_worker();
    if (g_job != nullptr) {
        CloseHandle(g_job);
        g_job = nullptr;
    }
    CloseHandle(g_service_stop);
    CloseHandle(g_session_wake);
    CloseHandle(g_worker_stop);
    CloseHandle(g_worker_switch);
    g_service_stop = nullptr;
    g_session_wake = nullptr;
    g_worker_stop = nullptr;
    g_worker_switch = nullptr;
    log_event(g_log_path, "service.stop");
    report_status(SERVICE_STOPPED);
}

}  // namespace

std::wstring session_switch_path() {
    const std::string settings = machine_settings_path();
    if (settings.empty()) {
        return {};
    }
    std::filesystem::path path(widen(settings));
    path.replace_filename(L"session-switch");
    return path.wstring();
}

ServicePresence service_presence() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return ServicePresence::Unknown;
    }
    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST || error == ERROR_SERVICE_MARKED_FOR_DELETE)
            return ServicePresence::Missing;
        return error == ERROR_ACCESS_DENIED ? ServicePresence::Installed : ServicePresence::Unknown;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ServicePresence::Installed;
}
bool service_installed() { return service_presence() == ServicePresence::Installed; }

ServiceState service_state() {
    DWORD state = 0;
    if (!query_service_state(&state)) return ServiceState::Unknown;
    switch (state) {
        case SERVICE_STOPPED: return ServiceState::Stopped;
        case SERVICE_START_PENDING: return ServiceState::Starting;
        case SERVICE_RUNNING: return ServiceState::Running;
        case SERVICE_STOP_PENDING: return ServiceState::Stopping;
        case SERVICE_PAUSED: return ServiceState::Paused;
        default: return ServiceState::Unknown;
    }
}

bool service_running() {
    DWORD state = SERVICE_STOPPED;
    if (!query_service_state(&state)) {
        return false;
    }
    return state == SERVICE_RUNNING || state == SERVICE_START_PENDING ||
           state == SERVICE_STOP_PENDING;
}

bool load_machine_settings(HostSettings& settings, std::string* path, std::string* warning,
                           std::string* error, bool persist_changes, bool* changed_out) {
    if (changed_out != nullptr) *changed_out = false;
    const std::string file = machine_settings_path();
    if (path != nullptr) {
        *path = file;
    }
    if (warning != nullptr) {
        warning->clear();
    }
    if (!file.empty()) {
        std::string security_error;
        if (!machine_state_directory_trusted(widen(machine_settings_directory()), &security_error) ||
            !machine_state_file_trusted(widen(file), &security_error)) {
            if (error != nullptr) {
                *error = "机器设置目录或文件不可信：" + security_error;
            }
            return false;
        }
    }
    bool changed = false;
    if (file.empty()) {
        settings = HostSettings{};
        changed = true;
    } else {
        const HostSettingsLoad loaded = load_host_settings(file);
        settings = loaded.settings;
        if (warning != nullptr) {
            *warning = loaded.warning;
        }
        changed = !loaded.existed;
    }
    fill_machine_identity(settings, &changed);
    if (changed_out != nullptr) *changed_out = changed;
    if (file.empty()) {
        if (error != nullptr) {
            *error = "无法确定 %ProgramData%\\chast 的位置";
        }
        return false;
    }
    if (!changed || !persist_changes) {
        return true;
    }
    std::string save_error;
    if (!save_host_settings(file, settings, &save_error)) {
        if (error != nullptr) {
            *error = "无法保存 " + file + "：" + save_error;
        }
        return false;
    }
    return true;
}

int install_service(std::string* error) {
    if (!is_elevated()) {
        *error = "安装服务需要管理员权限，请从提升的命令行运行 --install";
        return 1;
    }
    const auto manager_path = service_manager_executable();
    const auto manager_attributes = GetFileAttributesW(manager_path.c_str());
    if (manager_attributes == INVALID_FILE_ATTRIBUTES || (manager_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *error = "安装服务需要同目录下的 chast-host.exe，用于用户登录后的连接管理与审批";
        return 1;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        *error = "无法打开服务管理器（错误 " + std::to_string(GetLastError()) + "）";
        return 1;
    }

    SC_HANDLE service = nullptr;
    detail::FileSnapshot settings_snapshot;
    FirewallRuleUpdate firewall_update;
    detail::RollbackStack rollback;
    const auto close_handles = [&] {
        if (service != nullptr) {
            CloseServiceHandle(service);
            service = nullptr;
        }
        if (manager != nullptr) {
            CloseServiceHandle(manager);
            manager = nullptr;
        }
    };
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        const int rollback_error = rollback.rollback();
        if (error != nullptr && rollback_error != 0) {
            *error += "；回滚也失败（错误 " + std::to_string(rollback_error) + "）";
        }
        close_handles();
        return 1;
    };

    const std::wstring command = L"\"" + executable_path() + L"\" --run";
    DWORD initial_service_state = SERVICE_STOPPED;
    bool existing_service = false;
    bool service_config_changed = false;
    ServiceConfigSnapshot previous_config;
    service = CreateServiceW(
        manager, kServiceName, L"chast 主机服务", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr);
    if (service != nullptr) {
        rollback.push([service] { return delete_created_service(service); });
    } else {
        const DWORD created_error = GetLastError();
        if (created_error != ERROR_SERVICE_EXISTS) {
            return fail("创建服务失败（错误 " + std::to_string(created_error) + "）");
        }
        service = OpenServiceW(manager, kServiceName,
                               SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS |
                                   SERVICE_QUERY_CONFIG | SERVICE_STOP);
        if (service == nullptr) {
            return fail("无法打开现有服务（错误 " + std::to_string(GetLastError()) + "）");
        }
        existing_service = true;
        SERVICE_STATUS before_config{};
        if (!QueryServiceStatus(service, &before_config)) {
            return fail("无法读取现有服务状态（错误 " + std::to_string(GetLastError()) + "）");
        }
        initial_service_state = before_config.dwCurrentState;
        const DWORD config_error = previous_config.capture(service);
        if (config_error != ERROR_SUCCESS) {
            return fail("无法保存现有服务配置（错误 " +
                        std::to_string(config_error) + "）");
        }
        const DWORD runtime_config_error =
            previous_config.capture_runtime_configuration(service);
        if (runtime_config_error != ERROR_SUCCESS) {
            return fail("无法保存现有服务启动策略（错误 " +
                        std::to_string(runtime_config_error) + "）");
        }
        if (!detail::supported_existing_service_install_state(initial_service_state)) {
            return fail("现有服务处于启动、停止、暂停或其他过渡状态，拒绝更新");
        }
        service_config_changed = !previous_config.matches_target_configuration(command);
    }

    const std::string settings_file = machine_settings_path();
    if (settings_file.empty()) {
        return fail("无法确定 %ProgramData%\\chast 的位置");
    }
    std::string state_security_error;
    if (!harden_machine_state_directory(widen(machine_settings_directory()),
                                        &state_security_error) ||
        !harden_machine_state_file(widen(settings_file), &state_security_error)) {
        return fail("无法保护机器设置目录：" + state_security_error);
    }
    const DWORD settings_snapshot_error = settings_snapshot.capture(widen(settings_file));
    if (settings_snapshot_error != ERROR_SUCCESS) {
        return fail("无法读取现有机器配置（错误 " +
                    std::to_string(settings_snapshot_error) + "）");
    }

    HostSettings settings;
    bool settings_changed = false;
    if (!load_machine_settings(settings, nullptr, nullptr, error, false, &settings_changed)) {
        return fail(error == nullptr ? "无法准备机器配置" : *error);
    }

    if (existing_service) {
        const DWORD stop_error = detail::prepare_existing_service_for_update(
            initial_service_state, service_config_changed, settings_changed, rollback,
            [service] { return stop_service_if_running(service); },
            [service](DWORD state) { return restore_service_state(service, state); });
        if (stop_error != ERROR_SUCCESS) {
            return fail("停止现有服务以更新安装失败（错误 " +
                        std::to_string(stop_error) + "）");
        }
        rollback.push([&previous_config, service] {
            return static_cast<int>(previous_config.restore_runtime_configuration(service));
        });
        rollback.push([&previous_config, service] { return previous_config.restore(service); });
        if (service_config_changed &&
            !ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                  SERVICE_NO_CHANGE, command.c_str(), nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr)) {
            return fail("更新现有服务失败（错误 " + std::to_string(GetLastError()) + "）");
        }
    }

    if (settings_changed) {
        std::string save_error;
        const bool saved = detail::save_settings_with_rollback(
            rollback,
            [&] { return save_host_settings(settings_file, settings, &save_error); },
            [&settings_snapshot] { return settings_snapshot.restore(); });
        if (!saved) {
            return fail("无法保存 " + settings_file + "：" + save_error);
        }
    }

    int firewall_rollback = 0;
    const int firewall = firewall_update.apply(settings.port, tunnel_port_of(settings),
                                               &firewall_rollback);
    if (firewall != 0) {
        std::string message = firewall == ERROR_ALREADY_EXISTS
                                 ? "同名防火墙规则指向其他程序，已拒绝修改"
                                 : "防火墙规则操作失败（代码 " + std::to_string(firewall) + "）";
        if (firewall_rollback != 0) {
            message += "；规则回滚失败（代码 " + std::to_string(firewall_rollback) + "）";
        }
        return fail(std::move(message));
    }
    rollback.push([&firewall_update] { return firewall_update.rollback(); });

    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<LPWSTR>(kServiceDescription);
    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    delayed.fDelayedAutostart = TRUE;
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000},
                            {SC_ACTION_RESTART, 10000},
                            {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW failures{};
    failures.dwResetPeriod = 3600;
    failures.cActions = 3;
    failures.lpsaActions = actions;
    const DWORD runtime_config_apply = detail::apply_service_runtime_configuration(
        [service, &description] {
            return ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description)
                       ? ERROR_SUCCESS
                       : GetLastError();
        },
        [service, &delayed] {
            return ChangeServiceConfig2W(service,
                                        SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)
                       ? ERROR_SUCCESS
                       : GetLastError();
        },
        [service, &failures] {
            return ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failures)
                       ? ERROR_SUCCESS
                       : GetLastError();
        });
    if (runtime_config_apply != ERROR_SUCCESS) {
        return fail("应用服务启动策略失败（错误 " +
                    std::to_string(runtime_config_apply) + "）");
    }

    const bool start_service = !existing_service || initial_service_state != SERVICE_RUNNING ||
                               service_config_changed || settings_changed;
    if (start_service) {
        if (!StartServiceW(service, 0, nullptr)) {
            const DWORD started = GetLastError();
            if (started != ERROR_SERVICE_ALREADY_RUNNING) {
                return fail("服务已安装，但启动失败（错误 " + std::to_string(started) + "）");
            }
        }
        rollback.push([service] { return static_cast<int>(stop_service_if_running(service)); });
    }
    if (!wait_for_state(service, SERVICE_RUNNING, 15000)) {
        return fail("服务已安装，但没有在 15 秒内进入运行状态");
    }

    firewall_update.commit();
    rollback.commit();
    close_handles();
    return 0;
}

int uninstall_service(std::string* error) {
    if (!is_elevated()) {
        *error = "卸载服务需要管理员权限，请从提升的命令行运行 --uninstall";
        return 1;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        *error = "无法打开服务管理器（错误 " + std::to_string(GetLastError()) + "）";
        return 1;
    }
    SC_HANDLE service =
        OpenServiceW(manager, kServiceName,
                     SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_START |
                         SERVICE_PAUSE_CONTINUE | DELETE);
    if (service == nullptr) {
        const DWORD opened = GetLastError();
        CloseServiceHandle(manager);
        if (opened != ERROR_SERVICE_DOES_NOT_EXIST) {
            *error = "无法打开服务（错误 " + std::to_string(opened) + "）";
            return 1;
        }
        int rollback_error = 0;
        const int firewall_error = remove_firewall_rules(&rollback_error);
        if (firewall_error != 0) {
            *error = "服务不存在，但防火墙清理命令失败（错误 " +
                     std::to_string(firewall_error) + "）";
            if (rollback_error != 0) {
                *error += "；规则回滚也失败（错误 " + std::to_string(rollback_error) + "）";
            }
            return 1;
        }
        return 0;
    }

    SERVICE_STATUS previous_status{};
    if (!QueryServiceStatus(service, &previous_status)) {
        const DWORD query_error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        *error = "无法读取服务状态（错误 " + std::to_string(query_error) + "）";
        return 1;
    }

    FirewallRuleUpdate firewall_update;
    const auto close_handles = [&] {
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
    };
    const detail::ServiceUninstallResult result =
        detail::run_service_uninstall_transaction(
            previous_status.dwCurrentState,
            [service] { return stop_service_if_running(service); },
            [&firewall_update](int* rollback_error) {
                return firewall_update.clear(rollback_error);
            },
            [&firewall_update] { return firewall_update.rollback(); },
            [service]() -> DWORD {
                if (DeleteService(service)) return ERROR_SUCCESS;
                const DWORD error = GetLastError();
                return error == ERROR_SERVICE_MARKED_FOR_DELETE ? ERROR_SUCCESS : error;
            },
            [service](DWORD state) { return restore_service_state(service, state); });
    if (result.error != ERROR_SUCCESS) {
        switch (result.stage) {
            case detail::ServiceUninstallStage::Stop:
                *error = "停止服务失败，未继续删除服务或清理防火墙规则（错误 " +
                         std::to_string(result.error) + "）";
                break;
            case detail::ServiceUninstallStage::Firewall:
                *error = "防火墙清理失败（错误 " + std::to_string(result.error) + "）";
                if (result.firewall_rollback_error != 0) {
                    *error += "；防火墙回滚也失败（错误 " +
                              std::to_string(result.firewall_rollback_error) + "）";
                }
                break;
            case detail::ServiceUninstallStage::Delete:
                *error = "删除服务失败（错误 " + std::to_string(result.error) + "）";
                break;
            case detail::ServiceUninstallStage::Complete:
                *error = "卸载事务返回了不一致状态";
                break;
        }
        if (result.rollback_error != 0) {
            *error += "；恢复原服务/防火墙状态也失败（错误 " +
                      std::to_string(result.rollback_error) + "）";
        }
        close_handles();
        return 1;
    }
    firewall_update.commit();
    close_handles();
    return 0;
}

int run_service() {
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), service_main},
                                    {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::fprintf(stderr, "chast-service: --run must be started by the service manager\n");
        } else {
            std::fprintf(stderr, "chast-service: StartServiceCtrlDispatcher failed: %lu\n",
                         static_cast<unsigned long>(error));
        }
        return 1;
    }
    return 0;
}

}  // namespace chast::host
