#include "platform.h"

#include <shellapi.h>

#include <cstring>
#include <vector>

#include "tunnel.h"

namespace chast::host {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"chast-host";
constexpr wchar_t kHostRuleName[] = L"chast host";
constexpr wchar_t kHostTcpRuleName[] = L"chast host tcp";
constexpr wchar_t kTunnelRuleName[] = L"chast tunnel";
constexpr wchar_t kTunnelDiscoveryRuleName[] = L"chast tunnel discovery";
// Keep in sync with lan.DefaultPort in server/internal/lan. The host agent
// answers discovery on this fixed port, so the rule needs no settings field.
constexpr uint16_t kTunnelDiscoveryPort = 21129;

bool run_hidden(const std::wstring& command, DWORD* exit_code) {
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    *exit_code = code;
    return true;
}

}  // namespace

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

bool set_autostart(bool enabled, std::string* error) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                     KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        *error = "无法打开启动项注册表（错误 " + std::to_string(status) + "）";
        return false;
    }
    if (enabled) {
        const std::wstring command = L"\"" + executable_path() + L"\" --tray";
        status = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        *error = std::string(enabled ? "无法写入" : "无法删除") + "开机启动项（错误 " +
                 std::to_string(status) + "）";
        return false;
    }
    return true;
}

int add_firewall_rule(const wchar_t* name, const std::wstring& executable, uint16_t port,
                      const wchar_t* protocol) {
    const std::wstring netsh = L"netsh.exe";
    // A rule with the same name is replaced rather than duplicated, and the
    // program scope keeps every other binary out.
    DWORD ignored = 0;
    run_hidden(netsh + L" advfirewall firewall delete rule name=\"" + name + L"\"", &ignored);
    const std::wstring add =
        netsh + L" advfirewall firewall add rule name=\"" + name +
        L"\" dir=in action=allow program=\"" + executable +
        L"\" protocol=" + protocol + L" localport=" + std::to_wstring(port) +
        L" profile=any enable=yes";
    DWORD code = 1;
    if (!run_hidden(add, &code)) {
        return static_cast<int>(GetLastError());
    }
    return static_cast<int>(code);
}

int apply_firewall_rule(uint16_t port, uint16_t tunnel_port) {
    // Drop the tunnel and TCP rules first so disabling them does not leave a
    // stale allowance behind.
    DWORD ignored = 0;
    run_hidden(L"netsh.exe advfirewall firewall delete rule name=\"" +
                   std::wstring(kTunnelRuleName) + L"\"",
               &ignored);
    run_hidden(L"netsh.exe advfirewall firewall delete rule name=\"" +
                   std::wstring(kTunnelDiscoveryRuleName) + L"\"",
               &ignored);
    run_hidden(L"netsh.exe advfirewall firewall delete rule name=\"" +
                   std::wstring(kHostTcpRuleName) + L"\"",
               &ignored);
    const int host_code = add_firewall_rule(kHostRuleName, executable_path(), port, L"UDP");
    if (host_code != 0) {
        return host_code;
    }
    // The TCP/WebSocket fallback listens on the next port; port 65535 has no
    // adjacent port and the host cannot start it there either.
    if (port < 65535) {
        const int carrier_code = add_firewall_rule(kHostTcpRuleName, executable_path(),
                                                   static_cast<uint16_t>(port + 1), L"TCP");
        if (carrier_code != 0) {
            return carrier_code;
        }
    }
    if (tunnel_port == 0) {
        return 0;
    }
    const int tunnel_code =
        add_firewall_rule(kTunnelRuleName, tunnel_executable(), tunnel_port, L"UDP");
    if (tunnel_code != 0) {
        return tunnel_code;
    }
    return add_firewall_rule(kTunnelDiscoveryRuleName, tunnel_executable(),
                             kTunnelDiscoveryPort, L"UDP");
}

void remove_firewall_rules() {
    DWORD ignored = 0;
    run_hidden(L"netsh.exe advfirewall firewall delete rule name=\"" +
                   std::wstring(kHostRuleName) + L"\"",
               &ignored);
    run_hidden(L"netsh.exe advfirewall firewall delete rule name=\"" +
                   std::wstring(kHostTcpRuleName) + L"\"",
               &ignored);
    run_hidden(L"netsh.exe advfirewall firewall delete rule name=\"" +
                   std::wstring(kTunnelRuleName) + L"\"",
               &ignored);
    run_hidden(L"netsh.exe advfirewall firewall delete rule name=\"" +
                   std::wstring(kTunnelDiscoveryRuleName) + L"\"",
               &ignored);
}

bool request_firewall_rule(uint16_t port, uint16_t tunnel_port, std::string* error) {
    const std::wstring exe = executable_path();
    std::wstring arguments = L"--firewall " + std::to_wstring(port);
    if (tunnel_port != 0) {
        arguments += L" " + std::to_wstring(tunnel_port);
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        if (code == ERROR_CANCELLED) {
            *error = "已取消管理员授权，防火墙规则没有改变";
        } else {
            *error = "无法请求管理员权限（错误 " + std::to_string(code) + "）";
        }
        return false;
    }
    WaitForSingleObject(info.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    if (code != 0) {
        *error = "防火墙规则添加失败（退出码 " + std::to_string(code) + "）";
        return false;
    }
    return true;
}

void copy_text(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* target = GlobalLock(memory);
        if (target != nullptr) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

std::wstring executable_path() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::wstring(buffer, buffer + size);
}

}  // namespace chast::host
