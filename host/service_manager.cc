#define NOMINMAX
#define UNICODE
#define _UNICODE
#include "service_manager.h"
#include "service_control.h"
#include "connections.h"
#include "i18n.h"
#include "platform.h"
#include "service.h"
#include "chast/identity.h"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <map>

namespace chast::host {
namespace {
constexpr wchar_t kWindow[] = L"ChastServiceManager";
constexpr wchar_t kPrompt[] = L"ChastServiceApproval";
constexpr UINT kTray = WM_APP + 71;
constexpr int kOpen = 7100, kExit = 7101, kSecurity = 7102, kNetwork = 7103, kCopyId = 7104, kCopyFingerprint = 7105,
    kAllow = 7200, kDeny = 7201, kPermission = 7300;
const std::array<std::pair<PermissionMask, const wchar_t*>, 15> permissions{{
    {kPermissionInput, L"键盘与鼠标"}, {kPermissionClipboard, L"剪贴板"},
    {kPermissionAudio, L"系统声音"}, {kPermissionFileTransfer, L"文件传输"},
    {kPermissionMicrophone, L"麦克风（上行）"}, {kPermissionPrivacyMode, L"隐私模式"},
    {kPermissionRestart, L"远程重启"}, {kPermissionBlockInput, L"阻止本地输入"},
    {kPermissionOsLogin, L"OS 登录"}, {kPermissionSessionSelect, L"会话切换"},
    {kPermissionRemoteConfig, L"远程配置"}, {kPermissionTerminal, L"远程终端"},
    {kPermissionPortForward, L"端口转发"}, {kPermissionCamera, L"摄像头"},
    {kPermissionRecord, L"会话录制"},
}};
const wchar_t* mode_name(SessionMode mode) {
    switch (mode) {
        case SessionMode::FileTransfer: return tr(L"文件传输");
        case SessionMode::Terminal: return tr(L"远程终端");
        case SessionMode::Camera: return tr(L"摄像头");
        case SessionMode::PortForward: return tr(L"端口转发");
        default: return tr(L"桌面");
    }
}
struct Manager;
struct Prompt {
    Manager* manager = nullptr;
    ServiceApproval request;
    HWND window = nullptr;
    std::array<HWND, permissions.size()> checks{};
};
struct Manager {
    HINSTANCE instance;
    HWND window = nullptr, status = nullptr, device_id = nullptr, fingerprint = nullptr, authentication = nullptr;
    HANDLE editor = nullptr;
    HFONT font = nullptr;
    int dpi = 96;
    bool tray = false;
    bool service_seen = false;
    ULONGLONG service_probe_at = 0;
    ServiceState service_state_snapshot = ServiceState::Unknown;
    DWORD windows_session = 0;
    ServiceControlClient client;
    std::unique_ptr<ConnectionsWindow> connections;
    std::map<uint64_t, std::unique_ptr<Prompt>> prompts;
    int px(int v) const { return MulDiv(v, dpi, 96); }
    HWND add(HWND owner, const wchar_t* type, const wchar_t* label, DWORD style,
             int x, int y, int width, int height, int id = 0) {
        HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style,
            px(x), px(y), px(width), px(height), owner, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }
    static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* prompt = reinterpret_cast<Prompt*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            prompt = static_cast<Prompt*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(prompt)); prompt->window = hwnd;
        }
        if (!prompt) return DefWindowProcW(hwnd, message, wp, lp);
        if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wp) == kAllow || LOWORD(wp) == kDeny))) {
            const bool allow = message == WM_COMMAND && LOWORD(wp) == kAllow;
            PermissionMask mask = 0;
            for (size_t i = 0; i < permissions.size(); ++i)
                if (SendMessageW(prompt->checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED) mask |= permissions[i].first;
            prompt->manager->client.answer(prompt->request.id, allow, mask & prompt->request.permissions);
            DestroyWindow(hwnd);
            return 0;
        }
        if (message == WM_NCDESTROY) prompt->window = nullptr;
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    void create_prompt(const ServiceApproval& request) {
        auto prompt = std::make_unique<Prompt>(); prompt->manager = this; prompt->request = request;
        RECT rect{0, 0, px(640), px(365)};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectEx(&rect, style, FALSE, WS_EX_TOPMOST);
        const HWND dialog = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kPrompt,
            tr(L"chast 服务连接请求"), style, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
            rect.bottom - rect.top, window, nullptr, instance, prompt.get());
        if (!dialog) { client.answer(request.id, false, 0); prompts[request.id] = std::move(prompt); return; }
        const auto description = tr(L"来源：") + widen(request.peer_address) + L"\r\n" +
            tr(L"设备：") + widen(request.device_name) + L" · " + mode_name(request.mode);
        add(dialog, L"STATIC", description.c_str(), SS_LEFT, 20, 16, 600, 50);
        add(dialog, L"STATIC", request.device_proof ? tr(L"设备签名验证通过，允许后将记入可信设备。") :
            tr(L"请确认来源，并选择这次连接允许的权限。"), SS_LEFT, 20, 76, 600, 38);
        for (size_t i = 0; i < permissions.size(); ++i) {
            prompt->checks[i] = add(dialog, L"BUTTON", tr(permissions[i].second), BS_AUTOCHECKBOX | WS_TABSTOP,
                20 + static_cast<int>(i % 3) * 200, 120 + static_cast<int>(i / 3) * 29, 198, 26,
                kPermission + static_cast<int>(i));
            const bool granted = (request.permissions & permissions[i].first) != 0;
            SendMessageW(prompt->checks[i], BM_SETCHECK, granted ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(prompt->checks[i], granted);
        }
        add(dialog, L"BUTTON", tr(L"允许"), BS_PUSHBUTTON | WS_TABSTOP, 400, 298, 100, 34, kAllow);
        HWND deny = add(dialog, L"BUTTON", tr(L"拒绝"), BS_DEFPUSHBUTTON | WS_TABSTOP, 520, 298, 100, 34, kDeny);
        prompts[request.id] = std::move(prompt);
        ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog); SetFocus(deny);
    }
    void refresh() {
        const auto now = GetTickCount64();
        if (now >= service_probe_at) {
            service_probe_at = now + 2000;
            const auto presence = service_presence();
            if (presence == ServicePresence::Installed) service_seen = true;
            else if (service_seen && presence == ServicePresence::Missing) {
                DestroyWindow(window);
                return;
            }
            service_state_snapshot = presence == ServicePresence::Installed ? service_state() : ServiceState::Unknown;
        }
        if (editor && WaitForSingleObject(editor, 0) == WAIT_OBJECT_0) { CloseHandle(editor); editor = nullptr; }
        EnableWindow(GetDlgItem(window, kSecurity), !editor);
        EnableWindow(GetDlgItem(window, kNetwork), !editor);
        const auto pending = client.approvals();
        for (auto it = prompts.begin(); it != prompts.end();) {
            if (std::none_of(pending.begin(), pending.end(), [&](const auto& p) { return p.id == it->first; })) {
                if (it->second->window) DestroyWindow(it->second->window);
                it = prompts.erase(it);
            } else ++it;
        }
        for (const auto& request : pending) if (!prompts.contains(request.id)) create_prompt(request);
        for (const auto& notice : client.take_notices()) {
            if (notice.incoming) connections->receive_chat(notice.id, notice.text);
            else connections->notify(notice.id, notice.text);
        }
        SetWindowTextW(status, status_text().c_str());
        const auto info = client.information();
        // Avoid resetting selection/caret in a read-only field every timer tick.
        const auto set_changed = [](HWND control, const std::wstring& value) {
            if (!control) return;
            std::wstring previous(GetWindowTextLengthW(control) + 1, L'\0');
            previous.resize(GetWindowTextW(control, previous.data(), static_cast<int>(previous.size())));
            if (previous != value) SetWindowTextW(control, value.c_str());
        };
        set_changed(device_id, widen(info.device_id));
        set_changed(fingerprint, widen(format_fingerprint(info.fingerprint)));
        EnableWindow(GetDlgItem(window, kCopyId), !info.device_id.empty());
        EnableWindow(GetDlgItem(window, kCopyFingerprint), !info.fingerprint.empty());
        std::wstring auth;
        if (client.connected()) {
            const auto mode = info.auth_mode == HostAuthMode::Password ? tr(L"密码") : info.auth_mode == HostAuthMode::Click ? tr(L"点击批准") : tr(L"密码或点击批准");
            auth = std::wstring(tr(L"认证方式：")) + mode + L" · " + (info.password_configured ? tr(L"已设置永久密码") : tr(L"未设置永久密码")) +
                L"\r\n" + (info.two_factor_enabled ? tr(L"两步验证已启用") : tr(L"两步验证未启用")) + L" · " +
                (info.registration_enabled ? tr(L"连接服务功能已启用") : tr(L"连接服务功能已关闭"));
        }
        set_changed(authentication, auth);
    }
    std::wstring status_text() {
        std::wstring text;
        if (client.connected()) text = tr(L"已连接本会话的主机服务。");
        else switch (service_state_snapshot) {
            case ServiceState::Stopped: text = tr(L"主机服务已停止，启动后将自动连接。"); break;
            case ServiceState::Starting: text = tr(L"主机服务正在启动，请稍候…"); break;
            case ServiceState::Stopping: text = tr(L"主机服务正在停止，等待后续启动。"); break;
            case ServiceState::Paused: text = tr(L"主机服务已暂停，恢复后将自动连接。"); break;
            case ServiceState::Running: text = tr(L"服务正在运行，等待本会话的工作进程。"); break;
            default: text = tr(L"等待本会话的主机服务，正在自动重连…"); break;
        }
        text += L"\r\n" + std::wstring(tr(L"Windows 会话：")) + std::to_wstring(windows_session) + L" · ";
        text += client.connected() ? tr(L"活动连接：") + std::to_wstring(client.actions().viewers().size()) : tr(L"仅管理本登录会话");
        return text;
    }
    bool create_overview() {
        ProcessIdToSessionId(GetCurrentProcessId(), &windows_session);
        window = CreateWindowExW(0, kWindow, tr(L"chast 服务连接管理"),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
            px(730), px(390), nullptr, nullptr, instance, this);
        if (!window) return false;
        status = add(window, L"STATIC", tr(L"等待本会话的主机服务，正在自动重连…"), SS_LEFT, 20, 20, 680, 48);
        add(window, L"STATIC", tr(L"设备 ID"), SS_LEFT, 20, 84, 100, 24);
        device_id = add(window, L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 120, 78, 430, 28);
        add(window, L"BUTTON", tr(L"复制 ID"), BS_PUSHBUTTON | WS_TABSTOP, 565, 78, 130, 28, kCopyId);
        add(window, L"STATIC", L"SHA256", SS_LEFT, 20, 130, 100, 24);
        fingerprint = add(window, L"EDIT", L"", ES_READONLY | ES_MULTILINE | WS_BORDER | WS_TABSTOP, 120, 122, 430, 50);
        add(window, L"BUTTON", tr(L"复制指纹"), BS_PUSHBUTTON | WS_TABSTOP, 565, 130, 130, 28, kCopyFingerprint);
        authentication = add(window, L"STATIC", L"", SS_LEFT, 20, 190, 680, 48);
        add(window, L"BUTTON", tr(L"服务安全设置…"), BS_PUSHBUTTON | WS_TABSTOP, 20, 252, 215, 32, kSecurity);
        add(window, L"BUTTON", tr(L"服务连接设置…"), BS_PUSHBUTTON | WS_TABSTOP, 245, 252, 220, 32, kNetwork);
        add(window, L"BUTTON", tr(L"管理连接…"), BS_PUSHBUTTON | WS_TABSTOP, 475, 252, 220, 32, kOpen);
        add(window, L"BUTTON", tr(L"退出连接管理（服务继续运行）"), BS_PUSHBUTTON | WS_TABSTOP, 20, 302, 450, 32, kExit);
        return true;
    }
    void show() { ShowWindow(window, SW_SHOWNORMAL); SetForegroundWindow(window); }
    void settings(bool network) {
        if (editor) return;
        const auto executable = executable_path();
        SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info); info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.hwnd = window; info.lpVerb = L"runas"; info.lpFile = executable.c_str();
        info.lpParameters = network ? L"--service-network-settings" : L"--service-settings"; info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info)) {
            if (GetLastError() != ERROR_CANCELLED) MessageBoxW(window, tr(L"无法打开服务设置，请检查管理员权限。"),
                tr(L"主机服务设置"), MB_OK | MB_ICONERROR);
            return;
        }
        editor = info.hProcess;
        EnableWindow(GetDlgItem(window, kSecurity), FALSE); EnableWindow(GetDlgItem(window, kNetwork), FALSE);
    }
    void add_tray() {
        NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = window; data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; data.uCallbackMessage = kTray;
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(data.szTip, tr(L"chast 服务连接管理"));
        tray = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    }
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Manager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Manager*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = hwnd; SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wp, lp);
        if (message == WM_TIMER) { self->refresh(); return 0; }
        if (message == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
        if (message == WM_QUERYENDSESSION) return TRUE;
        if (message == WM_COMMAND) {
            if (LOWORD(wp) == kOpen) self->connections->show();
            else if (LOWORD(wp) == kExit) DestroyWindow(hwnd);
            else if (LOWORD(wp) == kSecurity) self->settings(false);
            else if (LOWORD(wp) == kNetwork) self->settings(true);
            else if (LOWORD(wp) == kCopyId) copy_text(hwnd, widen(self->client.information().device_id));
            else if (LOWORD(wp) == kCopyFingerprint) copy_text(hwnd, widen(format_fingerprint(self->client.information().fingerprint)));
            return 0;
        }
        if (message == kTray) {
            if (lp == WM_LBUTTONDBLCLK) self->show();
            else if (lp == WM_RBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, kOpen, tr(L"管理连接…"));
                AppendMenuW(menu, MF_STRING | (self->editor ? MF_GRAYED : 0), kSecurity, tr(L"服务安全设置…"));
                AppendMenuW(menu, MF_STRING | (self->editor ? MF_GRAYED : 0), kNetwork, tr(L"服务连接设置…"));
                AppendMenuW(menu, MF_STRING, kExit, tr(L"退出连接管理（服务继续运行）"));
                POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr); DestroyMenu(menu);
            }
            return 0;
        }
        if (message == RegisterWindowMessageW(L"TaskbarCreated")) { self->add_tray(); return 0; }
        if (message == WM_ENDSESSION && wp) { DestroyWindow(hwnd); return 0; }
        if (message == WM_DESTROY) {
            KillTimer(hwnd, 1); self->client.stop();
            for (auto& [id, prompt] : self->prompts) if (prompt->window) DestroyWindow(prompt->window);
            self->prompts.clear(); self->connections.reset();
            if (self->editor) { CloseHandle(self->editor); self->editor = nullptr; }
            NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = hwnd; data.uID = 1;
            if (self->tray) Shell_NotifyIconW(NIM_DELETE, &data);
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
};
}
int run_service_manager(HINSTANCE instance, bool hidden) {
    SetLastError(ERROR_SUCCESS);
    HANDLE unique = CreateMutexExW(nullptr, L"Local\\chast-service-manager", 0, SYNCHRONIZE);
    if (!unique) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!hidden) {
            if (HWND existing = FindWindowW(kWindow, nullptr)) { ShowWindow(existing, SW_SHOWNORMAL); SetForegroundWindow(existing); }
        }
        CloseHandle(unique); return 0;
    }
    Manager manager{instance};
    manager.service_seen = service_presence() == ServicePresence::Installed;
    HDC screen = GetDC(nullptr); manager.dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen);
    manager.font = CreateFontW(-manager.px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = kPrompt; cls.lpfnWndProc = Manager::prompt_proc; RegisterClassExW(&cls);
    cls.lpszClassName = kWindow; cls.lpfnWndProc = Manager::proc; RegisterClassExW(&cls);
    if (!manager.create_overview()) { DeleteObject(manager.font); CloseHandle(unique); return 1; }
    const auto window = manager.window;
    manager.connections = std::make_unique<ConnectionsWindow>(window, instance, manager.font, manager.dpi, manager.client.actions());
    manager.client.start(); manager.add_tray(); SetTimer(window, 1, 250, nullptr);
    if (!hidden || !manager.tray) manager.show();
    MSG message{};
    BOOL received = 0;
    while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        bool handled = false;
        for (auto& [id, prompt] : manager.prompts) if (prompt->window && IsDialogMessageW(prompt->window, &message)) { handled = true; break; }
        if (handled || (manager.connections && manager.connections->translate(message))) continue;
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (IsWindow(window)) DestroyWindow(window);
    manager.client.stop(); DeleteObject(manager.font); CloseHandle(unique);
    return received < 0 ? 1 : static_cast<int>(message.wParam);
}
}
