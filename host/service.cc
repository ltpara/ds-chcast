#include "service.h"

#include <windows.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "chast/host_settings.h"
#include "platform.h"
#include "service_log.h"

namespace chast::host {

namespace {

constexpr wchar_t kServiceDescription[] =
    L"chast 远程桌面主机。服务在活动控制台会话内运行采集与输入，托盘不启动时也可注册到连接服务。";

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_service_stop = nullptr;
HANDLE g_session_wake = nullptr;
HANDLE g_worker_stop = nullptr;
HANDLE g_job = nullptr;
HANDLE g_worker_process = nullptr;
DWORD g_worker_session = 0;
std::string g_log_path;

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
        if (desired == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
            return false;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(200);
    }
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

bool session_has_user(DWORD session) {
    LPWSTR name = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &name,
                                     &bytes)) {
        return false;
    }
    const bool has_user = name != nullptr && name[0] != L'\0';
    WTSFreeMemory(name);
    return has_user;
}

std::wstring worker_command_line() {
    return L"\"" + executable_path() + L"\" --worker";
}

bool start_worker(DWORD session, std::string* error) {
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
        log_line(g_log_path, "cannot assign the worker to the service job: " +
                                 std::to_string(GetLastError()));
    }
    CloseHandle(info.hThread);
    g_worker_process = info.hProcess;
    g_worker_session = session;
    log_line(g_log_path, "worker started: pid=" + std::to_string(info.dwProcessId) +
                             " session=" + std::to_string(session));
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
        log_line(g_log_path, "worker did not stop in 20 s; terminating");
        TerminateProcess(g_worker_process, 1);
        WaitForSingleObject(g_worker_process, 2000);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(g_worker_process, &exit_code);
    log_line(g_log_path, "worker exited: pid=" + std::to_string(GetProcessId(g_worker_process)) +
                             " code=" + std::to_string(exit_code));
    CloseHandle(g_worker_process);
    g_worker_process = nullptr;
    g_worker_session = 0;
    if (g_worker_stop != nullptr) {
        ResetEvent(g_worker_stop);
    }
}

