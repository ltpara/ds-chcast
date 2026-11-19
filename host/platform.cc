#include "platform.h"

#include <netfw.h>
#include <oleauto.h>
#include <shellapi.h>

#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include "firewall_transaction.h"
#include "firewall_com_rule.h"
#include "tunnel.h"
#include "service_manager_startup.h"

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

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    ~ComPtr() {
        reset();
    }

    T** put() {
        reset();
        return &value_;
    }

    T* get() const { return value_; }
    T* operator->() const { return value_; }
    T* detach() { return std::exchange(value_, nullptr); }
    void reset(T* value = nullptr) {
        if (value_ != nullptr) value_->Release();
        value_ = value;
    }

private:
    T* value_ = nullptr;
};

int firewall_hresult_error(HRESULT result) {
    if (HRESULT_FACILITY(result) == FACILITY_WIN32) {
        return static_cast<int>(HRESULT_CODE(result));
    }
    if (result == E_ACCESSDENIED) return ERROR_ACCESS_DENIED;
    if (result == E_OUTOFMEMORY) return ERROR_NOT_ENOUGH_MEMORY;
    return ERROR_GEN_FAILURE;
}

class FirewallApi final : public detail::FirewallRuleBackend {
public:
    FirewallApi() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitialize_ = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
            error_ = firewall_hresult_error(initialized);
            return;
        }
        HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(INetFwPolicy2),
                                      reinterpret_cast<void**>(policy_.put()));
        if (SUCCEEDED(hr)) {
            hr = policy_->get_Rules(rules_.put());
        }
        if (FAILED(hr)) error_ = firewall_hresult_error(hr);
    }

    ~FirewallApi() override {
        rules_.reset();
        policy_.reset();
        if (uninitialize_) CoUninitialize();
    }

    int inspect(const detail::FirewallRuleIdentity& identity,
                detail::FirewallRuleSnapshot& snapshot) override {
        snapshot = {};
        ComPtr<INetFwRule> rule;
        bool present = false;
        const int error = lookup(identity.name, present, rule);
        if (error != 0 || !present) return error;
        const int owner_error = verify_owner(identity, rule.get());
        if (owner_error != 0) return owner_error;
        snapshot.exists = true;
        snapshot.native = std::shared_ptr<void>(
            rule.detach(), [](void* value) { static_cast<INetFwRule*>(value)->Release(); });
        return 0;
    }

    int remove(const detail::FirewallRuleIdentity& identity) override {
        ComPtr<INetFwRule> rule;
        bool present = false;
        const int error = lookup(identity.name, present, rule);
        if (error != 0 || !present) return error;
        const int owner_error = verify_owner(identity, rule.get());
        if (owner_error != 0) return owner_error;
        BSTR name = SysAllocString(identity.name.c_str());
        if (name == nullptr) return ERROR_NOT_ENOUGH_MEMORY;
        const HRESULT hr = rules_->Remove(name);
        SysFreeString(name);
        return FAILED(hr) ? firewall_hresult_error(hr) : 0;
    }

    int add(const detail::FirewallRuleSpec& rule) override {
        if (error_ != 0) return error_;
        ComPtr<INetFwRule> native_rule;
        HRESULT hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(INetFwRule),
                                      reinterpret_cast<void**>(native_rule.put()));
        if (FAILED(hr)) return firewall_hresult_error(hr);
        hr = detail::configure_firewall_com_rule(native_rule.get(), rule);
        if (FAILED(hr)) return firewall_hresult_error(hr);
        hr = rules_->Add(native_rule.get());
        return FAILED(hr) ? firewall_hresult_error(hr) : 0;
    }

    int restore(const detail::FirewallRuleIdentity& identity,
                const detail::FirewallRuleSnapshot& snapshot) override {
        const int removed = remove(identity);
        if (removed != 0 || !snapshot.exists) return removed;
        if (!snapshot.native) return ERROR_INVALID_DATA;
        const HRESULT hr = rules_->Add(static_cast<INetFwRule*>(snapshot.native.get()));
        return FAILED(hr) ? firewall_hresult_error(hr) : 0;
    }

