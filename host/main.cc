// Tray application for the chast host.
//
// It owns the listener: the same HostServer the command-line tool runs, plus
// a window that shows the token, the identity fingerprint and the live state,
// a notification-area icon, the HKCU autostart entry and the firewall rule.
//
// Commands: --config <path> to use another settings file, --tray to start
// hidden (what the autostart entry passes), --autostart on|off to change the
// login entry and exit, --firewall <port> [<tunnel_port>] to apply the rules
// (the elevated child the tray starts through the UAC prompt).

#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "chast/audio.h"
#include "chast/file_transfer.h"
#include "chast/host_server.h"
#include "chast/host_settings.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "platform.h"
#include "service.h"
#include "tunnel.h"

namespace {

using namespace chast;
using chast::host::narrow;
using chast::host::TunnelEvent;
using chast::host::TunnelProcess;
using chast::host::validate_tunnel_settings;
using chast::host::widen;

constexpr wchar_t kWindowClass[] = L"ChastHostTrayWindow";
constexpr wchar_t kWindowTitle[] = L"chast 主机";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kLogMessage = WM_APP + 2;
constexpr UINT kTokenMessage = WM_APP + 3;
constexpr UINT kTunnelMessage = WM_APP + 4;
constexpr UINT kTunnelExitMessage = WM_APP + 5;
constexpr UINT kSecurityMessage = WM_APP + 6;
constexpr UINT kSecurityPageMessage = WM_APP + 7;
constexpr UINT kChatMessage = WM_APP + 8;
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kTrayIconId = 1;

enum ControlId {
    kStateLabel = 1000,
    kPeerLabel,
    kStreamLabel,
    kTunnelLabel,
    kPathLabel,
    kDeviceIdEdit,
    kCopyIdButton,
    kTempPasswordEdit,
    kCopyPasswordButton,
    kRefreshPasswordButton,
    kSecuritySummary,
    kSessionLabel,
    kInputCheck,
    kClipboardCheck,
    kFileTransferCheck,
    kPortForwardCheck,
    kTerminalCheck,
    kSendFileButton,
    kAudioCheck,
    kMicrophoneCheck,
    kPrivacyModeCheck,
    kCarrierCheck,
    kAutostartCheck,
    kSecurityButton,
    kFirewallButton,
    kOpenConfigButton,
    kToggleButton,
    kTunnelSettingsButton,
    kTunnelRegisterButton,
    kReloadButton,
    kHideButton,
    kQuitButton,
    kChatButton,
    kLogEdit,
};

enum ChatDialogId {
    kChatHistory = 5000,
    kChatInput,
    kChatSend,
};

struct HostApp {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND state_label = nullptr;
    HWND peer_label = nullptr;
    HWND stream_label = nullptr;
    HWND tunnel_label = nullptr;
    HWND path_label = nullptr;
    HWND device_id_edit = nullptr;
    HWND temp_password_edit = nullptr;
    HWND refresh_password_button = nullptr;
    HWND security_summary = nullptr;
    HWND session_label = nullptr;
    HWND input_check = nullptr;
    HWND clipboard_check = nullptr;
    HWND audio_check = nullptr;
    HWND microphone_check = nullptr;
    HWND privacy_check = nullptr;
    HWND carrier_check = nullptr;
    HWND autostart_check = nullptr;
    HWND toggle_button = nullptr;
    HWND log_edit = nullptr;
    HFONT font = nullptr;
    HFONT title_font = nullptr;
    int dpi = 96;
    bool tray_added = false;
    // Written by the UI thread, read by the approval dialog's timer on the
    // host thread.
    std::atomic<bool> quitting{false};
    std::atomic<bool> server_stopping{false};
    std::wstring tray_tip;

    std::string config_path;
    HostSettings settings;
    // The listener's microphone gate is fixed at creation; the UI uses this to
    // tell an operator whether enabling it applies now or next session.
    bool listener_microphone = false;
    // Same for the privacy-mode gate: requests are dropped by a listener that
    // was created before the feature was enabled.
    bool listener_privacy = false;
    std::string startup_warning;
    bool startup_created = false;
    std::string startup_save_error;
    std::unique_ptr<HostServer> server;
    TunnelProcess tunnel;
    std::mutex tunnel_mutex;
    std::deque<TunnelEvent> tunnel_events;
    std::string tunnel_relay;
    std::string tunnel_viewer_path;
    std::string tunnel_viewer_remote;
    std::string tunnel_error;
    std::deque<std::wstring> log_lines;
    // Chat is a modeless window so the operator can keep it open next to the
    // main page. History is cleared when a new viewer starts streaming.
    HWND chat_window = nullptr;
    HWND chat_history = nullptr;
    HWND chat_input = nullptr;
    bool chat_live = false;
    std::deque<std::wstring> chat_lines;
};

HostApp* g_app = nullptr;

void append_chat_line(const std::wstring& text);

int px(int value) {
    return MulDiv(value, g_app->dpi, 96);
}

void set_text(HWND control, const std::wstring& text) {
    if (control == nullptr) {
        return;
    }
    const int length = GetWindowTextLengthW(control);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, current.data(), length + 1);
    current.resize(static_cast<size_t>(length));
    if (current != text) {
        SetWindowTextW(control, text.c_str());
    }
}

void set_check(HWND control, bool checked) {
    if (control == nullptr) {
        return;
    }
    const LRESULT state = SendMessageW(control, BM_GETCHECK, 0, 0);
    const WPARAM wanted = checked ? BST_CHECKED : BST_UNCHECKED;
    if (static_cast<WPARAM>(state) != wanted) {
        SendMessageW(control, BM_SETCHECK, wanted, 0);
    }
}

void append_log_line(const std::wstring& text) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32]{};
    swprintf_s(stamp, std::size(stamp), L"[%02u:%02u:%02u] ", now.wHour, now.wMinute,
               now.wSecond);
    g_app->log_lines.push_back(std::wstring(stamp) + text);
    while (g_app->log_lines.size() > 400) {
        g_app->log_lines.pop_front();
    }
    if (g_app->log_edit == nullptr) {
        return;
    }
    std::wstring all;
    for (const std::wstring& line : g_app->log_lines) {
        all += line;
        all += L"\r\n";
    }
    SetWindowTextW(g_app->log_edit, all.c_str());
    SendMessageW(g_app->log_edit, EM_SETSEL, static_cast<WPARAM>(all.size()),
                 static_cast<LPARAM>(all.size()));
    SendMessageW(g_app->log_edit, EM_SCROLLCARET, 0, 0);
}

