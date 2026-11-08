#define NOMINMAX
#include "service_manager_startup.h"
#include "platform.h"
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <chrono>
#include <filesystem>
#include <map>
#include <vector>

namespace chast::host {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::wstring token_logon(HANDLE token) {
    DWORD bytes = 0;
    GetTokenInformation(token, TokenLogonSid, nullptr, 0, &bytes);
    if (!bytes) return {};
    std::vector<BYTE> data(bytes);
    if (!GetTokenInformation(token, TokenLogonSid, data.data(), bytes, &bytes)) return {};
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(data.data());
    if (groups->GroupCount != 1) return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(groups->Groups[0].Sid, &sid)) return {};
    const std::wstring result(sid);
    LocalFree(sid);
    return result;
}
bool interactive_token(HANDLE token, DWORD session) {
    DWORD token_session = 0, bytes = 0;
    if (!GetTokenInformation(token, TokenSessionId, &token_session, sizeof(token_session), &bytes) ||
        token_session != session) return false;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> user(bytes);
    if (user.empty() || !GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes)) return false;
    const auto sid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
    return !IsWellKnownSid(sid, WinLocalSystemSid) && !IsWellKnownSid(sid, WinLocalServiceSid) &&
           !IsWellKnownSid(sid, WinNetworkServiceSid);
}
}

struct ServiceManagerStartup::Impl {
    struct Session {
        std::wstring logon;
        HANDLE process = nullptr;
        bool exited = false;
        std::chrono::steady_clock::time_point retry{};
        ~Session() { if (process) CloseHandle(process); }
    };
    std::wstring executable;
    std::map<DWORD, Session> sessions;
    explicit Impl(std::wstring path) : executable(std::move(path)) {}

    ServiceManagerStartResult poll(DWORD session) {
        if (!session || session == 0xffffffff) return {};
        auto& state = sessions[session];
        const auto now = std::chrono::steady_clock::now();
        if (now < state.retry) return {};
        const auto fail = [&](const char* operation, DWORD code) {
            state.retry = now + std::chrono::seconds(10);
            return ServiceManagerStartResult{0, std::string(operation) + " (error " + std::to_string(code) + ")"};
        };
        Handle user;
        if (!WTSQueryUserToken(session, &user.value)) return fail("Cannot read the interactive user's token", GetLastError());
        auto logon = token_logon(user.value);
        if (logon.empty() || !interactive_token(user.value, session)) return fail("Invalid interactive user identity", ERROR_ACCESS_DENIED);
        if (state.logon != logon) {
            if (state.process) { CloseHandle(state.process); state.process = nullptr; }
            state.exited = false;
            state.logon = logon;
        }
        if (state.exited) return {};
        if (state.process) {
            const auto waited = WaitForSingleObject(state.process, 0);
            if (waited == WAIT_TIMEOUT) return {};
            DWORD code = 1;
            if (waited == WAIT_FAILED || !GetExitCodeProcess(state.process, &code))
                return fail("Cannot read the service manager process state", GetLastError());
            CloseHandle(state.process); state.process = nullptr;
            if (!code) { state.exited = true; return {}; }
            return fail("Service manager exited unexpectedly", code);
        }

        // An administrator's interactive token may have a linked filtered
        // token. Use that token, keeping the same session and logon identity.
        TOKEN_ELEVATION_TYPE elevation{}; DWORD bytes = 0;
        if (!GetTokenInformation(user.value, TokenElevationType, &elevation, sizeof(elevation), &bytes))
            return fail("Cannot read interactive token elevation", GetLastError());
        Handle filtered;
        HANDLE launch_token = user.value;
        if (elevation == TokenElevationTypeFull) {
            TOKEN_LINKED_TOKEN linked{};
            if (!GetTokenInformation(user.value, TokenLinkedToken, &linked, sizeof(linked), &bytes))
                return fail("Cannot obtain the interactive user's filtered token", GetLastError());
            filtered.value = linked.LinkedToken;
            if (!interactive_token(filtered.value, session) || token_logon(filtered.value) != logon)
                return fail("Filtered token does not match the interactive logon", ERROR_ACCESS_DENIED);
            launch_token = filtered.value;
        }
        LPVOID environment = nullptr;
        if (!CreateEnvironmentBlock(&environment, launch_token, FALSE))
            return fail("Cannot create the interactive user's environment", GetLastError());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        auto command = L"\"" + executable + L"\" --service-manager --tray";
        const auto directory = std::filesystem::path(executable).parent_path().wstring();
        const BOOL started = CreateProcessAsUserW(launch_token, executable.c_str(), command.data(), nullptr,
            nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment, directory.c_str(), &startup, &process);
        const auto error = GetLastError();
        DestroyEnvironmentBlock(environment);
        if (!started) return fail("Cannot start the service manager", error);
        CloseHandle(process.hThread);
        state.process = process.hProcess;
        return {process.dwProcessId, {}};
    }
};

ServiceManagerStartup::ServiceManagerStartup(std::wstring executable)
    : impl_(std::make_unique<Impl>(std::move(executable))) {}
ServiceManagerStartup::~ServiceManagerStartup() = default;
ServiceManagerStartResult ServiceManagerStartup::poll(DWORD session) { return impl_->poll(session); }
void ServiceManagerStartup::logged_off(DWORD session) { impl_->sessions.erase(session); }
std::wstring service_manager_executable() {
    return (std::filesystem::path(executable_path()).parent_path() / L"chast-host.exe").wstring();
}
}