private:
    int lookup(const std::wstring& name, bool& present, ComPtr<INetFwRule>& rule) {
        present = false;
        if (error_ != 0) return error_;
        BSTR key = SysAllocString(name.c_str());
        if (key == nullptr) return ERROR_NOT_ENOUGH_MEMORY;
        const HRESULT hr = rules_->Item(key, rule.put());
        SysFreeString(key);
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return 0;
        if (FAILED(hr)) return firewall_hresult_error(hr);
        present = true;
        return 0;
    }

    int verify_owner(const detail::FirewallRuleIdentity& identity, INetFwRule* rule) {
        BSTR application = nullptr;
        const HRESULT hr = rule->get_ApplicationName(&application);
        if (FAILED(hr)) {
            SysFreeString(application);
            return firewall_hresult_error(hr);
        }
        const std::wstring actual = application == nullptr
                                        ? std::wstring{}
                                        : std::wstring(application, SysStringLen(application));
        SysFreeString(application);
        for (const std::wstring& permitted : identity.permitted_applications) {
            if (firewall_rule_application_matches(actual, permitted)) return 0;
        }
        return ERROR_ALREADY_EXISTS;
    }

    int error_ = 0;
    bool uninitialize_ = false;
    ComPtr<INetFwPolicy2> policy_;
    ComPtr<INetFwRules> rules_;
};

std::vector<detail::FirewallRuleIdentity> managed_firewall_rules() {
    const std::wstring host_program = executable_path();
    const std::wstring host_manager_program = service_manager_executable();
    const std::wstring tunnel_program = tunnel_executable();
    const std::vector<std::wstring> host_programs = {host_program, host_manager_program};
    return {
        {kHostRuleName, host_programs},
        {kHostTcpRuleName, host_programs},
        {kTunnelRuleName, {tunnel_program}},
        {kTunnelDiscoveryRuleName, {tunnel_program}},
    };
}

void make_firewall_rule_plan(uint16_t port, uint16_t tunnel_port,
                             std::vector<detail::FirewallRuleIdentity>& managed,
                             std::vector<detail::FirewallRuleSpec>& desired) {
    const std::wstring host_program = executable_path();
    const std::wstring tunnel_program = tunnel_executable();
    managed = managed_firewall_rules();
    desired = {
        {kHostRuleName, host_program, L"UDP", port},
    };
    if (port < 65535) {
        desired.push_back({kHostTcpRuleName, host_program, L"TCP",
                           static_cast<uint16_t>(port + 1)});
    }
    if (tunnel_port != 0) {
        desired.push_back({kTunnelRuleName, tunnel_program, L"UDP", tunnel_port});
        desired.push_back({kTunnelDiscoveryRuleName, tunnel_program, L"UDP",
                           kTunnelDiscoveryPort});
    }
}

}  // namespace

struct FirewallRuleUpdate::Impl {
    FirewallApi backend;
    detail::FirewallRuleTransaction transaction;

    Impl() : transaction(backend) {}
};

FirewallRuleUpdate::FirewallRuleUpdate() : impl_(std::make_unique<Impl>()) {}
FirewallRuleUpdate::FirewallRuleUpdate(FirewallRuleUpdate&&) noexcept = default;
FirewallRuleUpdate& FirewallRuleUpdate::operator=(FirewallRuleUpdate&&) noexcept = default;
FirewallRuleUpdate::~FirewallRuleUpdate() = default;

int FirewallRuleUpdate::apply(uint16_t port, uint16_t tunnel_port, int* rollback_error) {
    if (rollback_error != nullptr) *rollback_error = 0;
    if (!impl_) return ERROR_INVALID_STATE;
    std::vector<detail::FirewallRuleIdentity> managed;
    std::vector<detail::FirewallRuleSpec> desired;
    make_firewall_rule_plan(port, tunnel_port, managed, desired);
    const detail::FirewallTransactionResult result =
        impl_->transaction.apply(managed, desired);
    if (rollback_error != nullptr) *rollback_error = result.rollback_error;
    return result.error;
}

int FirewallRuleUpdate::clear(int* rollback_error) {
    if (rollback_error != nullptr) *rollback_error = 0;
    if (!impl_) return ERROR_INVALID_STATE;
    const std::vector<detail::FirewallRuleIdentity> managed = managed_firewall_rules();
    const detail::FirewallTransactionResult result = impl_->transaction.apply(managed, {});
    if (rollback_error != nullptr) *rollback_error = result.rollback_error;
    return result.error;
}

int FirewallRuleUpdate::rollback() {
    return impl_ ? impl_->transaction.rollback() : ERROR_INVALID_STATE;
}

void FirewallRuleUpdate::commit() {
    if (impl_) impl_->transaction.commit();
}