void post_log(const std::wstring& text) {
    auto* copy = new std::wstring(text);
    if (!PostMessageW(g_app->window, kLogMessage, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

void server_log(const std::string& message) {
    post_log(widen(message));
}

void server_token_changed(const std::string& token) {
    auto* copy = new std::string(token);
    if (!PostMessageW(g_app->window, kTokenMessage, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

void server_trusted_devices_changed(const std::vector<TrustedDevice>& devices) {
    auto* copy = new std::vector<TrustedDevice>(devices);
    if (!PostMessageW(g_app->window, kSecurityMessage, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

void server_chat(const std::string& text) {
    auto* copy = new std::string(text);
    if (!PostMessageW(g_app->window, kChatMessage, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

void tunnel_event(const TunnelEvent& event) {
    {
        std::lock_guard<std::mutex> lock(g_app->tunnel_mutex);
        g_app->tunnel_events.push_back(event);
    }
    PostMessageW(g_app->window, kTunnelMessage, 0, 0);
}

void tunnel_log(const std::string& message) {
    post_log(widen(message));
}

void tunnel_exited(uint32_t code) {
    PostMessageW(g_app->window, kTunnelExitMessage, static_cast<WPARAM>(code), 0);
}

constexpr wchar_t kApprovalDialogClass[] = L"ChastApprovalWindow";

enum ApprovalDialogId {
    kApprovalInput = 4000,
    kApprovalClipboard,
    kApprovalFile,
    kApprovalAudio,
    kApprovalMicrophone,
    kApprovalPrivacy,
    kApprovalAllow,
    kApprovalDeny,
};

struct ApprovalDialogState {
    HWND window = nullptr;
    HWND input = nullptr;
    HWND clipboard = nullptr;
    HWND file = nullptr;
    HWND audio = nullptr;
    HWND microphone = nullptr;
    HWND privacy = nullptr;
    // Valid for the duration of server_approve(): the transport keeps the
    // request alive across the callback and re-checks liveness before it
    // admits the viewer.
    const ClientRequest* request = nullptr;
    uint16_t result = 0;
    bool allowed = false;
    bool peer_gone = false;
    bool done = false;
};

bool approval_peer_alive(const ApprovalDialogState& dialog) {
    // A caller that does not provide the predicate cannot be watched; keep
    // the dialog open rather than closing it for the wrong reason.
    return dialog.request == nullptr || !dialog.request->peer_alive ||
           dialog.request->peer_alive();
}

void finish_approval(ApprovalDialogState& dialog, bool allowed) {
    dialog.allowed = allowed;
    if (allowed) {
        uint16_t mask = 0;
        if (SendMessageW(dialog.input, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionInput;
        }
        if (SendMessageW(dialog.clipboard, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionClipboard;
        }
        if (SendMessageW(dialog.file, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionFileTransfer;
        }
        if (SendMessageW(dialog.audio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionAudio;
        }
        if (SendMessageW(dialog.microphone, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionMicrophone;
        }
        if (SendMessageW(dialog.privacy, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionPrivacyMode;
        }
        dialog.result = mask;
    }
    if (dialog.window != nullptr) {
        DestroyWindow(dialog.window);
    }
}

LRESULT CALLBACK approval_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog =
        reinterpret_cast<ApprovalDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        dialog = static_cast<ApprovalDialogState*>(create->lpCreateParams);
        dialog->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        return TRUE;
    }
    if (dialog == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message) {
        case WM_COMMAND:
            if (LOWORD(wparam) == kApprovalAllow) {
                // The viewer may have given up between two timer ticks. Count
                // that as a cancelled request, not an approval, so the log
                // says why nothing was admitted.
                if (!approval_peer_alive(*dialog)) {
                    dialog->peer_gone = true;
                    finish_approval(*dialog, false);
                } else {
                    finish_approval(*dialog, true);
                }
                return 0;
            }
            if (LOWORD(wparam) == kApprovalDeny || LOWORD(wparam) == IDCANCEL) {
                finish_approval(*dialog, false);
                return 0;
            }
            break;
        case WM_CLOSE:
            finish_approval(*dialog, false);
            return 0;
        case WM_TIMER:
            // The operator may quit or stop sharing while this dialog waits;
            // the host thread must not keep the viewer hanging on a window
            // nobody is going to answer.
            if (g_app->quitting.load() || g_app->server_stopping.load()) {
                finish_approval(*dialog, false);
                return 0;
            }
            // A viewer whose handshake timed out has already disconnected.
            // Close the prompt instead of letting the operator approve a
            // session that no longer exists.
            if (!approval_peer_alive(*dialog)) {
                dialog->peer_gone = true;
                finish_approval(*dialog, false);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            dialog->window = nullptr;
            dialog->done = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::wstring platform_label(uint8_t platform) {
    return platform == 1 ? L"Windows" : L"未知系统";
}

std::wstring approval_method_label(ClientApprovalMethod method) {
    switch (method) {
        case ClientApprovalMethod::Click:
            return L"点击批准模式：每次连接都需要确认";
        case ClientApprovalMethod::NoPassword:
            return L"查看器没有提供密码，请求当面批准";
        case ClientApprovalMethod::Unknown:
            break;
    }
    return L"查看器请求连接";
}

ClientApproval server_approve(const ClientRequest& request) {
    static bool class_ready = false;
    if (!class_ready) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = approval_dialog_proc;
        window_class.hInstance = g_app->instance;
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kApprovalDialogClass;
        if (RegisterClassExW(&window_class) == 0) {
            post_log(L"无法创建连接审批窗口，已拒绝这次连接");
            return ClientApproval{};
        }
        class_ready = true;
    }

    ApprovalDialogState dialog;
    dialog.request = &request;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT frame{0, 0, px(470), px(348)};
    AdjustWindowRectEx(&frame, style, FALSE, WS_EX_TOPMOST);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kApprovalDialogClass,
                                  L"chast 连接请求", style, x, y, width, height, nullptr,
                                  nullptr, g_app->instance, &dialog);
    if (window == nullptr) {
        post_log(L"无法创建连接审批窗口，已拒绝这次连接");
        return ClientApproval{};
    }

    const auto add = [&dialog](const wchar_t* cls, const wchar_t* text, DWORD control_style,
                               int left, int top, int w, int h, int id, HFONT font) {
        HWND control = CreateWindowExW(
            0, cls, text, WS_CHILD | WS_VISIBLE | control_style, px(left), px(top), px(w),
            px(h), dialog.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app->instance, nullptr);
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(font != nullptr ? font : g_app->font), TRUE);
        }
        return control;
    };

    add(L"STATIC", L"有查看器请求连接这台电脑", SS_LEFT, 20, 16, 420, 24, 0,
        g_app->title_font);
    const std::wstring peer =
        request.peer_address.empty() ? std::wstring(L"未知地址") : widen(request.peer_address);
    add(L"STATIC", (L"来源：" + peer).c_str(), SS_LEFT, 20, 52, 430, 20, 0, nullptr);
    std::wstring device = L"设备：";
    device += request.hello.device_name.empty() ? L"未命名" : widen(request.hello.device_name);
    device += L"（" + platform_label(request.hello.device_platform) + L"）";
    add(L"STATIC", device.c_str(), SS_LEFT, 20, 76, 430, 20, 0, nullptr);
    const std::wstring method = L"方式：" + approval_method_label(request.method);
    add(L"STATIC", method.c_str(), SS_LEFT, 20, 100, 430, 40, 0, nullptr);
    if (request.device_proof) {
        add(L"STATIC", L"设备签名验证通过，允许后将记入可信设备。", SS_LEFT, 20, 140, 430, 20,
            0, nullptr);
    }
    add(L"STATIC", L"允许的权限（可以取消勾选）：", SS_LEFT, 20, 164, 430, 20, 0, nullptr);

    const auto permission_check = [&](const wchar_t* text, uint16_t bit, int left, int top,
                                      int id) {
        HWND control = add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, left, top, 130, 24,
                           id, nullptr);
        if ((request.permissions & bit) == 0) {
            SendMessageW(control, BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(control, FALSE);
        } else {
            SendMessageW(control, BM_SETCHECK, BST_CHECKED, 0);
        }
        return control;
    };
    dialog.input =
        permission_check(L"键盘与鼠标", kPermissionInput, 20, 188, kApprovalInput);
    dialog.clipboard =
        permission_check(L"剪贴板", kPermissionClipboard, 170, 188, kApprovalClipboard);
    dialog.audio = permission_check(L"系统声音", kPermissionAudio, 320, 188, kApprovalAudio);
    dialog.file = permission_check(L"文件传输", kPermissionFileTransfer, 20, 216, kApprovalFile);
    const std::string receive_directory = g_app->settings.file_receive_directory.empty()
                                              ? chast::default_receive_directory()
                                              : g_app->settings.file_receive_directory;
    add(L"STATIC",
        (L"接收目录：" +
         (receive_directory.empty() ? std::wstring(L"（不可用）") : widen(receive_directory)))
            .c_str(),
        SS_LEFT | SS_ENDELLIPSIS, 170, 220, 280, 18, 0, nullptr);
    dialog.microphone =
        permission_check(L"麦克风（上行）", kPermissionMicrophone, 20, 244,
                         kApprovalMicrophone);
    dialog.privacy =
        permission_check(L"隐私模式（黑屏）", kPermissionPrivacyMode, 170, 244,
                         kApprovalPrivacy);

    add(L"BUTTON", L"允许", BS_DEFPUSHBUTTON | WS_TABSTOP, 250, 278, 90, 32, kApprovalAllow,
        nullptr);
    add(L"BUTTON", L"拒绝", BS_PUSHBUTTON | WS_TABSTOP, 350, 278, 90, 32, kApprovalDeny,
        nullptr);

    SetTimer(window, 1, 200, nullptr);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(dialog.input);

    MSG message{};
    while (!dialog.done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    const bool allowed = dialog.allowed;
    const bool peer_gone = dialog.peer_gone;
    const uint16_t mask = dialog.result;
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    if (peer_gone) {
        post_log(L"查看者在审批期间离开，已取消来自 " + peer + L" 的请求");
    } else {
        post_log(allowed ? L"已允许来自 " + peer + L" 的连接"
                         : L"已拒绝来自 " + peer + L" 的连接");
    }
    if (!allowed) {
        return ClientApproval{};
    }
    return ClientApproval{true, mask};
}

bool save_settings() {
    std::string error;
    if (!save_host_settings(g_app->config_path, g_app->settings, &error)) {
        post_log(L"保存设置失败：" + widen(error));
        return false;
    }
    return true;
}

void stop_tunnel() {
    g_app->tunnel.stop();
    {
        std::lock_guard<std::mutex> lock(g_app->tunnel_mutex);
        g_app->tunnel_events.clear();
    }
    g_app->tunnel_relay.clear();
    g_app->tunnel_viewer_path.clear();
    g_app->tunnel_viewer_remote.clear();
}

void start_tunnel() {
    if (!g_app->settings.tunnel || g_app->tunnel.running()) {
        return;
    }
    if (g_app->settings.server_url.empty()) {
        g_app->tunnel_error = "尚未配置服务器地址";
        append_log_line(L"连接服务：尚未配置服务器地址");
        return;
    }
    std::string error;
    if (!validate_tunnel_settings(g_app->settings, &error)) {
        g_app->tunnel_error = error;
        append_log_line(L"连接服务：" + widen(error));
        return;
    }
    std::string fingerprint;
    if (g_app->server) {
        fingerprint = g_app->server->status().fingerprint;
    }
    g_app->tunnel_relay.clear();
    g_app->tunnel_viewer_path.clear();
    g_app->tunnel_viewer_remote.clear();
    g_app->tunnel_error.clear();
    if (!g_app->tunnel.start(g_app->settings, g_app->settings.port, fingerprint, tunnel_event,
                             tunnel_log, tunnel_exited, &error)) {
        g_app->tunnel_error = error;
        append_log_line(L"连接服务启动失败：" + widen(error));
        return;
    }
    append_log_line(L"正在注册设备 " + widen(g_app->settings.tunnel_id) + L"…");
}

void stop_server() {
    stop_tunnel();
    if (!g_app->server) {
        return;
    }
    g_app->server_stopping.store(true);
    g_app->server->stop();
    g_app->server_stopping.store(false);
    g_app->server.reset();
    append_log_line(L"共享已停止");
}

void start_server() {
    if (g_app->server) {
        return;
    }
    if (g_app->config_path == host_settings_path() && chast::host::service_running()) {
        append_log_line(L"chast 主机服务正在运行，托盘不能同时共享");
        return;
    }
    HostServerConfig config;
    config.settings = g_app->settings;
    config.serve_once = false;
    config.first_client_timeout_ms = 0;
    // The policy decides when to ask; the dialog is available for both and
    // click modes and never called in password-only mode.
    config.approve_client = server_approve;
    config.log = server_log;
    config.token_changed = server_token_changed;
    config.trusted_devices_changed = server_trusted_devices_changed;
    config.chat_message = server_chat;
    std::string error;
    g_app->server = HostServer::create(std::move(config), &error);
    if (!g_app->server) {
        append_log_line(L"启动失败：" + widen(error));
        const std::wstring message = L"无法启动共享：\n" + widen(error);
        MessageBoxW(g_app->window, message.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
        return;
    }
    g_app->listener_microphone = g_app->settings.allow_microphone;
    g_app->listener_privacy = g_app->settings.allow_privacy_mode;
    g_app->server->start();
    append_log_line(L"正在等待查看器（端口 " + std::to_wstring(g_app->settings.port) + L"）");
    start_tunnel();
}

void restart_server() {
    stop_server();
    start_server();
}

void restart_tunnel() {
    stop_tunnel();
    if (!g_app->server) {
        start_server();
        return;
    }
    start_tunnel();
}

void update_tray_tip(const std::wstring& tip) {
    if (!g_app->tray_added || tip == g_app->tray_tip) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_app->window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
    g_app->tray_tip = tip;
}

std::wstring codec_label(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264:
            return L"H.264";
        case VideoCodec::Hevc:
            return L"HEVC";
    }
    return L"—";
}

std::wstring state_text(HostState state, const HostStatus& status) {
    switch (state) {
        case HostState::Stopped:
            return L"已停止";
        case HostState::Listening: {
            std::wstring text =
                L"等待查看器（端口 " + std::to_wstring(g_app->settings.port) + L"）";
            if (status.carrier_listening) {
                text += L" · 弱网回退端口 " + std::to_wstring(status.carrier_port);
            } else if (!status.carrier_error.empty()) {
                text += L" · 弱网回退不可用";
            }
            return text;
        }
        case HostState::Streaming:
            return L"正在共享";
        case HostState::Failed:
            return status.error.empty() ? L"出错" : L"出错：" + widen(status.error);
    }
    return L"—";
}

std::wstring tunnel_text() {
    const HostSettings& settings = g_app->settings;
    if (!settings.tunnel) {
        return L"连接服务：已关闭";
    }
    if (settings.server_url.empty()) {
        return L"连接服务：未配置服务器（打开“连接服务设置…”）";
    }
    std::wstring text = L"连接服务：";
    if (g_app->tunnel.running()) {
        text += g_app->tunnel_relay.empty() ? L"注册中…"
                                            : L"已在线 · 中继 " + widen(g_app->tunnel_relay);
    } else if (!g_app->server) {
        text += L"已停止";
    } else if (!g_app->tunnel_error.empty()) {
        text += L"离线 · " + widen(g_app->tunnel_error);
    } else {
        text += L"未连接";
    }
    if (!settings.tunnel_id.empty()) {
        text += L" · 设备 ID " + widen(settings.tunnel_id);
    }
    return text;
}

std::wstring path_text(const HostStatus& status) {
    if (status.state != HostState::Streaming || g_app->tunnel_viewer_path.empty()) {
        return L"最近路径：—";
    }
    const bool relay = g_app->tunnel_viewer_path == "relay";
    std::wstring text = relay ? L"最近路径：经中继" : L"最近路径：直连";
    if (!g_app->tunnel_viewer_remote.empty()) {
        text += L" · " + widen(g_app->tunnel_viewer_remote);
    }
    return text;
}

std::wstring auth_mode_label(HostAuthMode mode) {
    switch (mode) {
        case HostAuthMode::Password:
            return L"密码";
        case HostAuthMode::Click:
            return L"点击批准";
        case HostAuthMode::Both:
            return L"密码或批准";
    }
    return L"—";
}

std::wstring permission_label(uint16_t mask) {
    std::wstring text;
    auto add = [&text](const wchar_t* name) {
        if (!text.empty()) {
            text += L"、";
        }
        text += name;
    };
    if ((mask & kPermissionInput) != 0) {
        add(L"键盘");
    }
    if ((mask & kPermissionClipboard) != 0) {
        add(L"剪贴板");
    }
    if ((mask & kPermissionFileTransfer) != 0) {
        add(L"文件传输");
    }
    if ((mask & kPermissionPortForward) != 0) {
        add(L"端口转发");
    }
    if ((mask & kPermissionTerminal) != 0) {
        add(L"远程终端");
    }
    if ((mask & kPermissionAudio) != 0) {
        add(L"声音");
    }
    if ((mask & kPermissionMicrophone) != 0) {
        add(L"麦克风");
    }
    if ((mask & kPermissionPrivacyMode) != 0) {
        add(L"隐私模式");
    }
    return text.empty() ? L"只看" : text;
}

uint16_t current_gate_mask() {
    return static_cast<uint16_t>((g_app->settings.allow_input ? kPermissionInput : 0) |
                                 (g_app->settings.allow_clipboard ? kPermissionClipboard : 0) |
                                 (g_app->settings.allow_audio ? kPermissionAudio : 0) |
                                 (g_app->settings.allow_microphone ? kPermissionMicrophone : 0) |
                                 (g_app->settings.allow_privacy_mode ? kPermissionPrivacyMode
                                                                     : 0) |
                                 (g_app->settings.allow_file_transfer ? kPermissionFileTransfer
                                                                      : 0) |
                                 (g_app->settings.allow_port_forward ? kPermissionPortForward
                                                                     : 0) |
                                 (g_app->settings.allow_terminal ? kPermissionTerminal
                                                                 : 0));
}

std::wstring security_summary_text(const HostStatus& status) {
    const bool live = g_app->server != nullptr;
    const HostAuthMode mode = live ? status.auth_mode : g_app->settings.auth_mode;
    const bool password =
        live ? status.password_configured : !g_app->settings.password_hash.empty();
    const bool two_factor = live ? status.two_factor_enabled : !g_app->settings.totp_secret.empty();
    std::wstring text = L"接入：" + auth_mode_label(mode);
    text += password ? L" · 永久密码已设置" : L" · 永久密码未设置";
    text += two_factor ? L" · 2FA 已启用" : L" · 2FA 已关闭";
    text += L" · 默认权限：" + permission_label(current_gate_mask());
    return text;
}

std::wstring session_permission_text(const HostStatus& status) {
    if (status.state != HostState::Streaming) {
        return L"本次会话权限：—";
    }
    std::wstring text = L"本次会话权限：" + permission_label(status.session_permissions);
    if (status.stats.privacy_mode_active) {
        text += L" · 本地屏幕已遮挡（隐私模式）";
    }
    return text;
}

// Reads the live state and updates every label. Called on a timer, so it only
// writes text that changed: rewriting an edit would clear a selection the
// operator is about to copy.
void refresh_ui() {
    HostStatus status;
    if (g_app->server) {
        status = g_app->server->status();
    }
    const bool running = g_app->server && g_app->server->running();

    // Chat history belongs to one viewer: clear it when a session starts so a
    // later viewer never sees the previous conversation.
    const bool streaming = g_app->server != nullptr && status.state == HostState::Streaming;
    if (streaming != g_app->chat_live) {
        g_app->chat_live = streaming;
        if (streaming) {
            g_app->chat_lines.clear();
            append_chat_line(L"—— 查看者已连接，可以开始聊天 ——");
        } else {
            append_chat_line(L"—— 查看者已断开 ——");
        }
    }

    set_text(g_app->state_label, L"状态：" + (g_app->server ? state_text(status.state, status)
                                                           : std::wstring(L"已停止")));
    set_text(g_app->peer_label,
             L"查看器：" + (status.peer_address.empty() ? std::wstring(L"—")
                                                       : widen(status.peer_address)));
    if (status.stream.width > 0) {
        std::wstring stream = L"画面：" + widen(status.display_name) + L" · " +
                              std::to_wstring(status.stream.width) + L"×" +
                              std::to_wstring(status.stream.height) + L" · " +
                              codec_label(status.stream.codec) + L" · " +
                              std::to_wstring(status.stream.framerate) + L" fps · " +
                              std::to_wstring(status.stats.encoded) + L" 帧";
        if (status.stats.target_bitrate > 0) {
            stream += L" · " +
                      std::to_wstring((status.stats.target_bitrate + 500) / 1000) + L" kbps";
        }
        if (status.stats.rtt_ms > 0.0) {
            stream += L" · RTT " +
                      std::to_wstring(static_cast<int>(status.stats.rtt_ms + 0.5)) + L" ms";
        }
        if (status.stats.input_events > 0) {
            stream += L" · 输入 " + std::to_wstring(status.stats.input_events);
        }
        if (status.stats.audio_active) {
            stream += L" · 音频";
            if (status.stats.audio_dsp_frames > 0) {
                stream += L" · AEC " + std::to_wstring(status.stats.audio_dsp_frames) +
                          L" 帧";
                if (status.stats.audio_dsp_resets > 0) {
                    stream += L"（重置 " +
                              std::to_wstring(status.stats.audio_dsp_resets) + L"）";
                }
            }
        }
        if (status.download_active) {
            stream += L" · 发送 " + widen(status.download_name) + L" " +
                      std::to_wstring(status.download_sent) + L"/" +
                      std::to_wstring(status.download_size);
        } else if (!status.download_error.empty()) {
            stream += L" · 发送失败";
        }
        set_text(g_app->stream_label, stream);
    } else {
        set_text(g_app->stream_label, L"画面：—");
    }
    set_text(g_app->tunnel_label, tunnel_text());
    set_text(g_app->path_label, path_text(status));

    // A rotated one-time token still has to reach the settings file even
    // though the main page no longer shows it; the security dialog reads it.
    std::string token = g_app->settings.access_token;
    if (!status.token.empty()) {
        token = status.token;
    }
    if (!token.empty() && token != g_app->settings.access_token) {
        g_app->settings.access_token = token;
        save_settings();
    }

    set_text(g_app->device_id_edit, g_app->settings.tunnel_id.empty()
                                        ? std::wstring(L"—")
                                        : widen(g_app->settings.tunnel_id));
    const bool temp_available = !status.temporary_password.empty();
    std::wstring temporary = temp_available ? widen(status.temporary_password) : L"—";
    if (!temp_available && g_app->server && status.auth_mode == HostAuthMode::Click) {
        temporary = L"点击批准模式";
    }
    set_text(g_app->temp_password_edit, temporary);
    if (g_app->refresh_password_button != nullptr) {
        EnableWindow(g_app->refresh_password_button, temp_available);
    }
    set_text(g_app->security_summary, security_summary_text(status));
    set_text(g_app->session_label, session_permission_text(status));

    set_check(g_app->input_check, g_app->settings.allow_input);
    set_check(g_app->clipboard_check, g_app->settings.allow_clipboard);
    set_check(g_app->audio_check, g_app->settings.allow_audio);
    set_check(g_app->microphone_check, g_app->settings.allow_microphone);
    set_check(g_app->privacy_check, g_app->settings.allow_privacy_mode);
    set_check(g_app->carrier_check, g_app->settings.carrier);
    set_check(g_app->autostart_check, g_app->settings.autostart);
    set_text(g_app->toggle_button, running ? L"停止共享" : L"开始共享");

    std::wstring tip = std::wstring(L"chast 主机 · ") +
                       (g_app->server ? state_text(status.state, status) : std::wstring(L"已停止"));
    if (g_app->settings.tunnel && g_app->tunnel.running()) {
        tip += g_app->tunnel_relay.empty() ? L" · 连接服务注册中" : L" · 连接服务在线";
    }
    update_tray_tip(tip);
}

void apply_tunnel_event(const TunnelEvent& event) {
    if (event.event == "ready") {
        g_app->tunnel_relay = event.relay;
        g_app->tunnel_error.clear();
        const std::string device =
            event.host.empty() ? g_app->settings.tunnel_id : event.host;
        std::wstring line = L"连接服务已上线 · 设备 ID " + widen(device);
        if (!event.listen.empty()) {
            line += L" · 隧道 " + widen(event.listen);
        }
        if (!event.relay.empty()) {
            line += L" · 中继 " + widen(event.relay);
        }
        append_log_line(line);
        if (!event.migrated_from.empty()) {
            append_log_line(L"设备 ID 已从 " + widen(event.migrated_from) + L" 迁移到 " +
                            widen(device));
            if (g_app->settings.tunnel_old_id == event.migrated_from) {
                g_app->settings.tunnel_old_id.clear();
                save_settings();
            }
        }
    } else if (event.event == "relay") {
        g_app->tunnel_relay = event.relay;
        append_log_line(L"连接服务中继地址：" + widen(event.relay));
    } else if (event.event == "viewer") {
        g_app->tunnel_viewer_path = event.path;
        g_app->tunnel_viewer_remote = event.remote;
        std::wstring line =
            event.path == "relay" ? L"查看者经中继连接" : L"查看者经直连";
        if (!event.remote.empty()) {
            line += L"：" + widen(event.remote);
        }
        append_log_line(line);
    }
}

void process_tunnel_events() {
    std::deque<TunnelEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_app->tunnel_mutex);
        events.swap(g_app->tunnel_events);
    }
    for (const TunnelEvent& event : events) {
        apply_tunnel_event(event);
    }
    refresh_ui();
}

void load_settings() {
    const HostSettingsLoad loaded = load_host_settings(g_app->config_path);
    g_app->settings = loaded.settings;
    g_app->startup_warning = loaded.warning;
    g_app->startup_created = !loaded.existed || g_app->settings.access_token.empty() ||
                             g_app->settings.tunnel_id.empty() ||
                             g_app->settings.tunnel_key.empty();
    if (g_app->settings.access_token.empty()) {
        g_app->settings.access_token = generate_access_token();
    }
    if (g_app->settings.tunnel_id.empty()) {
        g_app->settings.tunnel_id = generate_tunnel_id();
    }
    if (g_app->settings.tunnel_key.empty()) {
        g_app->settings.tunnel_key = generate_access_token(16);
    }
    if (!g_app->settings.tunnel_old_id.empty() &&
        g_app->settings.tunnel_old_id == g_app->settings.tunnel_id) {
        g_app->settings.tunnel_old_id.clear();
    }
    if (g_app->startup_created) {
        std::string error;
        if (!save_host_settings(g_app->config_path, g_app->settings, &error)) {
            g_app->startup_save_error = error;
        }
    }
}

constexpr UINT kMenuShow = 1;

HWND add_control(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex_style,
                 int x, int y, int width, int height, int id, HFONT font) {
    HWND control = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style, px(x),
                                   px(y), px(width), px(height), g_app->window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   g_app->instance, nullptr);
    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font != nullptr ? font : g_app->font), TRUE);
    }
    return control;
}

void create_fonts() {
    const int normal = -MulDiv(9, g_app->dpi, 72);
    const int title = -MulDiv(15, g_app->dpi, 72);
    g_app->font = CreateFontW(normal, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    g_app->title_font =
        CreateFontW(title, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void create_controls() {
    add_control(L"STATIC", L"chast 主机", SS_LEFT, 0, 24, 18, 300, 30, 0,
                g_app->title_font);
    add_control(L"STATIC", L"远程桌面主机 · 设备 ID、临时密码与安全设置", SS_LEFT, 0, 24, 52,
                500, 20, 0, nullptr);

    g_app->state_label = add_control(L"STATIC", L"状态：启动中…", SS_LEFT, 0, 24, 84, 572, 22,
                                     kStateLabel, nullptr);
    g_app->peer_label = add_control(L"STATIC", L"查看器：—", SS_LEFT, 0, 24, 108, 572, 22,
                                    kPeerLabel, nullptr);
    g_app->stream_label = add_control(L"STATIC", L"画面：—", SS_LEFT, 0, 24, 132, 572, 22,
                                      kStreamLabel, nullptr);
    g_app->tunnel_label = add_control(L"STATIC", L"连接服务：—", SS_LEFT, 0, 24, 156, 572, 22,
                                      kTunnelLabel, nullptr);
    g_app->path_label = add_control(L"STATIC", L"最近路径：—", SS_LEFT, 0, 24, 180, 572, 22,
                                    kPathLabel, nullptr);

    add_control(L"STATIC", L"设备 ID（用于连接服务）", SS_LEFT, 0, 24, 214, 300, 20, 0,
                nullptr);
    g_app->device_id_edit =
        add_control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 24, 236, 340,
                    26, kDeviceIdEdit, nullptr);
    add_control(L"BUTTON", L"复制 ID", BS_PUSHBUTTON, 0, 376, 235, 88, 28, kCopyIdButton,
                nullptr);

    add_control(L"STATIC", L"临时密码（使用一次或连续失败后自动更换）", SS_LEFT, 0, 24, 272,
                400, 20, 0, nullptr);
    g_app->temp_password_edit =
        add_control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 24, 294, 340,
                    26, kTempPasswordEdit, nullptr);
    add_control(L"BUTTON", L"复制密码", BS_PUSHBUTTON, 0, 376, 293, 88, 28, kCopyPasswordButton,
                nullptr);
    g_app->refresh_password_button = add_control(L"BUTTON", L"刷新密码", BS_PUSHBUTTON, 0, 476,
                                                 293, 120, 28, kRefreshPasswordButton, nullptr);

    g_app->security_summary = add_control(L"STATIC", L"接入：—", SS_LEFT, 0, 24, 330, 572, 20,
                                          kSecuritySummary, nullptr);
    g_app->session_label = add_control(L"STATIC", L"本次会话权限：—", SS_LEFT, 0, 24, 352, 440,
                                       20, kSessionLabel, nullptr);
    add_control(L"BUTTON", L"会话聊天…", BS_PUSHBUTTON, 0, 476, 348, 120, 28, kChatButton,
                nullptr);

    g_app->input_check = add_control(L"BUTTON", L"接受远程输入", BS_AUTOCHECKBOX, 0, 24, 380,
                                     140, 24, kInputCheck, nullptr);
    g_app->clipboard_check = add_control(L"BUTTON", L"同步剪贴板", BS_AUTOCHECKBOX, 0, 180, 380,
                                         130, 24, kClipboardCheck, nullptr);
    g_app->audio_check = add_control(L"BUTTON", L"共享系统声音", BS_AUTOCHECKBOX, 0, 326, 380,
                                     130, 24, kAudioCheck, nullptr);
    g_app->autostart_check = add_control(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX, 0, 472, 380,
                                         120, 24, kAutostartCheck, nullptr);
    g_app->microphone_check = add_control(L"BUTTON", L"接收查看者麦克风", BS_AUTOCHECKBOX, 0, 24,
                                          408, 200, 24, kMicrophoneCheck, nullptr);
    g_app->privacy_check = add_control(L"BUTTON", L"允许隐私模式", BS_AUTOCHECKBOX, 0, 240, 408,
                                       200, 24, kPrivacyModeCheck, nullptr);
    g_app->carrier_check = add_control(L"BUTTON", L"弱网回退（TCP）", BS_AUTOCHECKBOX, 0, 446, 408,
                                       126, 24, kCarrierCheck, nullptr);

    add_control(L"BUTTON", L"安全设置…", BS_PUSHBUTTON, 0, 24, 444, 116, 30, kSecurityButton,
                nullptr);
    add_control(L"BUTTON", L"连接服务设置…", BS_PUSHBUTTON, 0, 146, 444, 150, 30,
                kTunnelSettingsButton, nullptr);
    add_control(L"BUTTON", L"打开设置文件", BS_PUSHBUTTON, 0, 302, 444, 104, 30,
                kOpenConfigButton, nullptr);
    add_control(L"BUTTON", L"允许防火墙访问…", BS_PUSHBUTTON, 0, 412, 444, 160, 30,
                kFirewallButton, nullptr);

    g_app->toggle_button = add_control(L"BUTTON", L"开始共享", BS_PUSHBUTTON, 0, 24, 482, 110,
                                       32, kToggleButton, nullptr);
    add_control(L"BUTTON", L"重新注册", BS_PUSHBUTTON, 0, 142, 482, 104, 32,
                kTunnelRegisterButton, nullptr);
    add_control(L"BUTTON", L"重新加载设置", BS_PUSHBUTTON, 0, 254, 482, 120, 32, kReloadButton,
                nullptr);
    add_control(L"BUTTON", L"隐藏到托盘", BS_PUSHBUTTON, 0, 382, 482, 110, 32, kHideButton,
                nullptr);
    add_control(L"BUTTON", L"退出", BS_PUSHBUTTON, 0, 500, 482, 80, 32, kQuitButton, nullptr);

    add_control(L"STATIC", L"日志", SS_LEFT, 0, 24, 524, 200, 20, 0, nullptr);
    g_app->log_edit =
        add_control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                    WS_EX_CLIENTEDGE, 24, 544, 572, 150, kLogEdit, nullptr);

    set_check(g_app->input_check, g_app->settings.allow_input);
    set_check(g_app->clipboard_check, g_app->settings.allow_clipboard);
    set_check(g_app->audio_check, g_app->settings.allow_audio);
    set_check(g_app->microphone_check, g_app->settings.allow_microphone);
    set_check(g_app->privacy_check, g_app->settings.allow_privacy_mode);
    set_check(g_app->carrier_check, g_app->settings.carrier);
    set_check(g_app->autostart_check, g_app->settings.autostart);
}

constexpr wchar_t kTunnelDialogClass[] = L"ChastTunnelSettingsWindow";

enum TunnelDialogId {
    kDialogUrl = 2000,
    kDialogServerKey,
    kDialogId,
    kDialogKey,
    kDialogListen,
    kDialogEnabled,
};

struct TunnelDialogState {
    HWND window = nullptr;
    HWND url = nullptr;
    HWND server_key = nullptr;
    HWND id = nullptr;
    HWND key = nullptr;
    HWND listen = nullptr;
    HWND enabled = nullptr;
    bool done = false;
    bool accepted = false;
    HostSettings result;
};

std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::string trim_ascii(std::string text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

constexpr wchar_t kChatWindowClass[] = L"ChastChatWindow";

void chat_refresh_history() {
    if (g_app->chat_history == nullptr) {
        return;
    }
    std::wstring all;
    for (const std::wstring& line : g_app->chat_lines) {
        all += line;
        all += L"\r\n";
    }
    SetWindowTextW(g_app->chat_history, all.c_str());
    SendMessageW(g_app->chat_history, EM_SETSEL, static_cast<WPARAM>(all.size()),
                 static_cast<LPARAM>(all.size()));
    SendMessageW(g_app->chat_history, EM_SCROLLCARET, 0, 0);
}

void append_chat_line(const std::wstring& text) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[16]{};
    swprintf_s(stamp, std::size(stamp), L"[%02u:%02u] ", now.wHour, now.wMinute);
    g_app->chat_lines.push_back(std::wstring(stamp) + text);
    while (g_app->chat_lines.size() > 300) {
        g_app->chat_lines.pop_front();
    }
    chat_refresh_history();
}

void send_chat_from_input();

LRESULT CALLBACK chat_input_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR) {
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        send_chat_from_input();
        return 0;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void send_chat_from_input() {
    if (g_app->server == nullptr) {
        append_chat_line(L"系统：共享没有运行，消息未发送");
        return;
    }
    const std::string text = trim_ascii(narrow(control_text(g_app->chat_input)));
    if (!valid_chat_text(text)) {
        append_chat_line(L"系统：消息不能为空，最长 4096 字节");
        return;
    }
    if (!g_app->server->send_chat(text)) {
        append_chat_line(L"系统：没有正在连接的查看者，或待发队列已满");
        return;
    }
    append_chat_line(L"[主机] " + widen(text));
    SetWindowTextW(g_app->chat_input, L"");
    SetFocus(g_app->chat_input);
}

LRESULT CALLBACK chat_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_COMMAND:
            if (HIWORD(wparam) == 0 && LOWORD(wparam) == kChatSend) {
                send_chat_from_input();
            }
            return 0;
        case WM_CLOSE:
            // Keep the window and its history; a new session clears both.
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            g_app->chat_window = nullptr;
            g_app->chat_history = nullptr;
            g_app->chat_input = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void show_chat_window(bool focus) {
    if (g_app->chat_window == nullptr) {
        static bool class_ready = false;
        if (!class_ready) {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.style = CS_HREDRAW | CS_VREDRAW;
            window_class.lpfnWndProc = chat_dialog_proc;
            window_class.hInstance = g_app->instance;
            window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            window_class.lpszClassName = kChatWindowClass;
            if (RegisterClassExW(&window_class) == 0) {
                append_log_line(L"无法创建聊天窗口");
                return;
            }
            class_ready = true;
        }
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT frame{0, 0, px(520), px(440)};
        AdjustWindowRectEx(&frame, style, FALSE, 0);
        const int width = frame.right - frame.left;
        const int height = frame.bottom - frame.top;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        // Ownerless so it still opens while the main window sits in the tray.
        const HWND window = CreateWindowExW(0, kChatWindowClass, L"chast 会话聊天", style, 0, 0,
                                            width, height, nullptr, nullptr, g_app->instance,
                                            nullptr);
        if (window == nullptr) {
            append_log_line(L"无法创建聊天窗口");
            return;
        }
        g_app->chat_window = window;
        RECT main{};
        if (g_app->window != nullptr) {
            GetWindowRect(g_app->window, &main);
        }
        int x = main.right - width;
        int y = main.top + px(80);
        if (x < work.left) {
            x = work.left + px(16);
        }
        if (x + width > work.right) {
            x = work.right - width;
        }
        if (y + height > work.bottom) {
            y = work.bottom - height;
        }
        if (y < work.top) {
            y = work.top;
        }
        SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        const auto add = [window](const wchar_t* cls, const wchar_t* text, DWORD control_style,
                                  DWORD ex_style, int left, int top, int w, int h, int id) {
            HWND control = CreateWindowExW(
                ex_style, cls, text, WS_CHILD | WS_VISIBLE | control_style, px(left), px(top),
                px(w), px(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                g_app->instance, nullptr);
            if (control != nullptr) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->font), TRUE);
            }
            return control;
        };
        add(L"STATIC", L"会话聊天 · 消息只在本会话中传输", SS_LEFT, 0, 16, 12, 480, 20, 0);
        g_app->chat_history =
            add(L"EDIT", L"",
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                WS_EX_CLIENTEDGE, 16, 38, 472, 310, kChatHistory);
        g_app->chat_input =
            add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 16, 360, 372, 28,
                kChatInput);
        add(L"BUTTON", L"发送", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 398, 358, 90, 32, kChatSend);
        add(L"STATIC", L"按 Enter 发送 · 最多 4096 字节", SS_LEFT, 0, 16, 396, 360, 18, 0);
        SetWindowSubclass(g_app->chat_input, chat_input_proc, 1, 0);
        chat_refresh_history();
    }
    ShowWindow(g_app->chat_window, focus ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    if (focus) {
        SetForegroundWindow(g_app->chat_window);
        SetFocus(g_app->chat_input);
    } else {
        FLASHWINFO flash{};
        flash.cbSize = sizeof(flash);
        flash.hwnd = g_app->chat_window;
        flash.dwFlags = FLASHW_TRAY;
        flash.uCount = 2;
        FlashWindowEx(&flash);
    }
}

bool collect_tunnel_settings(TunnelDialogState& dialog, HostSettings& updated, std::string& error) {
    std::string normalized;
    const std::string url = trim_ascii(narrow(control_text(dialog.url)));
    if (!url.empty()) {
        if (!parse_server_url(url, normalized)) {
            error = "服务器地址无效，需要 http:// 或 https:// 地址";
            return false;
        }
        updated.server_url = normalized;
    } else {
        updated.server_url.clear();
    }

    const std::string server_key = trim_ascii(narrow(control_text(dialog.server_key)));
    if (!valid_server_key(server_key)) {
        error = "服务器密钥需要最多 256 个可打印字符，且不能包含引号或反斜杠";
        return false;
    }
    updated.server_key = server_key;

    const std::string id = trim_ascii(narrow(control_text(dialog.id)));
    if (id.empty()) {
        updated.tunnel_id = updated.tunnel ? generate_tunnel_id() : std::string();
    } else {
        if (!parse_tunnel_id(id, normalized)) {
            error = "设备 ID 需要 1..64 个字母、数字、'.'、'_' 或 '-'，且以字母或数字开头结尾";
            return false;
        }
        updated.tunnel_id = normalized;
    }
    if (updated.tunnel_id != g_app->settings.tunnel_id &&
        !g_app->settings.tunnel_id.empty() && updated.tunnel_old_id.empty()) {
        // Remember the id to release on the server. A pending older id wins so
        // an edit made before the first migration was confirmed still resolves
        // the original record instead of leaking it.
        updated.tunnel_old_id = g_app->settings.tunnel_id;
    }
    if (updated.tunnel_old_id == updated.tunnel_id) {
        updated.tunnel_old_id.clear();
    }

    const std::string key = trim_ascii(narrow(control_text(dialog.key)));
    if (key.empty()) {
        updated.tunnel_key = updated.tunnel ? generate_access_token(16) : std::string();
    } else {
        if (!valid_tunnel_key(key)) {
            error = "设备密钥需要 8..128 个可打印字符，且不能包含引号或反斜杠";
            return false;
        }
        updated.tunnel_key = key;
    }

    const std::string listen = trim_ascii(narrow(control_text(dialog.listen)));
    if (listen.empty()) {
        updated.tunnel_listen = "0.0.0.0:14434";
    } else {
        if (!parse_tunnel_listen(listen, normalized)) {
            error = "隧道监听地址无效，需要 host:port";
            return false;
        }
        updated.tunnel_listen = normalized;
    }
    return validate_tunnel_settings(updated, &error);
}

void accept_tunnel_dialog(TunnelDialogState& dialog) {
    HostSettings updated = g_app->settings;
    updated.tunnel = SendMessageW(dialog.enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::string error;
    if (!collect_tunnel_settings(dialog, updated, error)) {
        MessageBoxW(dialog.window, widen(error).c_str(), L"连接服务设置",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    dialog.result = std::move(updated);
    dialog.accepted = true;
    DestroyWindow(dialog.window);
}

LRESULT CALLBACK tunnel_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog =
        reinterpret_cast<TunnelDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        dialog = static_cast<TunnelDialogState*>(create->lpCreateParams);
        dialog->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        return TRUE;
    }
    if (dialog == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message) {
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                accept_tunnel_dialog(*dialog);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            dialog->done = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool edit_tunnel_settings(HostSettings& result) {
    static bool class_ready = false;
    if (!class_ready) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = tunnel_dialog_proc;
        window_class.hInstance = g_app->instance;
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kTunnelDialogClass;
        if (RegisterClassExW(&window_class) == 0) {
            return false;
        }
        class_ready = true;
    }

    TunnelDialogState dialog;
    RECT owner_rect{};
    GetWindowRect(g_app->window, &owner_rect);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT frame{0, 0, px(540), px(470)};
    AdjustWindowRectEx(&frame, style, FALSE, WS_EX_DLGMODALFRAME);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kTunnelDialogClass, L"连接服务设置",
                                  style, x, y, width, height, g_app->window, nullptr,
                                  g_app->instance, &dialog);
    if (window == nullptr) {
        return false;
    }

    const auto add = [&dialog](const wchar_t* cls, const wchar_t* text, DWORD control_style,
                               DWORD ex_style, int left, int top, int w, int h, int id,
                               HFONT font) {
        HWND control = CreateWindowExW(
            ex_style, cls, text, WS_CHILD | WS_VISIBLE | control_style, px(left), px(top),
            px(w), px(h), dialog.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app->instance, nullptr);
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(font != nullptr ? font : g_app->font), TRUE);
        }
        return control;
    };

    add(L"STATIC",
        L"服务器地址留空表示不注册，只在本机或局域网共享。设备密钥不会上传，"
        L"服务端只保存它的摘要。",
        SS_LEFT, 0, 20, 14, 500, 40, 0, nullptr);
    add(L"STATIC", L"服务器地址（http:// 或 https://）", SS_LEFT, 0, 20, 64, 400, 20, 0,
        nullptr);
    dialog.url = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 86, 500,
                     26, kDialogUrl, nullptr);
    add(L"STATIC", L"服务器密钥（可选）", SS_LEFT, 0, 20, 122, 400, 20, 0, nullptr);
    dialog.server_key = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20,
                            144, 500, 26, kDialogServerKey, nullptr);
    add(L"STATIC", L"设备 ID（会合服务中的名称）", SS_LEFT, 0, 20, 180, 400, 20, 0, nullptr);
    dialog.id = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 202, 500,
                    26, kDialogId, nullptr);
    add(L"STATIC", L"设备密钥（8 个字符以上，不会被上传）", SS_LEFT, 0, 20, 238, 400, 20, 0,
        nullptr);
    dialog.key = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 260, 500,
                     26, kDialogKey, nullptr);
    add(L"STATIC", L"隧道监听地址（UDP）", SS_LEFT, 0, 20, 296, 240, 20, 0, nullptr);
    dialog.listen = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 318,
                        240, 26, kDialogListen, nullptr);
    const std::wstring port_hint =
        L"主机 QUIC 端口：" + std::to_wstring(g_app->settings.port) +
        L"\n隧道端口必须不同，例如 0.0.0.0:14434";
    add(L"STATIC", port_hint.c_str(), SS_LEFT, 0, 276, 316, 250, 40, 0, nullptr);
    dialog.enabled =
        add(L"BUTTON", L"启用连接服务（注册到会合服务器）", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 20,
            366, 400, 24, kDialogEnabled, nullptr);
    add(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 300, 408, 100, 32, IDOK,
        nullptr);
    add(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 0, 410, 408, 100, 32, IDCANCEL,
        nullptr);

    set_text(dialog.url, widen(g_app->settings.server_url));
    set_text(dialog.server_key, widen(g_app->settings.server_key));
    set_text(dialog.id, widen(g_app->settings.tunnel_id));
    set_text(dialog.key, widen(g_app->settings.tunnel_key));
    set_text(dialog.listen, widen(g_app->settings.tunnel_listen));
    set_check(dialog.enabled, g_app->settings.tunnel);

    EnableWindow(g_app->window, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(dialog.url);

    MSG message{};
    bool quit = false;
    while (!dialog.done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            quit = got == 0;
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(g_app->window, TRUE);
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    SetForegroundWindow(g_app->window);
    if (quit) {
        PostQuitMessage(static_cast<int>(message.wParam));
    }
    if (dialog.accepted) {
        result = dialog.result;
    }
    return dialog.accepted;
}

constexpr wchar_t kSecurityDialogClass[] = L"ChastSecurityWindow";

enum SecurityDialogId {
    kSecTab = 5000,
    kSecModePassword,
    kSecModeClick,
    kSecModeBoth,
    kSecPasswordCurrent,
    kSecPasswordNew,
    kSecPasswordConfirm,
    kSecTwoFactorState,
    kSecTwoFactorEnable,
    kSecTwoFactorDisable,
    kSecTotpSecret,
    kSecTotpUri,
    kSecTotpCode,
    kSecTotpConfirm,
    kSecTrustedList,
    kSecRevoke,
    kSecWhitelist,
    kSecAllowInput,
    kSecAllowClipboard,
    kSecAllowFile,
    kSecAllowAudio,
    kSecAllowMicrophone,
    kSecAllowPrivacy,
    kSecAllowPortForward,
    kSecAllowTerminal,
    kSecAllowChange,
    kSecTokenAuth,
    kSecOneTime,
    kSecTokenEdit,
    kSecTokenCopy,
    kSecTokenRotate,
    kSecAudioDevice,
    kSecAudioRefresh,
    kSecAudioAec,
    kSecSave,
    kSecCancel,
};

struct SecurityDialogState {
    HWND window = nullptr;
    HWND tab = nullptr;
    HWND mode_password = nullptr;
    HWND mode_click = nullptr;
    HWND mode_both = nullptr;
    HWND password_current = nullptr;
    HWND password_new = nullptr;
    HWND password_confirm = nullptr;
    HWND two_factor_state = nullptr;
    HWND two_factor_enable = nullptr;
    HWND two_factor_disable = nullptr;
    HWND totp_secret = nullptr;
    HWND totp_uri = nullptr;
    HWND totp_code = nullptr;
    HWND totp_confirm = nullptr;
    HWND trusted_list = nullptr;
    HWND whitelist = nullptr;
    HWND allow_input = nullptr;
    HWND allow_clipboard = nullptr;
    HWND allow_file = nullptr;
    HWND allow_port_forward = nullptr;
    HWND allow_terminal = nullptr;
    HWND allow_audio = nullptr;
    HWND allow_microphone = nullptr;
    HWND allow_privacy = nullptr;
    HWND allow_change = nullptr;
    HWND token_auth = nullptr;
    HWND one_time = nullptr;
    HWND token_edit = nullptr;
    HWND audio_device = nullptr;
    HWND audio_aec = nullptr;
    // Parallel to the combo box: index 0 is the system default.
    std::vector<std::string> audio_device_ids;
    std::string selected_audio_device;
    std::vector<HWND> auth_page;
    std::vector<HWND> access_page;
    std::vector<HWND> advanced_page;
    std::vector<TrustedDevice> devices;
    std::vector<std::vector<uint8_t>> revoked;
    // A generated but not yet protected secret; empty when 2FA is untouched.
    std::string pending_totp;
    bool pending_totp_confirmed = false;
    bool two_factor_disable_pending = false;
    bool token_edited = false;
    bool done = false;
    bool accepted = false;
    HostSettings result;
};

bool is_checked(HWND control) {
    return control != nullptr &&
           SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

std::wstring unix_date(int64_t seconds) {
    if (seconds <= 0) {
        return L"从未";
    }
    const time_t stamp = static_cast<time_t>(seconds);
    tm local{};
    if (localtime_s(&local, &stamp) != 0) {
        return L"—";
    }
    wchar_t buffer[32]{};
    swprintf_s(buffer, std::size(buffer), L"%04d-%02d-%02d", local.tm_year + 1900,
               local.tm_mon + 1, local.tm_mday);
    return buffer;
}

std::string format_ipv4_range(const Ipv4Range& range) {
    std::string text = std::to_string((range.address >> 24) & 0xFF) + "." +
                       std::to_string((range.address >> 16) & 0xFF) + "." +
                       std::to_string((range.address >> 8) & 0xFF) + "." +
                       std::to_string(range.address & 0xFF);
    text += "/" + std::to_string(range.prefix);
    return text;
}

void refresh_trusted_list(SecurityDialogState& dialog) {
    SendMessageW(dialog.trusted_list, LB_RESETCONTENT, 0, 0);
    for (const TrustedDevice& device : dialog.devices) {
        std::wstring line = device.name.empty() ? std::wstring(L"未命名设备")
                                                : widen(device.name);
        line += L" · " + platform_label(device.platform);
        const int64_t seen =
            device.last_seen_unix_seconds != 0 ? device.last_seen_unix_seconds
                                               : device.added_unix_seconds;
        line += L" · 最近使用 " + unix_date(seen);
        SendMessageW(dialog.trusted_list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(line.c_str()));
    }
}

void show_security_page(SecurityDialogState& dialog, int page) {
    if (page < 0 || page > 2) {
        page = 0;
    }
    const auto show = [](const std::vector<HWND>& controls, bool visible) {
        for (HWND control : controls) {
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        }
    };
    show(dialog.auth_page, page == 0);
    show(dialog.access_page, page == 1);
    show(dialog.advanced_page, page == 2);
}

void refresh_audio_devices(SecurityDialogState& dialog) {
    if (dialog.audio_device == nullptr) {
        return;
    }
    std::string selected = dialog.selected_audio_device;
    const LRESULT current =
        SendMessageW(dialog.audio_device, CB_GETCURSEL, 0, 0);
    if (current >= 0 && static_cast<size_t>(current) < dialog.audio_device_ids.size()) {
        selected = dialog.audio_device_ids[static_cast<size_t>(current)];
    }

    SendMessageW(dialog.audio_device, CB_RESETCONTENT, 0, 0);
    dialog.audio_device_ids.clear();
    SendMessageW(dialog.audio_device, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"系统默认（跟随 Windows）"));
    dialog.audio_device_ids.emplace_back();
    int chosen = 0;

    std::string error;
    for (const AudioOutputDevice& output : list_audio_outputs(&error)) {
        if (output.id.empty()) {
            continue;
        }
        std::wstring label = widen(output.name.empty() ? output.id : output.name);
        if (output.is_default) {
            label += L"（默认）";
        }
        const int index = static_cast<int>(dialog.audio_device_ids.size());
        SendMessageW(dialog.audio_device, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
        dialog.audio_device_ids.push_back(output.id);
        if (output.id == selected) {
            chosen = index;
        }
    }
    // A saved id that is not plugged in right now stays selectable, so opening
    // and saving this page never silently moves the session to another output.
    if (!selected.empty() &&
        dialog.audio_device_ids[static_cast<size_t>(chosen)] != selected) {
        SendMessageW(dialog.audio_device, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"已选设备（当前不可用）"));
        chosen = static_cast<int>(dialog.audio_device_ids.size());
        dialog.audio_device_ids.push_back(selected);
    }
    dialog.selected_audio_device = dialog.audio_device_ids[static_cast<size_t>(chosen)];
    SendMessageW(dialog.audio_device, CB_SETCURSEL, chosen, 0);
}

void security_enable_two_factor(SecurityDialogState& dialog) {
    if (!dialog.pending_totp.empty()) {
        return;
    }
    dialog.pending_totp = generate_totp_secret();
    dialog.pending_totp_confirmed = false;
    dialog.two_factor_disable_pending = false;
    set_text(dialog.totp_secret, widen(dialog.pending_totp));
    set_text(dialog.totp_uri,
             widen(totp_uri(dialog.pending_totp, "chast", "chast@host")));
    set_text(dialog.two_factor_state, L"待验证：把密钥加入认证器，再输入 6 位验证码");
    set_text(dialog.totp_code, L"");
    EnableWindow(dialog.totp_code, TRUE);
    EnableWindow(dialog.totp_confirm, TRUE);
    EnableWindow(dialog.two_factor_enable, FALSE);
    EnableWindow(dialog.two_factor_disable, FALSE);
    SetFocus(dialog.totp_code);
}

void security_disable_two_factor(SecurityDialogState& dialog) {
    if (!g_app->settings.password_hash.empty()) {
        const std::wstring current = control_text(dialog.password_current);
        if (current.empty() ||
            !verify_password(narrow(current), g_app->settings.password_hash)) {
            MessageBoxW(dialog.window, L"关闭两步验证需要先输入当前密码。", L"安全设置",
                        MB_OK | MB_ICONWARNING);
            SetFocus(dialog.password_current);
            return;
        }
    }
    dialog.two_factor_disable_pending = true;
    dialog.pending_totp.clear();
    dialog.pending_totp_confirmed = false;
    set_text(dialog.two_factor_state, L"已标记关闭，点击“保存”生效");
    set_text(dialog.totp_secret, L"");
    set_text(dialog.totp_uri, L"");
    set_text(dialog.totp_code, L"");
    EnableWindow(dialog.totp_code, FALSE);
    EnableWindow(dialog.totp_confirm, FALSE);
    EnableWindow(dialog.two_factor_enable, TRUE);
    EnableWindow(dialog.two_factor_disable, FALSE);
}

void security_confirm_totp(SecurityDialogState& dialog) {
    if (dialog.pending_totp.empty()) {
        return;
    }
    const std::wstring code = control_text(dialog.totp_code);
    if (code.empty() || !verify_totp(dialog.pending_totp, narrow(code),
                                     static_cast<uint64_t>(std::time(nullptr)), 1)) {
        MessageBoxW(dialog.window,
                    L"验证码不正确，请按认证器当前显示的 6 位数字重新输入。", L"安全设置",
                    MB_OK | MB_ICONWARNING);
        SendMessageW(dialog.totp_code, EM_SETSEL, 0, -1);
        SetFocus(dialog.totp_code);
        return;
    }
    dialog.pending_totp_confirmed = true;
    set_text(dialog.two_factor_state, L"验证码正确，点击“保存”启用");
    EnableWindow(dialog.totp_code, FALSE);
    EnableWindow(dialog.totp_confirm, FALSE);
}

bool accept_security_dialog(SecurityDialogState& dialog) {
    const auto complain = [&dialog](const std::wstring& text) {
        MessageBoxW(dialog.window, text.c_str(), L"安全设置", MB_OK | MB_ICONWARNING);
        return false;
    };

    HostSettings updated = g_app->settings;
    if (is_checked(dialog.mode_click)) {
        updated.auth_mode = HostAuthMode::Click;
    } else if (is_checked(dialog.mode_both)) {
        updated.auth_mode = HostAuthMode::Both;
    } else {
        updated.auth_mode = HostAuthMode::Password;
    }
    updated.require_approval = updated.auth_mode != HostAuthMode::Password;
    updated.allow_input = is_checked(dialog.allow_input);
    updated.allow_clipboard = is_checked(dialog.allow_clipboard);
    updated.allow_file_transfer = is_checked(dialog.allow_file);
    updated.allow_port_forward = is_checked(dialog.allow_port_forward);
    updated.allow_terminal = is_checked(dialog.allow_terminal);
    updated.allow_audio = is_checked(dialog.allow_audio);
    updated.allow_microphone = is_checked(dialog.allow_microphone);
    updated.allow_privacy_mode = is_checked(dialog.allow_privacy);
    updated.allow_permission_change = is_checked(dialog.allow_change);

    std::string whitelist = narrow(control_text(dialog.whitelist));
    std::replace(whitelist.begin(), whitelist.end(), '\r', ',');
    std::replace(whitelist.begin(), whitelist.end(), '\n', ',');
    std::vector<Ipv4Range> ranges;
    std::string whitelist_error;
    if (!parse_ip_whitelist(whitelist, ranges, &whitelist_error)) {
        return complain(L"IP 白名单无效：" + widen(whitelist_error));
    }
    std::string normalized;
    for (const Ipv4Range& range : ranges) {
        if (!normalized.empty()) {
            normalized += ",";
        }
        normalized += format_ipv4_range(range);
    }
    updated.ip_whitelist = normalized;

    const std::wstring current = control_text(dialog.password_current);
    const std::wstring next = control_text(dialog.password_new);
    const std::wstring confirm = control_text(dialog.password_confirm);
    if (!next.empty() || !confirm.empty()) {
        if (next.size() < kMinPermanentPasswordLength) {
            return complain(L"新密码至少需要 8 个字符。");
        }
        if (next != confirm) {
            return complain(L"两次输入的新密码不一致。");
        }
        const std::string encoded = narrow(next);
        if (encoded.size() > 128) {
            return complain(L"新密码太长（最多 128 字节）。");
        }
        if (!updated.password_hash.empty() &&
            (current.empty() || !verify_password(narrow(current), updated.password_hash))) {
            return complain(L"当前密码不正确。");
        }
        updated.password_hash = hash_password(encoded);
        // A device trusted under the old secret must prove itself again.
        updated.trusted_devices.clear();
    }

    if (dialog.two_factor_disable_pending) {
        updated.totp_secret.clear();
    } else if (!dialog.pending_totp.empty()) {
        if (!dialog.pending_totp_confirmed) {
            return complain(L"请先用认证器输入 6 位验证码并点击“确认验证码”。");
        }
        std::string protected_secret;
        std::string error;
        if (!protect_totp_secret(dialog.pending_totp, SecretScope::CurrentUser,
                                 protected_secret, &error)) {
            return complain(L"无法保护两步验证密钥：" + widen(error));
        }
        updated.totp_secret = std::move(protected_secret);
    }

    if (!dialog.revoked.empty()) {
        std::vector<TrustedDevice>& devices = updated.trusted_devices;
        devices.erase(
            std::remove_if(devices.begin(), devices.end(),
                           [&dialog](const TrustedDevice& device) {
                               return std::find(dialog.revoked.begin(), dialog.revoked.end(),
                                                device.public_key) != dialog.revoked.end();
                           }),
            devices.end());
    }

    if (dialog.audio_device != nullptr) {
        const LRESULT selected = SendMessageW(dialog.audio_device, CB_GETCURSEL, 0, 0);
        updated.audio_device =
            selected >= 0 &&
                    static_cast<size_t>(selected) < dialog.audio_device_ids.size()
                ? dialog.audio_device_ids[static_cast<size_t>(selected)]
                : dialog.selected_audio_device;
    }
    updated.audio_aec = is_checked(dialog.audio_aec);

    updated.token_auth = is_checked(dialog.token_auth);
    updated.token_mode = is_checked(dialog.one_time) ? HostTokenMode::OneTime
                                                     : HostTokenMode::Reusable;
    if (dialog.token_edited) {
        const std::string token = trim_ascii(narrow(control_text(dialog.token_edit)));
        if (token.empty() || token.size() > 128) {
            return complain(L"访问令牌不能为空，且不能超过 128 个字符。");
        }
        updated.access_token = token;
    }

    dialog.result = std::move(updated);
    dialog.accepted = true;
    DestroyWindow(dialog.window);
    return true;
}

LRESULT CALLBACK security_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog =
        reinterpret_cast<SecurityDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        dialog = static_cast<SecurityDialogState*>(create->lpCreateParams);
        dialog->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        return TRUE;
    }
    if (dialog == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kSecSave:
                    accept_security_dialog(*dialog);
                    return 0;
                case kSecCancel:
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
                case kSecTwoFactorEnable:
                    security_enable_two_factor(*dialog);
                    return 0;
                case kSecTwoFactorDisable:
                    security_disable_two_factor(*dialog);
                    return 0;
                case kSecTotpConfirm:
                    security_confirm_totp(*dialog);
                    return 0;
                case kSecTokenCopy:
                    chast::host::copy_text(dialog->window,
                                           control_text(dialog->token_edit));
                    return 0;
                case kSecTokenRotate:
                    set_text(dialog->token_edit, widen(generate_access_token()));
                    dialog->token_edited = true;
                    return 0;
                case kSecAudioRefresh: {
                    const LRESULT selected =
                        SendMessageW(dialog->audio_device, CB_GETCURSEL, 0, 0);
                    if (selected >= 0 &&
                        static_cast<size_t>(selected) < dialog->audio_device_ids.size()) {
                        dialog->selected_audio_device =
                            dialog->audio_device_ids[static_cast<size_t>(selected)];
                    }
                    refresh_audio_devices(*dialog);
                    return 0;
                }
                case kSecRevoke: {
                    const LRESULT selected =
                        SendMessageW(dialog->trusted_list, LB_GETCURSEL, 0, 0);
                    if (selected == LB_ERR ||
                        static_cast<size_t>(selected) >= dialog->devices.size()) {
                        return 0;
                    }
                    dialog->revoked.push_back(
                        dialog->devices[static_cast<size_t>(selected)].public_key);
                    dialog->devices.erase(dialog->devices.begin() + selected);
                    refresh_trusted_list(*dialog);
                    return 0;
                }
                default:
                    break;
            }
            break;
        case WM_NOTIFY: {
            if (lparam == 0) {
                break;
            }
            const auto* header = reinterpret_cast<const NMHDR*>(lparam);
            if (dialog->tab == nullptr || header->hwndFrom != dialog->tab ||
                header->idFrom != kSecTab || header->code != TCN_SELCHANGE) {
                break;
            }
            // Showing or hiding controls from inside the tab control's own
            // notification re-enters COMCTL32 mid-callback; defer the page
            // switch until the notification has fully unwound.
            PostMessageW(hwnd, kSecurityPageMessage, 0, 0);
            return 0;
        }
        case kSecurityPageMessage:
            if (dialog->tab != nullptr) {
                const int page = TabCtrl_GetCurSel(dialog->tab);
                if (page >= 0) {
                    show_security_page(*dialog, page);
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            dialog->done = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool edit_security_settings(HostSettings& result) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&controls);

    static bool class_ready = false;
    if (!class_ready) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = security_dialog_proc;
        window_class.hInstance = g_app->instance;
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kSecurityDialogClass;
        if (RegisterClassExW(&window_class) == 0) {
            return false;
        }
        class_ready = true;
    }

    SecurityDialogState dialog;
    if (g_app->server) {
        dialog.devices = g_app->server->trusted_devices();
    } else {
        dialog.devices = g_app->settings.trusted_devices;
    }

    RECT owner_rect{};
    GetWindowRect(g_app->window, &owner_rect);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT frame{0, 0, px(620), px(600)};
    AdjustWindowRectEx(&frame, style, FALSE, WS_EX_DLGMODALFRAME);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kSecurityDialogClass, L"安全设置",
                                  style, x, y, width, height, g_app->window, nullptr,
                                  g_app->instance, &dialog);
    if (window == nullptr) {
        return false;
    }

    const auto add = [&dialog](const wchar_t* cls, const wchar_t* text, DWORD control_style,
                               DWORD ex_style, int left, int top, int w, int h, int id,
                               HFONT font, std::vector<HWND>* page) {
        const DWORD visible = page == nullptr ? WS_VISIBLE : 0;
        HWND control = CreateWindowExW(
            ex_style, cls, text, WS_CHILD | visible | control_style, px(left), px(top), px(w),
            px(h), dialog.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            g_app->instance, nullptr);
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(font != nullptr ? font : g_app->font), TRUE);
            if (page != nullptr) {
                page->push_back(control);
            }
        }
        return control;
    };

    dialog.tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(16), px(12), px(588),
                                 px(520), window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSecTab)),
                                 g_app->instance, nullptr);
    if (dialog.tab != nullptr) {
        SendMessageW(dialog.tab, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->font), TRUE);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(L"认证");
        TabCtrl_InsertItem(dialog.tab, 0, &item);
        item.pszText = const_cast<wchar_t*>(L"访问控制");
        TabCtrl_InsertItem(dialog.tab, 1, &item);
        item.pszText = const_cast<wchar_t*>(L"高级");
        TabCtrl_InsertItem(dialog.tab, 2, &item);
    }

    // ----- authentication page -----
    add(L"STATIC", L"接入方式", SS_LEFT, 0, 32, 56, 200, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.mode_password =
        add(L"BUTTON", L"密码", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 0, 32, 78, 110, 24,
            kSecModePassword, nullptr, &dialog.auth_page);
    dialog.mode_click =
        add(L"BUTTON", L"点击批准", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 150, 78, 130, 24,
            kSecModeClick, nullptr, &dialog.auth_page);
    dialog.mode_both =
        add(L"BUTTON", L"两者", BS_AUTORADIOBUTTON | WS_TABSTOP, 0, 290, 78, 130, 24,
            kSecModeBoth, nullptr, &dialog.auth_page);
    add(L"STATIC",
        L"密码：必须输入密码；点击批准：每次连接都弹窗；两者：有密码就验证，"
        L"空密码请求批准。",
        SS_LEFT, 0, 32, 104, 540, 36, 0, nullptr, &dialog.auth_page);
    add(L"STATIC", L"修改永久密码（留空表示不修改）", SS_LEFT, 0, 32, 144, 300, 20, 0,
        nullptr, &dialog.auth_page);
    add(L"STATIC", L"当前密码（修改时需要）", SS_LEFT, 0, 32, 168, 170, 20, 0, nullptr,
        &dialog.auth_page);
    add(L"STATIC", L"新密码（至少 8 个字符）", SS_LEFT, 0, 214, 168, 170, 20, 0, nullptr,
        &dialog.auth_page);
    add(L"STATIC", L"确认新密码", SS_LEFT, 0, 396, 168, 170, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.password_current =
        add(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 188,
            170, 26, kSecPasswordCurrent, nullptr, &dialog.auth_page);
    dialog.password_new =
        add(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 214,
            188, 170, 26, kSecPasswordNew, nullptr, &dialog.auth_page);
    dialog.password_confirm =
        add(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 396,
            188, 170, 26, kSecPasswordConfirm, nullptr, &dialog.auth_page);
    add(L"STATIC", L"修改永久密码会清空可信设备；两步验证密钥保留。", SS_LEFT, 0, 32, 220,
        540, 20, 0, nullptr, &dialog.auth_page);
    add(L"STATIC", L"两步验证（TOTP）", SS_LEFT, 0, 32, 248, 200, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.two_factor_state =
        add(L"STATIC", L"", SS_LEFT, 0, 300, 248, 270, 20, kSecTwoFactorState, nullptr,
            &dialog.auth_page);
    dialog.two_factor_enable =
        add(L"BUTTON", L"启用两步验证", BS_PUSHBUTTON | WS_TABSTOP, 0, 32, 272, 120, 28,
            kSecTwoFactorEnable, nullptr, &dialog.auth_page);
    dialog.two_factor_disable =
        add(L"BUTTON", L"关闭两步验证", BS_PUSHBUTTON | WS_TABSTOP, 0, 162, 272, 120, 28,
            kSecTwoFactorDisable, nullptr, &dialog.auth_page);
    add(L"STATIC", L"密钥（Base32，手动添加到认证器）", SS_LEFT, 0, 32, 308, 400, 20, 0,
        nullptr, &dialog.auth_page);
    dialog.totp_secret =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 326, 540, 26,
            kSecTotpSecret, nullptr, &dialog.auth_page);
    add(L"STATIC", L"otpauth 链接", SS_LEFT, 0, 32, 358, 400, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.totp_uri =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 376, 540, 26,
            kSecTotpUri, nullptr, &dialog.auth_page);
    add(L"STATIC", L"6 位验证码", SS_LEFT, 0, 32, 408, 100, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.totp_code =
        add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 426, 120, 26,
            kSecTotpCode, nullptr, &dialog.auth_page);
    dialog.totp_confirm =
        add(L"BUTTON", L"确认验证码", BS_PUSHBUTTON | WS_TABSTOP, 0, 164, 426, 110, 26,
            kSecTotpConfirm, nullptr, &dialog.auth_page);
    add(L"STATIC", L"输入认证器当前显示的 6 位数字", SS_LEFT, 0, 286, 430, 280, 20, 0,
        nullptr, &dialog.auth_page);

    // ----- access-control page -----
    add(L"STATIC", L"可信设备（通过两步验证后可以记住这台查看器）", SS_LEFT, 0, 32, 56, 500,
        20, 0, nullptr, &dialog.access_page);
    dialog.trusted_list =
        add(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP | WS_BORDER, 0, 32, 78,
            440, 110, kSecTrustedList, nullptr, &dialog.access_page);
    add(L"BUTTON", L"撤销信任", BS_PUSHBUTTON | WS_TABSTOP, 0, 484, 78, 92, 30, kSecRevoke,
        nullptr, &dialog.access_page);
    add(L"STATIC", L"设备 90 天未使用自动失效；修改永久密码会清空列表。", SS_LEFT, 0, 32,
        196, 540, 20, 0, nullptr, &dialog.access_page);
    add(L"STATIC", L"IP 白名单（IPv4 或 CIDR，逗号或换行分隔，留空不限制）", SS_LEFT, 0,
        32, 224, 540, 20, 0, nullptr, &dialog.access_page);
    dialog.whitelist =
        add(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
            WS_EX_CLIENTEDGE, 32, 246, 544, 64, kSecWhitelist, nullptr,
            &dialog.access_page);
    add(L"STATIC", L"经中继连接时，主机看到的是中继地址。", SS_LEFT, 0, 32, 316, 540, 20,
        0, nullptr, &dialog.access_page);
    add(L"STATIC", L"新连接的默认权限", SS_LEFT, 0, 32, 344, 300, 20, 0, nullptr,
        &dialog.access_page);
    dialog.allow_input =
        add(L"BUTTON", L"键盘与鼠标", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 366, 120, 24,
            kSecAllowInput, nullptr, &dialog.access_page);
    dialog.allow_clipboard =
        add(L"BUTTON", L"剪贴板", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 160, 366, 110, 24,
            kSecAllowClipboard, nullptr, &dialog.access_page);
    dialog.allow_audio =
        add(L"BUTTON", L"系统声音", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 276, 366, 110, 24,
            kSecAllowAudio, nullptr, &dialog.access_page);
    dialog.allow_file =
        add(L"BUTTON", L"文件传输", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 394, 110, 24,
            kSecAllowFile, nullptr, &dialog.access_page);
    dialog.allow_microphone =
        add(L"BUTTON", L"麦克风（上行）", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 150, 394, 130, 24,
            kSecAllowMicrophone, nullptr, &dialog.access_page);
    dialog.allow_privacy =
        add(L"BUTTON", L"隐私模式（本机黑屏）", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 400, 366,
            160, 24, kSecAllowPrivacy, nullptr, &dialog.access_page);
    const std::string receive_directory = g_app->settings.file_receive_directory.empty()
                                              ? chast::default_receive_directory()
                                              : g_app->settings.file_receive_directory;
    add(L"STATIC",
        (L"接收目录：" +
         (receive_directory.empty() ? std::wstring(L"（不可用）") : widen(receive_directory)))
            .c_str(),
        SS_LEFT | SS_ENDELLIPSIS, 0, 290, 398, 290, 20, 0, nullptr, &dialog.access_page);
    dialog.allow_port_forward =
        add(L"BUTTON", L"端口转发", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 428, 120, 24,
            kSecAllowPortForward, nullptr, &dialog.access_page);
    add(L"STATIC", L"允许查看器经本机访问你可达的 TCP 地址。", SS_LEFT, 0, 150, 432, 430, 20, 0,
        nullptr, &dialog.access_page);
    dialog.allow_change =
        add(L"BUTTON", L"允许在会话中调整权限", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 460, 260,
            24, kSecAllowChange, nullptr, &dialog.access_page);
    dialog.allow_terminal =
        add(L"BUTTON", L"远程终端", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 488, 120, 24,
            kSecAllowTerminal, nullptr, &dialog.access_page);
    add(L"STATIC", L"允许查看器在本机打开 shell（使用当前账户）。", SS_LEFT, 0, 160, 492,
        420, 20, 0, nullptr, &dialog.access_page);

    // ----- advanced page -----
    add(L"STATIC", L"访问令牌（仅供 CLI 与自动化，默认关闭）", SS_LEFT, 0, 32, 56, 500, 20, 0,
        nullptr, &dialog.advanced_page);
    dialog.token_auth =
        add(L"BUTTON", L"允许令牌认证", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 80, 200, 24,
            kSecTokenAuth, nullptr, &dialog.advanced_page);
    dialog.one_time =
        add(L"BUTTON", L"一次性令牌（使用后自动更换）", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32,
            106, 260, 24, kSecOneTime, nullptr, &dialog.advanced_page);
    dialog.token_edit =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 134, 408, 26,
            kSecTokenEdit, nullptr, &dialog.advanced_page);
    add(L"BUTTON", L"复制", BS_PUSHBUTTON | WS_TABSTOP, 0, 448, 134, 60, 26, kSecTokenCopy,
        nullptr, &dialog.advanced_page);
    add(L"BUTTON", L"生成", BS_PUSHBUTTON | WS_TABSTOP, 0, 514, 134, 62, 26, kSecTokenRotate,
        nullptr, &dialog.advanced_page);
    add(L"STATIC", L"查看器不再使用访问令牌；只有勾选“允许令牌认证”后令牌才有效。",
        SS_LEFT, 0, 32, 166, 540, 36, 0, nullptr, &dialog.advanced_page);
    add(L"STATIC", L"身份指纹（SHA-256）", SS_LEFT, 0, 32, 212, 300, 20, 0, nullptr,
        &dialog.advanced_page);
    HWND fingerprint =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 234, 544, 26,
            0, nullptr, &dialog.advanced_page);
    add(L"STATIC", L"查看器首次连接时固定此指纹；变更后会提示身份不匹配。", SS_LEFT, 0, 32,
        266, 540, 20, 0, nullptr, &dialog.advanced_page);

    add(L"STATIC", L"系统声音输出（捕获与查看者麦克风共用）", SS_LEFT, 0, 32, 300, 500, 20,
        0, nullptr, &dialog.advanced_page);
    dialog.audio_device = add(L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                              WS_EX_CLIENTEDGE, 32, 322, 460, 200, kSecAudioDevice, nullptr,
                              &dialog.advanced_page);
    add(L"BUTTON", L"刷新", BS_PUSHBUTTON | WS_TABSTOP, 0, 500, 321, 76, 28,
        kSecAudioRefresh, nullptr, &dialog.advanced_page);
    dialog.audio_aec =
        add(L"BUTTON", L"回声消除（以查看者麦克风播放为参考）",
            BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 358, 400, 24, kSecAudioAec, nullptr,
            &dialog.advanced_page);
    add(L"STATIC", L"留空跟随系统默认；固定设备不可用时系统声音不会启动。", SS_LEFT, 0, 32,
        386, 540, 20, 0, nullptr, &dialog.advanced_page);

    add(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 410, 548, 90, 32, kSecSave,
        nullptr, nullptr);
    add(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 0, 510, 548, 90, 32, kSecCancel,
        nullptr, nullptr);

    // ----- initial state -----
    const HostSettings& settings = g_app->settings;
    SendMessageW(settings.auth_mode == HostAuthMode::Click ? dialog.mode_click
                                                           : settings.auth_mode ==
                                                                     HostAuthMode::Password
                                                                 ? dialog.mode_password
                                                                 : dialog.mode_both,
                 BM_SETCHECK, BST_CHECKED, 0);
    set_text(dialog.whitelist, widen(settings.ip_whitelist));
    set_check(dialog.allow_input, settings.allow_input);
    set_check(dialog.allow_clipboard, settings.allow_clipboard);
    set_check(dialog.allow_file, settings.allow_file_transfer);
    set_check(dialog.allow_port_forward, settings.allow_port_forward);
    set_check(dialog.allow_terminal, settings.allow_terminal);
    set_check(dialog.allow_audio, settings.allow_audio);
    set_check(dialog.allow_microphone, settings.allow_microphone);
    set_check(dialog.allow_privacy, settings.allow_privacy_mode);
    set_check(dialog.allow_change, settings.allow_permission_change);
    set_check(dialog.token_auth, settings.token_auth);
    set_check(dialog.one_time, settings.token_mode == HostTokenMode::OneTime);
    set_text(dialog.token_edit, widen(settings.access_token));
    set_text(fingerprint,
             g_app->server ? widen(format_fingerprint(g_app->server->status().fingerprint))
                           : std::wstring(L"共享未启动"));
    dialog.selected_audio_device = settings.audio_device;
    set_check(dialog.audio_aec, settings.audio_aec);
    refresh_audio_devices(dialog);

    if (!settings.totp_secret.empty()) {
        set_text(dialog.two_factor_state, L"已启用");
        set_text(dialog.totp_secret, L"（已启用；密钥只在设置时显示）");
        EnableWindow(dialog.two_factor_enable, FALSE);
        EnableWindow(dialog.two_factor_disable, TRUE);
    } else {
        set_text(dialog.two_factor_state, L"已关闭");
        set_text(dialog.totp_secret, L"");
        EnableWindow(dialog.two_factor_enable, TRUE);
        EnableWindow(dialog.two_factor_disable, FALSE);
    }
    EnableWindow(dialog.totp_code, FALSE);
    EnableWindow(dialog.totp_confirm, FALSE);
    refresh_trusted_list(dialog);

    if (dialog.tab != nullptr) {
        TabCtrl_SetCurSel(dialog.tab, 0);
    }
    show_security_page(dialog, 0);

    EnableWindow(g_app->window, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(dialog.mode_password);

    MSG message{};
    bool quit = false;
    while (!dialog.done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            quit = got == 0;
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(g_app->window, TRUE);
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    SetForegroundWindow(g_app->window);
    if (quit) {
        PostQuitMessage(static_cast<int>(message.wParam));
    }
    if (dialog.accepted) {
        result = std::move(dialog.result);
    }
    return dialog.accepted;
}

void add_tray_icon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_app->window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(data.szTip, L"chast 主机 · 已停止", _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        g_app->tray_added = true;
        g_app->tray_tip = L"chast 主机 · 已停止";
    }
}