DWORD WINAPI service_handler(DWORD control, DWORD event_type, void* event_data, void* context) {
    (void)event_type;
    (void)event_data;
    (void)context;
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            if (g_status.dwCurrentState == SERVICE_RUNNING) {
                report_status(SERVICE_STOP_PENDING, NO_ERROR, 30000);
                if (g_service_stop != nullptr) {
                    SetEvent(g_service_stop);
                }
            }
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
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
    g_log_path = service_log_path("service.log");
    log_line(g_log_path, "service starting");

    HostSettings settings;
    std::string settings_path;
    std::string settings_warning;
    std::string settings_error;
    if (!load_machine_settings(settings, &settings_path, &settings_warning, &settings_error)) {
        log_line(g_log_path, "machine settings: " + settings_error);
    } else {
        log_line(g_log_path, "settings: " + settings_path + " port=" +
                                 std::to_string(settings.port));
    }
    if (!settings_warning.empty()) {
        log_line(g_log_path, "settings warning: " + settings_warning);
    }

    const int firewall = apply_firewall_rule(settings.port, tunnel_port_of(settings));
    log_line(g_log_path, firewall == 0 ? "firewall rules applied"
                                       : "firewall rules failed: " + std::to_string(firewall));

    g_service_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_session_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker_stop = CreateEventW(nullptr, TRUE, FALSE, kWorkerStopEvent);
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            log_line(g_log_path, "cannot configure the service job: " +
                                     std::to_string(GetLastError()));
        }
    } else {
        log_line(g_log_path, "cannot create the service job: " + std::to_string(GetLastError()));
    }
    if (g_service_stop == nullptr || g_session_wake == nullptr || g_worker_stop == nullptr) {
        log_line(g_log_path, "cannot create service events: " + std::to_string(GetLastError()));
        report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    const bool debug_privilege = enable_privilege(L"SeDebugPrivilege");
    (void)enable_privilege(L"SeAssignPrimaryTokenPrivilege");
    (void)enable_privilege(L"SeIncreaseQuotaPrivilege");
    log_line(g_log_path, debug_privilege ? "SeDebugPrivilege enabled"
                                         : "SeDebugPrivilege could not be enabled");

    report_status(SERVICE_RUNNING);
    log_line(g_log_path, "service running");

    auto retry_at = std::chrono::steady_clock::now();
    for (;;) {
        if (WaitForSingleObject(g_service_stop, 0) == WAIT_OBJECT_0) {
            break;
        }
        const DWORD session = WTSGetActiveConsoleSessionId();
        const bool logged_in = session != 0xFFFFFFFF && session_has_user(session);

        if (g_worker_process != nullptr) {
            if (!logged_in || session != g_worker_session) {
                log_line(g_log_path, "session changed; stopping the worker");
                stop_worker();
                retry_at = std::chrono::steady_clock::now();
            } else if (WaitForSingleObject(g_worker_process, 0) == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                GetExitCodeProcess(g_worker_process, &exit_code);
                log_line(g_log_path, "worker ended: code=" + std::to_string(exit_code));
                CloseHandle(g_worker_process);
                g_worker_process = nullptr;
                g_worker_session = 0;
                retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (g_worker_process == nullptr && logged_in && now >= retry_at) {
            std::string error;
            if (!start_worker(session, &error)) {
                log_line(g_log_path, error);
                retry_at = now + std::chrono::seconds(10);
            }
        }

        HANDLE waits[2] = {g_service_stop, g_session_wake};
        WaitForMultipleObjects(2, waits, FALSE, 1000);
    }

    stop_worker();
    if (g_job != nullptr) {
        CloseHandle(g_job);
        g_job = nullptr;
    }
    CloseHandle(g_service_stop);
    CloseHandle(g_session_wake);
    CloseHandle(g_worker_stop);
    g_service_stop = nullptr;
    g_session_wake = nullptr;
    g_worker_stop = nullptr;
    log_line(g_log_path, "service stopped");
    report_status(SERVICE_STOPPED);
}

}  // namespace

bool service_installed() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr) {
        const DWORD error = GetLastError();
        CloseServiceHandle(manager);
        return error == ERROR_ACCESS_DENIED;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return true;
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
                           std::string* error) {
    const std::string file = machine_settings_path();
    if (path != nullptr) {
        *path = file;
    }
    if (warning != nullptr) {
        warning->clear();
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
    if (file.empty()) {
        if (error != nullptr) {
            *error = "无法确定 %ProgramData%\\chast 的位置";
        }
        return false;
    }
    if (!changed) {
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
    HostSettings settings;
    if (!load_machine_settings(settings, nullptr, nullptr, error)) {
        return 1;
    }

    const int firewall = apply_firewall_rule(settings.port, tunnel_port_of(settings));
    if (firewall != 0) {
        *error = "防火墙规则添加失败（netsh 退出码 " + std::to_string(firewall) + "）";
        return 1;
    }

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        *error = "无法打开服务管理器（错误 " + std::to_string(GetLastError()) + "）";
        return 1;
    }
    const std::wstring command = L"\"" + executable_path() + L"\" --run";
    SC_HANDLE service = CreateServiceW(
        manager, kServiceName, L"chast 主机服务", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
        nullptr, nullptr);
    if (service == nullptr) {
        const DWORD created_error = GetLastError();
        if (created_error != ERROR_SERVICE_EXISTS) {
            *error = "创建服务失败（错误 " + std::to_string(created_error) + "）";
            CloseServiceHandle(manager);
            return 1;
        }
        service = OpenServiceW(manager, kServiceName,
                               SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS);
        if (service == nullptr ||
            !ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                  SERVICE_NO_CHANGE, command.c_str(), nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr)) {
            *error = "更新现有服务失败（错误 " + std::to_string(GetLastError()) + "）";
            if (service != nullptr) {
                CloseServiceHandle(service);
            }
            CloseServiceHandle(manager);
            return 1;
        }
    }
    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<LPWSTR>(kServiceDescription);
    (void)ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    delayed.fDelayedAutostart = TRUE;
    (void)ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000},
                            {SC_ACTION_RESTART, 10000},
                            {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW failures{};
    failures.dwResetPeriod = 3600;
    failures.cActions = 3;
    failures.lpsaActions = actions;
    (void)ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failures);

    std::string ignored;
    (void)set_autostart(false, &ignored);

    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD started = GetLastError();
        if (started != ERROR_SERVICE_ALREADY_RUNNING) {
            *error = "服务已安装，但启动失败（错误 " + std::to_string(started) + "）";
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 1;
        }
    }
    if (!wait_for_state(service, SERVICE_RUNNING, 15000)) {
        *error = "服务已安装，但没有在 15 秒内进入运行状态";
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return 1;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
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
        OpenServiceW(manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service == nullptr) {
        const DWORD opened = GetLastError();
        CloseServiceHandle(manager);
        if (opened != ERROR_SERVICE_DOES_NOT_EXIST) {
            *error = "无法打开服务（错误 " + std::to_string(opened) + "）";
            return 1;
        }
        remove_firewall_rules();
        return 0;
    }
    SERVICE_STATUS status{};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status) ||
        GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
        (void)wait_for_state(service, SERVICE_STOPPED, 30000);
    }
    if (!DeleteService(service)) {
        const DWORD deleted = GetLastError();
        if (deleted != ERROR_SERVICE_MARKED_FOR_DELETE) {
            *error = "删除服务失败（错误 " + std::to_string(deleted) + "）";
            CloseServiceHandle(service);
            CloseServiceHandle(manager);
            return 1;
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    remove_firewall_rules();
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