bool run_hidden_command(const std::wstring& command, DWORD timeout_ms, DWORD* exit_code,
                       HANDLE* process_handle) {
    if (exit_code != nullptr) {
        *exit_code = 1;
    }
    if (process_handle != nullptr) {
        *process_handle = nullptr;
    }
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
    const auto release_handles = [&] {
        CloseHandle(process.hThread);
        if (process_handle != nullptr) {
            *process_handle = process.hProcess;
        } else {
            CloseHandle(process.hProcess);
        }
    };
    const DWORD waited = WaitForSingleObject(process.hProcess, timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        if (waited == WAIT_TIMEOUT || waited == WAIT_FAILED) {
            const BOOL terminated = TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            const DWORD terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
            const DWORD reaped =
                WaitForSingleObject(process.hProcess, terminated ? INFINITE : 5000);
            if (reaped != WAIT_OBJECT_0) {
                if (reaped == WAIT_FAILED) {
                    error = GetLastError();
                } else if (terminate_error != ERROR_SUCCESS) {
                    error = terminate_error;
                } else {
                    error = ERROR_TIMEOUT;
                }
            }
        } else {
            error = ERROR_GEN_FAILURE;
        }
        release_handles();
        SetLastError(error);
        return false;
    }
    DWORD code = 1;
    if (!GetExitCodeProcess(process.hProcess, &code)) {
        const DWORD error = GetLastError();
        release_handles();
        SetLastError(error);
        return false;
    }
    release_handles();
    if (exit_code != nullptr) {
        *exit_code = code;
    }
    return true;
}

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

bool firewall_rule_application_matches(const std::wstring& actual,
                                       const std::wstring& expected,
                                       const std::wstring& alternate) {
    if (actual.empty()) return false;
    const auto matches = [&actual](const std::wstring& path) {
        return !path.empty() &&
               CompareStringOrdinal(actual.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
    };
    return matches(expected) || matches(alternate);
}

bool set_autostart(bool enabled, std::string* error) {
    HKEY key = nullptr;
    HKEY user = nullptr;
    LSTATUS status = RegOpenCurrentUser(KEY_WRITE, &user);
    if (status == ERROR_SUCCESS) {
        status = RegCreateKeyExW(user, kRunKey, 0, nullptr, 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
        RegCloseKey(user);
    }
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

int apply_firewall_rule(uint16_t port, uint16_t tunnel_port, int* rollback_error) {
    FirewallRuleUpdate update;
    const int error = update.apply(port, tunnel_port, rollback_error);
    if (error == 0) update.commit();
    return error;
}

int remove_firewall_rules(int* rollback_error) {
    FirewallRuleUpdate update;
    const int error = update.clear(rollback_error);
    if (error == 0) update.commit();
    return error;
}

int run_elevated(const std::wstring& arguments, DWORD timeout_ms, bool* cancelled,
                 std::string* error) {
    if (cancelled != nullptr) {
        *cancelled = false;
    }
    const std::wstring exe = executable_path();
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
            if (cancelled != nullptr) {
                *cancelled = true;
            }
            if (error != nullptr) {
                *error = "已取消管理员授权";
            }
        } else if (error != nullptr) {
            *error = "无法请求管理员权限（错误 " + std::to_string(code) + "）";
        }
        return -1;
    }
    const DWORD waited = WaitForSingleObject(info.hProcess, timeout_ms == 0 ? 60000 : timeout_ms);
    if (waited == WAIT_TIMEOUT) {
        if (error != nullptr) {
            *error = "等待管理员操作超时";
        }
        CloseHandle(info.hProcess);
        return -1;
    }
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

bool request_firewall_rule(uint16_t port, uint16_t tunnel_port, std::string* error) {
    std::wstring arguments = L"--firewall " + std::to_wstring(port);
    if (tunnel_port != 0) {
        arguments += L" " + std::to_wstring(tunnel_port);
    }
    bool cancelled = false;
    std::string failure;
    const int code = run_elevated(arguments, 60000, &cancelled, &failure);
    if (code < 0) {
        *error = cancelled ? "已取消管理员授权，防火墙规则没有改变" : failure;
        return false;
    }
    if (code != 0) {
        *error = code == ERROR_ALREADY_EXISTS
                     ? "同名防火墙规则指向其他程序，已拒绝修改"
                     : "防火墙规则操作失败（代码 " + std::to_string(code) + "）";
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