void remove_tray_icon() {
    if (!g_app->tray_added) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_app->window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_app->tray_added = false;
}

void show_window() {
    ShowWindow(g_app->window, SW_SHOWNORMAL);
    SetForegroundWindow(g_app->window);
}

void show_tray_menu() {
    POINT point{};
    GetCursorPos(&point);
    const bool running = g_app->server && g_app->server->running();
    HostStatus status;
    if (g_app->server) {
        status = g_app->server->status();
    }
    const std::wstring line =
        L"状态：" + (g_app->server ? state_text(status.state, status) : std::wstring(L"已停止"));

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kMenuShow, L"显示 chast 主机");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCopyIdButton, L"复制设备 ID");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_input ? MF_CHECKED : 0), kInputCheck,
                L"接受远程输入");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_clipboard ? MF_CHECKED : 0),
                kClipboardCheck, L"同步剪贴板");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_file_transfer ? MF_CHECKED : 0),
                kFileTransferCheck, L"接受文件传输");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_port_forward ? MF_CHECKED : 0),
                kPortForwardCheck, L"允许端口转发（查看器访问本机 TCP）");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_terminal ? MF_CHECKED : 0),
                kTerminalCheck, L"允许远程终端（查看器在本机开 shell）");
    const bool can_send = running && status.state == HostState::Streaming &&
                          (status.session_permissions & kPermissionFileTransfer) != 0;
    AppendMenuW(menu, MF_STRING | (can_send ? 0 : MF_GRAYED), kSendFileButton,
                L"发送文件到查看器…");
    const bool can_chat = running && status.state == HostState::Streaming;
    AppendMenuW(menu, MF_STRING | (can_chat ? 0 : MF_GRAYED), kChatButton,
                L"与查看者聊天…");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_audio ? MF_CHECKED : 0), kAudioCheck,
                L"共享系统声音");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_microphone ? MF_CHECKED : 0),
                kMicrophoneCheck, L"接收查看者麦克风");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_privacy_mode ? MF_CHECKED : 0),
                kPrivacyModeCheck, L"允许查看者开启隐私模式");
    AppendMenuW(menu, MF_STRING | (g_app->settings.carrier ? MF_CHECKED : 0), kCarrierCheck,
                L"弱网回退（TCP/WebSocket）");
    AppendMenuW(menu, MF_STRING | (g_app->settings.autostart ? MF_CHECKED : 0), kAutostartCheck,
                L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSecurityButton, L"安全设置…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, tunnel_text().c_str());
    AppendMenuW(menu, MF_STRING, kTunnelSettingsButton, L"连接服务设置…");
    AppendMenuW(menu, MF_STRING, kTunnelRegisterButton, L"重新注册连接服务");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kToggleButton, running ? L"停止共享" : L"开始共享");
    AppendMenuW(menu, MF_STRING, kOpenConfigButton, L"打开设置文件");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitButton, L"退出");

    SetForegroundWindow(g_app->window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, point.x, point.y, 0,
                   g_app->window, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_app->window, WM_NULL, 0, 0);
}

void reload_settings() {
    stop_server();
    load_settings();
    if (g_app->startup_created) {
        append_log_line(L"已重新创建设置文件：" + widen(g_app->config_path));
    } else {
        append_log_line(L"设置已重新加载");
    }
    if (!g_app->startup_warning.empty()) {
        append_log_line(L"设置文件有警告：" + widen(g_app->startup_warning));
    }
    if (!g_app->startup_save_error.empty()) {
        append_log_line(L"保存设置失败：" + widen(g_app->startup_save_error));
    }
    start_server();
    refresh_ui();
}

bool session_is_live() {
    return g_app->server && g_app->server->running() &&
           g_app->server->status().state == HostState::Streaming;
}

void handle_command(int id) {
    switch (id) {
        case kMenuShow:
            show_window();
            return;
        case kCopyIdButton:
            chast::host::copy_text(g_app->window, widen(g_app->settings.tunnel_id));
            append_log_line(L"设备 ID 已复制到剪贴板");
            break;
        case kCopyPasswordButton: {
            std::string password;
            if (g_app->server) {
                password = g_app->server->status().temporary_password;
            }
            if (password.empty()) {
                append_log_line(L"当前没有临时密码（点击批准模式）");
            } else {
                chast::host::copy_text(g_app->window, widen(password));
                append_log_line(L"临时密码已复制到剪贴板");
            }
            break;
        }
        case kRefreshPasswordButton: {
            if (!g_app->server) {
                append_log_line(L"共享未启动，无法生成临时密码");
                break;
            }
            const std::string password = g_app->server->refresh_temporary_password();
            append_log_line(password.empty() ? std::wstring(L"无法生成临时密码")
                                             : L"已生成新的临时密码 " + widen(password));
            break;
        }
        case kSecurityButton: {
            HostSettings updated;
            if (edit_security_settings(updated)) {
                g_app->settings = updated;
                save_settings();
                restart_server();
                append_log_line(L"安全设置已保存，共享已重新启动");
            }
            break;
        }
        case kInputCheck:
            g_app->settings.allow_input = !g_app->settings.allow_input;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_input
                                    ? L"远程输入已开启（本次会话实时生效）"
                                    : L"远程输入已关闭（本次会话实时生效）");
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_input ? L"远程输入已开启"
                                                            : L"远程输入已关闭");
            }
            break;
        case kClipboardCheck:
            g_app->settings.allow_clipboard = !g_app->settings.allow_clipboard;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_clipboard
                                    ? L"剪贴板同步已开启（本次会话实时生效）"
                                    : L"剪贴板同步已关闭（本次会话实时生效）");
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_clipboard ? L"剪贴板同步已开启"
                                                                : L"剪贴板同步已关闭");
            }
            break;
        case kSendFileButton: {
            if (!session_is_live()) {
                append_log_line(L"没有正在连接的查看者，无法发送文件");
                break;
            }
            wchar_t path[32768]{};
            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = g_app->window;
            dialog.lpstrFilter = L"所有文件\0*.*\0";
            dialog.lpstrFile = path;
            dialog.nMaxFile = static_cast<DWORD>(std::size(path));
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
            if (!GetOpenFileNameW(&dialog)) {
                break;
            }
            std::string error;
            if (!g_app->server->start_file_download(narrow(path), &error)) {
                append_log_line(L"无法发送文件：" + widen(error));
            } else {
                append_log_line(L"已排队发送：" + std::wstring(path));
            }
            break;
        }
        case kChatButton:
            show_chat_window(true);
            break;
        case kFileTransferCheck:
            g_app->settings.allow_file_transfer = !g_app->settings.allow_file_transfer;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_file_transfer
                                    ? L"文件传输已开启（本次会话实时生效）"
                                    : L"文件传输已关闭（本次会话实时生效）");
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_file_transfer ? L"文件传输已开启"
                                                                    : L"文件传输已关闭");
            }
            break;
        case kPortForwardCheck:
            g_app->settings.allow_port_forward = !g_app->settings.allow_port_forward;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_port_forward
                                    ? L"端口转发已开启（本次会话实时生效）"
                                    : L"端口转发已关闭（本次会话实时生效）");
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_port_forward
                                    ? L"端口转发已开启"
                                    : L"端口转发已关闭");
            }
            break;
        case kTerminalCheck:
            g_app->settings.allow_terminal = !g_app->settings.allow_terminal;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_terminal
                                    ? L"远程终端已开启（本次会话实时生效）"
                                    : L"远程终端已关闭（本次会话实时生效）");
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_terminal ? L"远程终端已开启"
                                                               : L"远程终端已关闭");
            }
            break;
        case kAudioCheck:
            g_app->settings.allow_audio = !g_app->settings.allow_audio;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_audio
                                    ? L"系统声音共享已开启（本次会话实时生效）"
                                    : L"系统声音共享已关闭（本次会话实时生效）");
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_audio ? L"系统声音共享已开启"
                                                            : L"系统声音共享已关闭");
            }
            break;
        case kMicrophoneCheck:
            g_app->settings.allow_microphone = !g_app->settings.allow_microphone;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                if (g_app->settings.allow_microphone && !g_app->listener_microphone) {
                    // The transport refuses CMIC on a listener that was created
                    // without the microphone gate; restarting would drop the
                    // viewer, so the change waits for the next connection.
                    append_log_line(
                        L"已开启接收查看者麦克风；当前监听器未启用，将在下次连接时生效");
                } else {
                    append_log_line(g_app->settings.allow_microphone
                                        ? L"接收查看者麦克风已开启（本次会话实时生效）"
                                        : L"接收查看者麦克风已关闭（本次会话实时生效）");
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_microphone
                                    ? L"接收查看者麦克风已开启"
                                    : L"接收查看者麦克风已关闭");
            }
            break;
        case kPrivacyModeCheck:
            g_app->settings.allow_privacy_mode = !g_app->settings.allow_privacy_mode;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                if (g_app->settings.allow_privacy_mode && !g_app->listener_privacy) {
                    // The listener gate is fixed at creation, so the permission
                    // cannot be raised until the next connection.
                    append_log_line(
                        L"已允许隐私模式；当前监听器未启用，将在下次连接时生效");
                } else if (g_app->settings.allow_privacy_mode) {
                    append_log_line(L"已允许隐私模式（本次会话实时生效）");
                } else {
                    append_log_line(L"隐私模式已关闭（本次会话实时生效，遮罩会立即撤下）");
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_privacy_mode ? L"已允许隐私模式"
                                                                   : L"隐私模式已关闭");
            }
            break;
        case kCarrierCheck:
            g_app->settings.carrier = !g_app->settings.carrier;
            save_settings();
            if (session_is_live()) {
                // The fallback listener is bound at start-up; changing it now
                // would drop the viewer, so the switch waits for the next run.
                append_log_line(g_app->settings.carrier
                                    ? L"已开启弱网回退；将在下次开始共享时生效"
                                    : L"已关闭弱网回退；将在下次开始共享时生效");
            } else {
                restart_server();
                if (!g_app->settings.carrier) {
                    append_log_line(L"弱网回退已关闭");
                } else if (!g_app->server) {
                    append_log_line(L"弱网回退已开启");
                } else {
                    const HostStatus status = g_app->server->status();
                    append_log_line(status.carrier_listening
                                        ? L"弱网回退已开启（端口 " +
                                              std::to_wstring(status.carrier_port) + L"）"
                                        : L"弱网回退已开启，但监听失败：" +
                                              widen(status.carrier_error));
                }
            }
            break;
        case kAutostartCheck: {
            const bool enabled = !g_app->settings.autostart;
            std::string error;
            if (!chast::host::set_autostart(enabled, &error)) {
                append_log_line(L"开机启动：" + widen(error));
                const std::wstring message = L"无法修改开机启动项：\n" + widen(error);
                MessageBoxW(g_app->window, message.c_str(), kWindowTitle,
                            MB_ICONERROR | MB_OK);
                break;
            }
            g_app->settings.autostart = enabled;
            save_settings();
            append_log_line(enabled ? L"已加入开机启动" : L"已从开机启动移除");
            break;
        }
        case kFirewallButton: {
            append_log_line(L"正在请求管理员权限以添加防火墙规则…");
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            uint16_t tunnel_port = 0;
            if (g_app->settings.tunnel) {
                (void)tunnel_listen_port(g_app->settings.tunnel_listen, tunnel_port);
            }
            std::string error;
            if (chast::host::request_firewall_rule(g_app->settings.port, tunnel_port, &error)) {
                std::wstring ports = std::to_wstring(g_app->settings.port);
                if (tunnel_port != 0) {
                    ports += L" 和 " + std::to_wstring(tunnel_port);
                }
                append_log_line(L"已允许 chast 在 UDP 端口 " + ports + L" 上通过防火墙");
            } else {
                append_log_line(L"防火墙：" + widen(error));
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            break;
        }
        case kTunnelSettingsButton: {
            HostSettings updated;
            if (edit_tunnel_settings(updated)) {
                g_app->settings = updated;
                save_settings();
                restart_server();
                append_log_line(L"连接服务设置已保存");
            }
            break;
        }
        case kTunnelRegisterButton:
            if (!g_app->settings.tunnel) {
                append_log_line(L"连接服务已关闭，请先在“连接服务设置…”中启用");
            } else {
                append_log_line(L"正在重新注册连接服务…");
                restart_tunnel();
            }
            break;
        case kOpenConfigButton:
            save_settings();
            ShellExecuteW(g_app->window, L"open", widen(g_app->config_path).c_str(), nullptr,
                          nullptr, SW_SHOWNORMAL);
            break;
        case kToggleButton:
            if (g_app->server && g_app->server->running()) {
                stop_server();
            } else {
                if (g_app->server) {
                    g_app->server.reset();
                }
                start_server();
            }
            break;
        case kReloadButton:
            reload_settings();
            return;
        case kHideButton:
            ShowWindow(g_app->window, SW_HIDE);
            return;
        case kQuitButton:
            g_app->quitting = true;
            DestroyWindow(g_app->window);
            return;
        default:
            return;
    }
    refresh_ui();
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            g_app->window = hwnd;
            create_fonts();
            create_controls();
            SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                         reinterpret_cast<LPARAM>(LoadIconW(nullptr, IDI_APPLICATION)));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                         reinterpret_cast<LPARAM>(LoadIconW(nullptr, IDI_APPLICATION)));
            return 0;
        case WM_COMMAND:
            // Menus report 0 here, as do buttons and check boxes (BN_CLICKED).
            if (HIWORD(wparam) == 0) {
                handle_command(LOWORD(wparam));
            }
            return 0;
        case WM_TIMER:
            if (wparam == kStatusTimer) {
                refresh_ui();
            }
            return 0;
        case kLogMessage: {
            auto* text = reinterpret_cast<std::wstring*>(lparam);
            append_log_line(*text);
            delete text;
            return 0;
        }
        case kTokenMessage: {
            auto* token = reinterpret_cast<std::string*>(lparam);
            g_app->settings.access_token = *token;
            delete token;
            save_settings();
            refresh_ui();
            return 0;
        }
        case kSecurityMessage: {
            auto* devices = reinterpret_cast<std::vector<TrustedDevice>*>(lparam);
            g_app->settings.trusted_devices = *devices;
            delete devices;
            save_settings();
            refresh_ui();
            return 0;
        }
        case kChatMessage: {
            auto* text = reinterpret_cast<std::string*>(lparam);
            append_chat_line(L"[查看器] " + widen(*text));
            delete text;
            // Show it without taking focus away from whatever the operator is
            // doing; the taskbar flash announces the message.
            show_chat_window(false);
            return 0;
        }
        case kTunnelMessage:
            process_tunnel_events();
            return 0;
        case kTunnelExitMessage: {
            const uint32_t code = static_cast<uint32_t>(wparam);
            g_app->tunnel.stop();
            g_app->tunnel_error = "代理已退出（代码 " + std::to_string(code) + "）";
            g_app->tunnel_relay.clear();
            g_app->tunnel_viewer_path.clear();
            g_app->tunnel_viewer_remote.clear();
            append_log_line(L"连接服务已断开：" + widen(g_app->tunnel_error));
            refresh_ui();
            return 0;
        }
        case kTrayMessage:
            switch (LOWORD(lparam)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                    show_window();
                    break;
                case WM_RBUTTONUP:
                    show_tray_menu();
                    break;
                default:
                    break;
            }
            return 0;
        case WM_CLOSE:
            if (g_app->quitting) {
                DestroyWindow(hwnd);
            } else {
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kStatusTimer);
            g_app->quitting.store(true);
            if (g_app->chat_window != nullptr) {
                DestroyWindow(g_app->chat_window);
            }
            g_app->tunnel.stop();
            if (g_app->server) {
                g_app->server_stopping.store(true);
                g_app->server->stop();
                g_app->server.reset();
            }
            remove_tray_icon();
            if (g_app->font != nullptr) {
                DeleteObject(g_app->font);
                g_app->font = nullptr;
            }
            if (g_app->title_font != nullptr) {
                DeleteObject(g_app->title_font);
                g_app->title_font = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

std::wstring absolute_path(const std::wstring& path) {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length =
        GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (length == 0 || length >= static_cast<DWORD>(std::size(buffer))) {
        return path;
    }
    return std::wstring(buffer, length);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    static HostApp app;
    g_app = &app;
    app.instance = instance;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string config_path = host_settings_path();
    bool start_hidden = false;
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--config" && i + 1 < argc) {
            config_path = narrow(absolute_path(argv[++i]));
        } else if (arg == L"--tray") {
            start_hidden = true;
        } else if (arg == L"--firewall" && i + 1 < argc) {
            const unsigned long port = wcstoul(argv[++i], nullptr, 10);
            unsigned long tunnel_port = 0;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                tunnel_port = wcstoul(argv[++i], nullptr, 10);
            }
            LocalFree(argv);
            if (port == 0 || port > 65535 || tunnel_port > 65535) {
                return 2;
            }
            return chast::host::apply_firewall_rule(static_cast<uint16_t>(port),
                                                    static_cast<uint16_t>(tunnel_port)) == 0
                       ? 0
                       : 1;
        } else if (arg == L"--autostart" && i + 1 < argc) {
            const bool enabled = _wcsicmp(argv[++i], L"on") == 0;
            std::string error;
            const bool ok = chast::host::set_autostart(enabled, &error);
            LocalFree(argv);
            return ok ? 0 : 1;
        } else if (arg == L"--help" || arg == L"-h") {
            MessageBoxW(nullptr,
                        L"chast 主机\n\n"
                        L"--config <路径>     使用其他设置文件\n"
                        L"--tray              启动后隐藏窗口\n"
                        L"--autostart on|off  设置开机启动\n"
                        L"--firewall <端口> [隧道端口]  添加防火墙规则（含局域网发现 21129/UDP，需要管理员）",
                        kWindowTitle, MB_OK | MB_ICONINFORMATION);
            LocalFree(argv);
            return 0;
        }
    }
    LocalFree(argv);
    if (config_path.empty()) {
        MessageBoxW(nullptr, L"无法确定设置文件的位置。", kWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (config_path == host_settings_path() && chast::host::service_running()) {
        MessageBoxW(nullptr,
                    L"chast 主机服务正在运行，托盘主机已让位。\n"
                    L"请使用 chast-service.exe --token 查看访问令牌。",
                    kWindowTitle, MB_ICONINFORMATION | MB_OK);
        return 0;
    }
    app.config_path = config_path;

    HANDLE instance_mutex = nullptr;
    if (config_path == host_settings_path()) {
        instance_mutex = CreateMutexW(nullptr, FALSE, L"Local\\chast-host-tray");
        if (instance_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
                ShowWindow(existing, SW_SHOWNORMAL);
                SetForegroundWindow(existing);
            } else {
                MessageBoxW(nullptr, L"chast 主机已经在运行。请查看通知区域。", kWindowTitle,
                            MB_ICONINFORMATION | MB_OK);
            }
            if (instance_mutex != nullptr) {
                CloseHandle(instance_mutex);
            }
            return 0;
        }
    }

    HDC screen = GetDC(nullptr);
    app.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (app.dpi <= 0) {
        app.dpi = 96;
    }

    load_settings();

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, px(620), px(724)};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    const HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, style, x, y, width,
                                        height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"无法创建主机窗口。", kWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }
    g_app->window = window;
    add_tray_icon();

    if (!g_app->startup_save_error.empty()) {
        append_log_line(L"保存默认设置失败：" + widen(g_app->startup_save_error));
    }
    append_log_line(L"设置文件：" + widen(g_app->config_path));
    if (!g_app->startup_warning.empty()) {
        append_log_line(L"设置文件有警告：" + widen(g_app->startup_warning));
    }
    if (g_app->settings.autostart) {
        std::string error;
        if (!chast::host::set_autostart(true, &error)) {
            append_log_line(L"开机启动：" + widen(error));
        }
    }

    start_server();
    if (g_app->server) {
        const HostStatus status = g_app->server->status();
        if (!status.fingerprint.empty()) {
            append_log_line(L"身份 SHA256 " + widen(format_fingerprint(status.fingerprint)));
        }
    }

    SetTimer(window, kStatusTimer, 500, nullptr);
    refresh_ui();
    if (!start_hidden) {
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (instance_mutex != nullptr) {
        CloseHandle(instance_mutex);
    }
    return static_cast<int>(message.wParam);
}
