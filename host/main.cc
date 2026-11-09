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
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <deque>
#include <iterator>
#include <future>
#include <filesystem>
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
#include "chast/virtual_display.h"
#include "chast/user_context.h"
#include "elevation_handoff.h"
#include "i18n.h"
#include "connections.h"
#include "platform.h"
#include "service.h"
#include "service_manager.h"
#include "service_settings.h"
#include "tunnel.h"

namespace {

using namespace chast;
using chast::host::narrow;
using chast::host::load_ui_language;
using chast::host::tr;
using chast::host::TunnelEvent;
using chast::host::TunnelProcess;
using chast::host::validate_tunnel_settings;
using chast::host::widen;

constexpr wchar_t kWindowClass[] = L"ChastHostTrayWindow";

const wchar_t* window_title() {
    return tr(L"chast 主机");
}

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kLogMessage = WM_APP + 2;
constexpr UINT kTokenMessage = WM_APP + 3;
constexpr UINT kTunnelMessage = WM_APP + 4;
constexpr UINT kTunnelExitMessage = WM_APP + 5;
constexpr UINT kSecurityMessage = WM_APP + 6;
constexpr UINT kSecurityPageMessage = WM_APP + 7;
constexpr UINT kChatMessage = WM_APP + 8;
constexpr UINT kPasswordHashMessage = WM_APP + 9;
constexpr UINT kSettingsMessage = WM_APP + 10;
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
    kRecordCheck,
    kCameraCheck,
    kSendFileButton,
    kAudioCheck,
    kMicrophoneCheck,
    kPrivacyModeCheck,
    kRestartCheck,
    kBlockInputCheck,
    kOsLoginCheck,
    kSessionSelectCheck,
    kRemoteConfigCheck,
    kLockAfterEndCheck,
    kCarrierCheck,
    kAutostartCheck,
    kSecurityButton,
    kFirewallButton,
    kVdisplayInstallButton,
    kVdisplayUninstallButton,
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

struct ViewerChatMessage {
    uint64_t id;
    std::string text;
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
    // Portable mode: settings live beside the executable and the host never
    // writes the login autostart entry, so the folder stays self-contained.
    bool portable = false;
    bool machine_settings_editor = false;
    bool service_handoff = false;
    std::shared_ptr<chast::host::ElevationController> elevation = std::make_shared<chast::host::ElevationController>();
    std::unique_ptr<chast::host::ElevatedChild> elevated_child;
    std::shared_ptr<void> identity_user_token;
    HANDLE instance_mutex = nullptr;
    bool elevation_transferring = false;
    std::shared_ptr<chast::host::ElevationHandoff> handled_elevation;
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
    std::unique_ptr<chast::host::ConnectionsWindow> connections;

};

HostApp* g_app = nullptr;

chast::host::ConnectionsWindow& connection_window() {
    if (!g_app->connections) g_app->connections = std::make_unique<chast::host::ConnectionsWindow>(
        g_app->window, g_app->instance, g_app->font, g_app->dpi, [] { return g_app->server.get(); });
    return *g_app->connections;
}

int px(int value) {
    return MulDiv(value, g_app->dpi, 96);
}

// English labels are wider than the four-character Chinese ones the layout was
// drawn for. Measure with the real UI font so a control grows with its text
// instead of clipping it.
int text_px(const wchar_t* text, HFONT font = nullptr) {
    HFONT use = font != nullptr ? font : g_app->font;
    if (text == nullptr || *text == L'\0' || use == nullptr) {
        return 0;
    }
    HDC dc = GetDC(g_app->window);
    HGDIOBJ previous = SelectObject(dc, use);
    SIZE extent{};
    GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &extent);
    SelectObject(dc, previous);
    ReleaseDC(g_app->window, dc);
    return MulDiv(extent.cx, 96, g_app->dpi) + 2;
}

int button_px(const wchar_t* text) {
    return text_px(text) + 22;
}

int check_px(const wchar_t* text) {
    return text_px(text) + 26;
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

void server_password_hash_changed(const std::string& hash) {
    auto* copy = new std::string(hash);
    if (!PostMessageW(g_app->window, kPasswordHashMessage, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

// The switches a viewer may change remotely. Credentials, ports and the
// remote-config gate itself stay with the listener's copy of the settings.
void apply_remote_switches(const HostSettings& from, HostSettings& to) {
    to.lock_after_session_end = from.lock_after_session_end;
    to.allow_clipboard = from.allow_clipboard;
    to.allow_file_transfer = from.allow_file_transfer;
    to.allow_audio = from.allow_audio;
    to.allow_microphone = from.allow_microphone;
    to.allow_privacy_mode = from.allow_privacy_mode;
    to.allow_restart = from.allow_restart;
    to.allow_block_input = from.allow_block_input;
    to.allow_os_login = from.allow_os_login;
    to.allow_session_select = from.allow_session_select;
    to.allow_terminal = from.allow_terminal;
    to.allow_port_forward = from.allow_port_forward;
    to.allow_camera = from.allow_camera;
    to.allow_record = from.allow_record;
}

void server_settings_changed(const HostSettings& settings) {
    auto* copy = new HostSettings();
    apply_remote_switches(settings, *copy);
    if (!PostMessageW(g_app->window, kSettingsMessage, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

void server_chat(uint64_t id, const std::string& text) {
    auto* copy = new ViewerChatMessage{id, text};
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
    kApprovalRecord,
    kApprovalRemoteConfig,
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
    HWND record = nullptr;
    HWND remote_config = nullptr;
    // Valid for the duration of server_approve(): the transport keeps the
    // request alive across the callback and re-checks liveness before it
    // admits the viewer.
    const ClientRequest* request = nullptr;
    PermissionMask result = 0;
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
        PermissionMask mask = 0;
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
        if (SendMessageW(dialog.record, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionRecord;
        }
        if (SendMessageW(dialog.remote_config, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kPermissionRemoteConfig;
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
    return platform == 1 ? L"Windows" : tr(L"未知系统");
}

std::wstring approval_method_label(ClientApprovalMethod method) {
    switch (method) {
        case ClientApprovalMethod::Click:
            return tr(L"点击批准模式：每次连接都需要确认");
        case ClientApprovalMethod::NoPassword:
            return tr(L"查看器没有提供密码，请求当面批准");
        case ClientApprovalMethod::Unknown:
            break;
    }
    return tr(L"查看器请求连接");
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
            post_log(tr(L"无法创建连接审批窗口，已拒绝这次连接"));
            return ClientApproval{};
        }
        class_ready = true;
    }

    ApprovalDialogState dialog;
    dialog.request = &request;
    const wchar_t* input_text = tr(L"键盘与鼠标");
    const wchar_t* clipboard_text = tr(L"剪贴板");
    const wchar_t* audio_text = tr(L"系统声音");
    const wchar_t* file_text = tr(L"文件传输");
    const wchar_t* microphone_text = tr(L"麦克风（上行）");
    const wchar_t* privacy_text = tr(L"隐私模式（黑屏）");
    const wchar_t* record_text = tr(L"查看者录制会话");
    const wchar_t* remote_config_text = tr(L"修改主机设置");
    const int column1 = std::max(130, std::max(check_px(input_text), check_px(file_text)));
    const int column2 = std::max(130, std::max(check_px(clipboard_text), check_px(microphone_text)));
    const int column3 =
        std::max(130, std::max(check_px(audio_text),
                               std::max(check_px(privacy_text), check_px(record_text))));
    const int second = 20 + column1 + 20;
    const int third = second + column2 + 20;
    const int dialog_width = third + column3 + 20;
    const int full_width = dialog_width - 40;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT frame{0, 0, px(dialog_width), px(376)};
    AdjustWindowRectEx(&frame, style, FALSE, WS_EX_TOPMOST);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kApprovalDialogClass,
                                  tr(L"chast 连接请求"), style, x, y, width, height, nullptr,
                                  nullptr, g_app->instance, &dialog);
    if (window == nullptr) {
        post_log(tr(L"无法创建连接审批窗口，已拒绝这次连接"));
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

    add(L"STATIC", tr(L"有查看器请求连接这台电脑"), SS_LEFT, 20, 16, full_width, 24, 0,
        g_app->title_font);
    const std::wstring peer =
        request.peer_address.empty() ? std::wstring(tr(L"未知地址")) : widen(request.peer_address);
    add(L"STATIC", (tr(L"来源：") + peer).c_str(), SS_LEFT, 20, 52, full_width, 20, 0, nullptr);
    std::wstring device = tr(L"设备：");
    device += request.hello.device_name.empty() ? tr(L"未命名") : widen(request.hello.device_name);
    device += tr(L"（") + platform_label(request.hello.device_platform) + tr(L"）");
    add(L"STATIC", device.c_str(), SS_LEFT, 20, 76, full_width, 20, 0, nullptr);
    const std::wstring method = tr(L"方式：") + approval_method_label(request.method);
    add(L"STATIC", method.c_str(), SS_LEFT, 20, 100, full_width, 40, 0, nullptr);
    if (request.device_proof) {
        add(L"STATIC", tr(L"设备签名验证通过，允许后将记入可信设备。"), SS_LEFT, 20, 140,
            full_width, 20, 0, nullptr);
    }
    add(L"STATIC", tr(L"允许的权限（可以取消勾选）："), SS_LEFT, 20, 164, full_width, 20, 0,
        nullptr);

    const auto permission_check = [&](const wchar_t* text, PermissionMask bit, int left, int width,
                                      int top, int id) {
        HWND control = add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, left, top, width, 24,
                           id, nullptr);
        if ((request.permissions & bit) == 0) {
            SendMessageW(control, BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(control, FALSE);
        } else {
            SendMessageW(control, BM_SETCHECK, BST_CHECKED, 0);
        }
        return control;
    };
    dialog.input = permission_check(input_text, kPermissionInput, 20, column1, 188,
                                    kApprovalInput);
    dialog.clipboard = permission_check(clipboard_text, kPermissionClipboard, second, column2,
                                        188, kApprovalClipboard);
    dialog.audio = permission_check(audio_text, kPermissionAudio, third, column3, 188,
                                    kApprovalAudio);
    dialog.file = permission_check(file_text, kPermissionFileTransfer, 20, column1, 216,
                                   kApprovalFile);
    const std::string receive_directory = g_app->settings.file_receive_directory.empty()
                                              ? chast::default_receive_directory()
                                              : g_app->settings.file_receive_directory;
    add(L"STATIC",
        (tr(L"接收目录：") +
         (receive_directory.empty() ? std::wstring(tr(L"（不可用）")) : widen(receive_directory)))
            .c_str(),
        SS_LEFT | SS_ENDELLIPSIS, second, 220, third - second - 10, 18, 0, nullptr);
    dialog.microphone = permission_check(microphone_text, kPermissionMicrophone, 20, column1,
                                         244, kApprovalMicrophone);
    dialog.privacy = permission_check(privacy_text, kPermissionPrivacyMode, second, column2, 244,
                                      kApprovalPrivacy);
    dialog.record = permission_check(record_text, kPermissionRecord, third, column3, 244,
                                     kApprovalRecord);
    dialog.remote_config = permission_check(remote_config_text, kPermissionRemoteConfig, third,
                                            column3, 272, kApprovalRemoteConfig);

    const int deny_left = dialog_width - 20 - 90;
    const int allow_left = deny_left - 10 - 90;
    add(L"BUTTON", tr(L"允许"), BS_DEFPUSHBUTTON | WS_TABSTOP, allow_left, 306, 90, 32,
        kApprovalAllow, nullptr);
    add(L"BUTTON", tr(L"拒绝"), BS_PUSHBUTTON | WS_TABSTOP, deny_left, 306, 90, 32, kApprovalDeny,
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
    const PermissionMask mask = dialog.result;
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    if (peer_gone) {
        post_log(tr(L"查看者在审批期间离开，已取消来自 ") + peer + tr(L" 的请求"));
    } else {
        post_log(allowed ? tr(L"已允许来自 ") + peer + tr(L" 的连接")
                         : tr(L"已拒绝来自 ") + peer + tr(L" 的连接"));
    }
    if (!allowed) {
        return ClientApproval{};
    }
    return ClientApproval{true, mask};
}

bool save_settings() {
    std::string error;
    ScopedUserContext original(g_app->identity_user_token.get());
    if (!original.ok() || !save_host_settings(g_app->config_path, g_app->settings, &error)) {
        post_log(tr(L"保存设置失败：") + widen(error));
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
        append_log_line(tr(L"连接服务：尚未配置服务器地址"));
        return;
    }
    std::string error;
    if (!validate_tunnel_settings(g_app->settings, &error)) {
        g_app->tunnel_error = error;
        append_log_line(tr(L"连接服务：") + widen(error));
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
        append_log_line(tr(L"连接服务启动失败：") + widen(error));
        return;
    }
    append_log_line(tr(L"正在注册设备 ") + widen(g_app->settings.tunnel_id) + L"…");
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
    append_log_line(tr(L"共享已停止"));
}

void start_server() {
    if (g_app->server) {
        return;
    }
    if (!g_app->portable && g_app->config_path == host_settings_path() && chast::host::service_installed()) {
        append_log_line(tr(L"已安装主机服务，请使用服务连接管理。"));
        return;
    }
    HostServerConfig config;
    config.settings = g_app->settings;
    config.audio_silence_gate_ms = g_app->settings.audio_silence_gate_ms;
    config.serve_once = false;
    config.first_client_timeout_ms = 0;
    // The policy decides when to ask; the dialog is available for both and
    // click modes and never called in password-only mode.
    config.approve_client = server_approve;
    config.log = server_log;
    config.token_changed = server_token_changed;
    config.trusted_devices_changed = server_trusted_devices_changed;
    config.password_hash_changed = server_password_hash_changed;
    config.settings_changed = server_settings_changed;
    config.viewer_chat_message = server_chat;
    config.identity_user_token = g_app->identity_user_token;
    if (g_app->elevated_child) config.expected_identity_fingerprint = g_app->elevated_child->snapshot.fingerprint;
    const auto elevation = g_app->elevation;
    config.elevation_state = [elevation] { return elevation->state(); };
    config.request_elevation = [elevation] { return elevation->request(); };
    std::string error;
    g_app->server = HostServer::create(std::move(config), &error);
    if (!g_app->server) {
        append_log_line(tr(L"启动失败：") + widen(error));
        const std::wstring message = tr(L"无法启动共享：\n") + widen(error);
        if (!g_app->elevated_child) MessageBoxW(g_app->window, message.c_str(), window_title(), MB_ICONERROR | MB_OK);
        return;
    }
    if (g_app->elevated_child && g_app->server->status().fingerprint != g_app->elevated_child->snapshot.fingerprint) {
        append_log_line(L"Elevation failed: the original device identity could not be retained.");
        g_app->server.reset();
        return;
    }
    g_app->listener_microphone = g_app->settings.allow_microphone;
    g_app->listener_privacy = g_app->settings.allow_privacy_mode;
    g_app->server->start();
    append_log_line(tr(L"正在等待查看器（端口 ") + std::to_wstring(g_app->settings.port) + tr(L"）"));
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

void poll_elevation() {
    if (g_app->elevated_child && g_app->elevated_child->cancelled()) {
        DestroyWindow(g_app->window);
        return;
    }
    const auto task = g_app->elevation->current();
    if (!task || !task->committed()) return;
    if (g_app->handled_elevation != task) {
        // Settings dialogs run their own message loop. Let their save/cancel
        // finish before taking the final snapshot or destroying the owner.
        if (!IsWindowEnabled(g_app->window)) return;
        g_app->handled_elevation = task;
        if (task->finished()) {
            append_log_line(L"Elevation ended before transfer: " + widen(task->poll().reason));
            return;
        }
        g_app->elevation_transferring = true;
        EnableWindow(g_app->window, FALSE);
        chast::host::ElevationSnapshot snapshot;
        snapshot.fingerprint = g_app->server ? g_app->server->status().fingerprint : std::string();
        snapshot.hidden = IsWindowVisible(g_app->window) == FALSE;
        snapshot.portable = g_app->portable;
        snapshot.instance_mutex = g_app->instance_mutex;
        stop_server();
        // stop() joins the callbacks. Apply every queued credential/policy
        // update before serializing so an in-flight change cannot be lost.
        for (const UINT id : {kTokenMessage, kSecurityMessage, kPasswordHashMessage, kSettingsMessage}) {
            MSG message{};
            while (PeekMessageW(&message, g_app->window, id, id, PM_REMOVE)) DispatchMessageW(&message);
        }
        snapshot.config_path = g_app->config_path;
        snapshot.settings = g_app->settings;
        std::string error;
        if (!task->supply(snapshot, &error)) {
            append_log_line(L"Elevation transfer failed: " + widen(error));
            task->shutdown();
        }
    }
    if (!g_app->elevation_transferring || !task->finished()) return;
    const auto state = task->poll();
    if (state.code == ElevationCode::Elevated) {
        DestroyWindow(g_app->window);
    } else {
        g_app->elevation_transferring = false;
        EnableWindow(g_app->window, TRUE);
        append_log_line(L"Elevation failed; restoring the original host: " + widen(state.reason));
        start_server();
    }
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
        case VideoCodec::Vp9:
            return L"VP9";
        case VideoCodec::Av1:
            return L"AV1";
    }
    return L"—";
}

std::wstring state_text(HostState state, const HostStatus& status) {
    switch (state) {
        case HostState::Stopped:
            return tr(L"已停止");
        case HostState::Listening: {
            std::wstring text =
                tr(L"等待查看器（端口 ") + std::to_wstring(g_app->settings.port) + tr(L"）");
            if (status.carrier_listening) {
                text += tr(L" · 弱网回退端口 ") + std::to_wstring(status.carrier_port);
            } else if (!status.carrier_error.empty()) {
                text += tr(L" · 弱网回退不可用");
            }
            return text;
        }
        case HostState::Streaming:
            return tr(L"正在共享");
        case HostState::Failed:
            return status.error.empty() ? tr(L"出错") : tr(L"出错：") + widen(status.error);
    }
    return L"—";
}

std::wstring tunnel_text() {
    const HostSettings& settings = g_app->settings;
    if (!settings.tunnel) {
        return tr(L"连接服务：已关闭");
    }
    if (settings.server_url.empty()) {
        return tr(L"连接服务：未配置服务器（打开“连接服务设置…”）");
    }
    std::wstring text = tr(L"连接服务：");
    if (g_app->tunnel.running()) {
        text += g_app->tunnel_relay.empty() ? tr(L"注册中…")
                                            : tr(L"已在线 · 中继 ") + widen(g_app->tunnel_relay);
    } else if (!g_app->server) {
        text += tr(L"已停止");
    } else if (!g_app->tunnel_error.empty()) {
        text += tr(L"离线 · ") + widen(g_app->tunnel_error);
    } else {
        text += tr(L"未连接");
    }
    if (!settings.tunnel_id.empty()) {
        text += tr(L" · 设备 ID ") + widen(settings.tunnel_id);
    }
    return text;
}

std::wstring virtual_display_text(const VirtualDisplayProbe& probe) {
    const VirtualDisplayStatus status = classify_virtual_display_probe(probe);
    switch (status.state) {
        case VirtualDisplayState::Unsupported:
            return tr(L"虚拟显示器：不支持");
        case VirtualDisplayState::NotInstalled:
            return tr(L"虚拟显示器：未安装");
        case VirtualDisplayState::Installed:
            return tr(L"虚拟显示器：已安装，未启用");
        case VirtualDisplayState::Active:
            return tr(L"虚拟显示器：运行中 ") + std::to_wstring(status.active_monitors) + L"/" +
                   std::to_wstring(status.max_monitors);
        case VirtualDisplayState::Error:
            if (!probe.conflicting_driver.empty()) {
                return tr(L"虚拟显示器：检测到其他虚拟显示驱动");
            }
            return tr(L"虚拟显示器：检测失败");
    }
    return tr(L"虚拟显示器：不支持");
}

bool path_is_directory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring path_text(const HostStatus& status) {
    if (status.state != HostState::Streaming || g_app->tunnel_viewer_path.empty()) {
        return tr(L"最近路径：—");
    }
    const bool relay = g_app->tunnel_viewer_path == "relay";
    std::wstring text = relay ? tr(L"最近路径：经中继") : tr(L"最近路径：直连");
    if (!g_app->tunnel_viewer_remote.empty()) {
        text += L" · " + widen(g_app->tunnel_viewer_remote);
    }
    return text;
}

std::wstring auth_mode_label(HostAuthMode mode) {
    switch (mode) {
        case HostAuthMode::Password:
            return tr(L"密码");
        case HostAuthMode::Click:
            return tr(L"点击批准");
        case HostAuthMode::Both:
            return tr(L"密码或批准");
    }
    return L"—";
}

std::wstring permission_label(PermissionMask mask) {
    std::wstring text;
    auto add = [&text](const wchar_t* name) {
        if (!text.empty()) {
            text += tr(L"、");
        }
        text += name;
    };
    if ((mask & kPermissionInput) != 0) {
        add(tr(L"键盘"));
    }
    if ((mask & kPermissionClipboard) != 0) {
        add(tr(L"剪贴板"));
    }
    if ((mask & kPermissionFileTransfer) != 0) {
        add(tr(L"文件传输"));
    }
    if ((mask & kPermissionPortForward) != 0) {
        add(tr(L"端口转发"));
    }
    if ((mask & kPermissionTerminal) != 0) {
        add(tr(L"远程终端"));
    }
    if ((mask & kPermissionAudio) != 0) {
        add(tr(L"声音"));
    }
    if ((mask & kPermissionMicrophone) != 0) {
        add(tr(L"麦克风"));
    }
    if ((mask & kPermissionPrivacyMode) != 0) {
        add(tr(L"隐私模式"));
    }
    if ((mask & kPermissionRecord) != 0) {
        add(tr(L"录制"));
    }
    if ((mask & kPermissionCamera) != 0) {
        add(tr(L"摄像头"));
    }
    if ((mask & kPermissionRestart) != 0) {
        add(tr(L"远程重启"));
    }
    if ((mask & kPermissionBlockInput) != 0) {
        add(tr(L"阻止本地输入"));
    }
    if ((mask & kPermissionOsLogin) != 0) {
        add(tr(L"系统登录"));
    }
    if ((mask & kPermissionSessionSelect) != 0) {
        add(tr(L"会话切换"));
    }
    if ((mask & kPermissionRemoteConfig) != 0) {
        add(tr(L"远程配置"));
    }
    return text.empty() ? tr(L"只看") : text;
}

PermissionMask current_gate_mask() {
    return ((g_app->settings.allow_input ? kPermissionInput : 0) |
                                 (g_app->settings.allow_clipboard ? kPermissionClipboard : 0) |
                                 (g_app->settings.allow_audio ? kPermissionAudio : 0) |
                                 (g_app->settings.allow_microphone ? kPermissionMicrophone : 0) |
                                 (g_app->settings.allow_privacy_mode ? kPermissionPrivacyMode
                                                                     : 0) |
                                 (g_app->settings.allow_file_transfer ? kPermissionFileTransfer
                                                                      : 0) |
                                 (g_app->settings.allow_port_forward ? kPermissionPortForward
                                                                     : 0) |
                                 (g_app->settings.allow_terminal ? kPermissionTerminal : 0) |
                                 (g_app->settings.allow_record ? kPermissionRecord : 0) |
                                 (g_app->settings.allow_camera ? kPermissionCamera : 0) |
                                 (g_app->settings.allow_restart ? kPermissionRestart : 0) |
                                 (g_app->settings.allow_block_input ? kPermissionBlockInput
                                                                    : 0) |
                                 (g_app->settings.allow_os_login ? kPermissionOsLogin : 0) |
                                 (g_app->settings.allow_session_select
                                      ? kPermissionSessionSelect
                                      : 0) |
                                 (g_app->settings.allow_remote_config
                                      ? kPermissionRemoteConfig
                                      : 0));
}

std::wstring security_summary_text(const HostStatus& status) {
    const bool live = g_app->server != nullptr;
    const HostAuthMode mode = live ? status.auth_mode : g_app->settings.auth_mode;
    const bool password =
        live ? status.password_configured : !g_app->settings.password_hash.empty();
    const bool two_factor = live ? status.two_factor_enabled : !g_app->settings.totp_secret.empty();
    std::wstring text = tr(L"接入：") + auth_mode_label(mode);
    text += password ? tr(L" · 永久密码已设置") : tr(L" · 永久密码未设置");
    text += two_factor ? tr(L" · 2FA 已启用") : tr(L" · 2FA 已关闭");
    text += tr(L" · 默认权限：") + permission_label(current_gate_mask());
    return text;
}

std::wstring session_permission_text(const HostStatus& status) {
    if (status.state != HostState::Streaming) {
        return tr(L"本次会话权限：—");
    }
    std::wstring text = L"#" + std::to_wstring(status.connection_id) + L" · " +
        permission_label(status.session_permissions);
    if (status.stats.privacy_mode_active) {
        text += tr(L" · 本地屏幕已遮挡（隐私模式）");
    }
    if (status.stats.block_input_active) {
        text += tr(L" · 本地输入已阻止");
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

    set_text(g_app->state_label, tr(L"状态：") + (g_app->server ? state_text(status.state, status)
                                                           : std::wstring(tr(L"已停止"))));
    set_text(g_app->peer_label,
             tr(L"查看器：") + (status.peer_address.empty() ? std::wstring(L"—")
                                                       : widen(status.peer_address)));
    if (status.stream.width > 0) {
        std::wstring stream = tr(L"画面：") + widen(status.display_name) + L" · " +
                              std::to_wstring(status.stream.width) + L"×" +
                              std::to_wstring(status.stream.height) + L" · " +
                              codec_label(status.stream.codec) + L" · " +
                              std::to_wstring(status.stream.framerate) + L" fps · " +
                              std::to_wstring(status.stats.encoded) + tr(L" 帧");
        if (status.stats.target_bitrate > 0) {
            stream += L" · " +
                      std::to_wstring((status.stats.target_bitrate + 500) / 1000) + L" kbps";
        }
        if (status.stats.rtt_ms > 0.0) {
            stream += L" · RTT " +
                      std::to_wstring(static_cast<int>(status.stats.rtt_ms + 0.5)) + L" ms";
        }
        if (status.stats.input_events > 0) {
            stream += tr(L" · 输入 ") + std::to_wstring(status.stats.input_events);
        }
        if (status.stats.audio_active) {
            stream += tr(L" · 音频");
            if (status.stats.audio_dsp_frames > 0) {
                stream += L" · AEC " + std::to_wstring(status.stats.audio_dsp_frames) +
                          tr(L" 帧");
                if (status.stats.audio_dsp_resets > 0) {
                    stream += tr(L"（重置 ") +
                              std::to_wstring(status.stats.audio_dsp_resets) + tr(L"）");
                }
            }
        }
        if (status.download_active) {
            stream += tr(L" · 发送 ") + widen(status.download_name) + L" " +
                      std::to_wstring(status.download_sent) + L"/" +
                      std::to_wstring(status.download_size);
        } else if (!status.download_error.empty()) {
            stream += tr(L" · 发送失败");
        }
        set_text(g_app->stream_label, stream);
    } else {
        set_text(g_app->stream_label, tr(L"画面：—"));
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
        temporary = tr(L"点击批准模式");
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
    set_text(g_app->toggle_button, running ? tr(L"停止共享") : tr(L"开始共享"));

    std::wstring tip = std::wstring(tr(L"chast 主机 · ")) +
                       (g_app->server ? state_text(status.state, status) : std::wstring(tr(L"已停止")));
    if (g_app->settings.tunnel && g_app->tunnel.running()) {
        tip += g_app->tunnel_relay.empty() ? tr(L" · 连接服务注册中") : tr(L" · 连接服务在线");
    }
    update_tray_tip(tip);
}

void apply_tunnel_event(const TunnelEvent& event) {
    if (event.event == "ready") {
        g_app->tunnel_relay = event.relay;
        g_app->tunnel_error.clear();
        const std::string device =
            event.host.empty() ? g_app->settings.tunnel_id : event.host;
        std::wstring line = tr(L"连接服务已上线 · 设备 ID ") + widen(device);
        if (!event.listen.empty()) {
            line += tr(L" · 隧道 ") + widen(event.listen);
        }
        if (!event.relay.empty()) {
            line += tr(L" · 中继 ") + widen(event.relay);
        }
        append_log_line(line);
        if (!event.migrated_from.empty()) {
            append_log_line(tr(L"设备 ID 已从 ") + widen(event.migrated_from) + tr(L" 迁移到 ") +
                            widen(device));
            if (g_app->settings.tunnel_old_id == event.migrated_from) {
                g_app->settings.tunnel_old_id.clear();
                save_settings();
            }
        }
    } else if (event.event == "relay") {
        g_app->tunnel_relay = event.relay;
        append_log_line(tr(L"连接服务中继地址：") + widen(event.relay));
    } else if (event.event == "viewer") {
        g_app->tunnel_viewer_path = event.path;
        g_app->tunnel_viewer_remote = event.remote;
        std::wstring line =
            event.path == "relay" ? tr(L"查看者经中继连接") : tr(L"查看者经直连");
        if (!event.remote.empty()) {
            line += tr(L"：") + widen(event.remote);
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
    ScopedUserContext original(g_app->identity_user_token.get());
    if (!original.ok()) {
        g_app->startup_warning = "Cannot access the original user's settings.";
        return;
    }
    const HostSettingsLoad loaded = load_host_settings(g_app->config_path);
    g_app->settings = loaded.settings;
    if (g_app->portable) {
        // A copied folder must not keep a login entry pointing at whatever
        // path it had on the machine that created it.
        g_app->settings.autostart = false;
    }
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

int main_window_width() {
    return chast::host::english_ui() ? 700 : 620;
}

int main_window_summary_shift() {
    // English lists every permission by its full label, which needs two lines.
    return chast::host::english_ui() ? 24 : 0;
}

int main_window_height() {
    return 724 + main_window_summary_shift();
}

void create_controls() {
    const int content = main_window_width() - 48;
    const int shift = main_window_summary_shift();

    add_control(L"STATIC", tr(L"chast 主机"), SS_LEFT, 0, 24, 18, 300, 30, 0,
                g_app->title_font);
    add_control(L"STATIC", tr(L"远程桌面主机 · 设备 ID、临时密码与安全设置"), SS_LEFT, 0, 24, 52,
                500, 20, 0, nullptr);

    g_app->state_label = add_control(L"STATIC", tr(L"状态：启动中…"), SS_LEFT, 0, 24, 84,
                                     content, 22, kStateLabel, nullptr);
    g_app->peer_label = add_control(L"STATIC", tr(L"查看器：—"), SS_LEFT, 0, 24, 108, content, 22,
                                    kPeerLabel, nullptr);
    g_app->stream_label = add_control(L"STATIC", tr(L"画面：—"), SS_LEFT, 0, 24, 132, content, 22,
                                      kStreamLabel, nullptr);
    g_app->tunnel_label = add_control(L"STATIC", tr(L"连接服务：—"), SS_LEFT, 0, 24, 156, content,
                                      22, kTunnelLabel, nullptr);
    g_app->path_label = add_control(L"STATIC", tr(L"最近路径：—"), SS_LEFT, 0, 24, 180, content,
                                    22, kPathLabel, nullptr);

    add_control(L"STATIC", tr(L"设备 ID（用于连接服务）"), SS_LEFT, 0, 24, 214, 300, 20, 0,
                nullptr);
    g_app->device_id_edit =
        add_control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 24, 236, 340,
                    26, kDeviceIdEdit, nullptr);
    const wchar_t* copy_id_text = tr(L"复制 ID");
    add_control(L"BUTTON", copy_id_text, BS_PUSHBUTTON, 0, 376, 235,
                std::max(88, button_px(copy_id_text)), 28, kCopyIdButton, nullptr);

    add_control(L"STATIC", tr(L"临时密码（使用一次或连续失败后自动更换）"), SS_LEFT, 0, 24, 272,
                400, 20, 0, nullptr);
    g_app->temp_password_edit =
        add_control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 24, 294, 340,
                    26, kTempPasswordEdit, nullptr);
    const wchar_t* copy_password_text = tr(L"复制密码");
    const int copy_password_width = std::max(88, button_px(copy_password_text));
    add_control(L"BUTTON", copy_password_text, BS_PUSHBUTTON, 0, 376, 293, copy_password_width, 28,
                kCopyPasswordButton, nullptr);
    const wchar_t* refresh_password_text = tr(L"刷新密码");
    g_app->refresh_password_button =
        add_control(L"BUTTON", refresh_password_text, BS_PUSHBUTTON, 0,
                    376 + copy_password_width + 10, 293,
                    std::max(120, button_px(refresh_password_text)), 28, kRefreshPasswordButton,
                    nullptr);

    g_app->security_summary = add_control(L"STATIC", tr(L"接入：—"), SS_LEFT, 0, 24, 330, content,
                                          20 + shift, kSecuritySummary, nullptr);
    g_app->session_label = add_control(L"STATIC", tr(L"本次会话权限：—"), SS_LEFT, 0, 24,
                                       352 + shift, 440, 20, kSessionLabel, nullptr);
    add_control(L"BUTTON", tr(L"管理连接…"), BS_PUSHBUTTON, 0, 476, 348 + shift, 120, 28,
                kChatButton, nullptr);

    int left = 24;
    const auto check_row = [&left](const wchar_t* text, int top, int id) {
        const int width = check_px(text);
        HWND control =
            add_control(L"BUTTON", text, BS_AUTOCHECKBOX, 0, left, top, width, 24, id, nullptr);
        left += width + 12;
        return control;
    };
    g_app->input_check = check_row(tr(L"接受远程输入"), 380 + shift, kInputCheck);
    g_app->clipboard_check = check_row(tr(L"同步剪贴板"), 380 + shift, kClipboardCheck);
    g_app->audio_check = check_row(tr(L"共享系统声音"), 380 + shift, kAudioCheck);
    g_app->autostart_check = check_row(tr(L"开机自动启动"), 380 + shift, kAutostartCheck);
    if (g_app->portable) {
        EnableWindow(g_app->autostart_check, FALSE);
    }
    left = 24;
    g_app->microphone_check = check_row(tr(L"接收查看者麦克风"), 408 + shift, kMicrophoneCheck);
    g_app->privacy_check = check_row(tr(L"允许隐私模式"), 408 + shift, kPrivacyModeCheck);
    g_app->carrier_check = check_row(tr(L"弱网回退（TCP/WebSocket）"), 408 + shift, kCarrierCheck);

    left = 24;
    const auto row_button = [&left](const wchar_t* text, int top, int height, int id,
                                    int min_width) {
        const int width = std::max(min_width, button_px(text));
        HWND control =
            add_control(L"BUTTON", text, BS_PUSHBUTTON, 0, left, top, width, height, id, nullptr);
        left += width + 10;
        return control;
    };
    row_button(tr(L"安全设置…"), 444 + shift, 30, kSecurityButton, 116);
    row_button(tr(L"连接服务设置…"), 444 + shift, 30, kTunnelSettingsButton, 150);
    row_button(tr(L"打开设置文件"), 444 + shift, 30, kOpenConfigButton, 104);
    row_button(tr(L"允许防火墙访问…"), 444 + shift, 30, kFirewallButton, 160);

    left = 24;
    g_app->toggle_button = row_button(tr(L"开始共享"), 482 + shift, 32, kToggleButton, 110);
    row_button(tr(L"重新注册"), 482 + shift, 32, kTunnelRegisterButton, 104);
    row_button(tr(L"重新加载设置"), 482 + shift, 32, kReloadButton, 120);
    row_button(tr(L"隐藏到托盘"), 482 + shift, 32, kHideButton, 110);
    row_button(tr(L"退出"), 482 + shift, 32, kQuitButton, 80);

    add_control(L"STATIC", tr(L"日志"), SS_LEFT, 0, 24, 524 + shift, 200, 20, 0, nullptr);
    g_app->log_edit =
        add_control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                    WS_EX_CLIENTEDGE, 24, 544 + shift, content, 150, kLogEdit, nullptr);

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
    kDialogHostKey,
    kDialogId,
    kDialogKey,
    kDialogListen,
    kDialogProxy,
    kDialogEnabled,
};

struct TunnelDialogState {
    HWND window = nullptr;
    HWND url = nullptr;
    HWND server_key = nullptr;
    HWND host_key = nullptr;
    HWND id = nullptr;
    HWND key = nullptr;
    HWND listen = nullptr;
    HWND proxy = nullptr;
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

    // Registration and signed audit uploads use this role key; empty keeps the
    // legacy behavior where the management key is sent instead.
    const std::string host_key = trim_ascii(narrow(control_text(dialog.host_key)));
    if (!valid_server_key(host_key)) {
        error = "主机密钥需要最多 256 个可打印字符，且不能包含引号或反斜杠";
        return false;
    }
    updated.host_key = host_key;

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

    const std::string proxy = trim_ascii(narrow(control_text(dialog.proxy)));
    if (proxy.empty()) {
        updated.proxy.clear();
    } else {
        if (!parse_proxy_url(proxy, normalized)) {
            error = "出站代理地址无效，需要 http://、https://、socks5:// 或 socks5h:// 且包含端口";
            return false;
        }
        updated.proxy = normalized;
    }
    return validate_tunnel_settings(updated, &error);
}

void accept_tunnel_dialog(TunnelDialogState& dialog) {
    HostSettings updated = g_app->settings;
    updated.tunnel = SendMessageW(dialog.enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::string error;
    if (!collect_tunnel_settings(dialog, updated, error)) {
        MessageBoxW(dialog.window, widen(error).c_str(), tr(L"连接服务设置"),
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
    const int dialog_width = chast::host::english_ui() ? 660 : 540;
    const int tunnel_content = dialog_width - 40;
    RECT frame{0, 0, px(dialog_width), px(588)};
    AdjustWindowRectEx(&frame, style, FALSE, WS_EX_DLGMODALFRAME);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kTunnelDialogClass,
                                  tr(g_app->machine_settings_editor ? L"服务连接设置" : L"连接服务设置"),
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
        tr(L"服务器地址留空表示不注册，只在本机或局域网共享。设备密钥不会上传，"
        L"服务端只保存它的摘要。"),
        SS_LEFT, 0, 20, 14, tunnel_content, 40, 0, nullptr);
    add(L"STATIC", tr(L"服务器地址（http:// 或 https://）"), SS_LEFT, 0, 20, 64, 400, 20, 0,
        nullptr);
    dialog.url = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 86,
                     tunnel_content, 26, kDialogUrl, nullptr);
    add(L"STATIC", tr(L"服务器密钥（可选）"), SS_LEFT, 0, 20, 122, 400, 20, 0, nullptr);
    dialog.server_key = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20,
                            144, tunnel_content, 26, kDialogServerKey, nullptr);
    add(L"STATIC", tr(L"主机密钥（可选，注册与审计用；留空则使用服务器密钥）"), SS_LEFT, 0, 20,
        180, tunnel_content, 20, 0, nullptr);
    dialog.host_key = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 202,
                          tunnel_content, 26, kDialogHostKey, nullptr);
    add(L"STATIC", tr(L"设备 ID（会合服务中的名称）"), SS_LEFT, 0, 20, 238, 400, 20, 0, nullptr);
    dialog.id = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 260,
                    tunnel_content, 26, kDialogId, nullptr);
    add(L"STATIC", tr(L"设备密钥（8 个字符以上，不会被上传）"), SS_LEFT, 0, 20, 296, 400, 20, 0,
        nullptr);
    dialog.key = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 318,
                     tunnel_content, 26, kDialogKey, nullptr);
    add(L"STATIC", tr(L"隧道监听地址（UDP）"), SS_LEFT, 0, 20, 354, 240, 20, 0, nullptr);
    dialog.listen = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 376,
                        240, 26, kDialogListen, nullptr);
    const std::wstring port_hint =
        tr(L"主机 QUIC 端口：") + std::to_wstring(g_app->settings.port) +
        tr(L"\n隧道端口必须不同，例如 0.0.0.0:14434");
    add(L"STATIC", port_hint.c_str(), SS_LEFT, 0, 276, 374, dialog_width - 20 - 276, 40, 0,
        nullptr);
    add(L"STATIC", tr(L"出站代理（可选，http://、https://、socks5:// 或 socks5h://，含端口）"),
        SS_LEFT, 0, 20, 414, tunnel_content, 20, 0, nullptr);
    dialog.proxy = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 20, 436,
                       tunnel_content, 26, kDialogProxy, nullptr);
    const wchar_t* enable_text = tr(L"启用连接服务（注册到会合服务器）");
    dialog.enabled = add(L"BUTTON", enable_text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 20, 472,
                         std::max(400, check_px(enable_text)), 24, kDialogEnabled, nullptr);
    const int tunnel_cancel_left = dialog_width - 20 - 100;
    const int tunnel_save_width = g_app->machine_settings_editor ? 220 : 100;
    add(L"BUTTON", tr(g_app->machine_settings_editor ? L"保存并重启服务" : L"保存"), BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
        tunnel_cancel_left - 10 - tunnel_save_width, 514, tunnel_save_width, 32, IDOK, nullptr);
    add(L"BUTTON", tr(L"取消"), BS_PUSHBUTTON | WS_TABSTOP, 0, tunnel_cancel_left, 514, 100, 32,
        IDCANCEL, nullptr);

    set_text(dialog.url, widen(g_app->settings.server_url));
    set_text(dialog.server_key, widen(g_app->settings.server_key));
    set_text(dialog.host_key, widen(g_app->settings.host_key));
    set_text(dialog.id, widen(g_app->settings.tunnel_id));
    set_text(dialog.key, widen(g_app->settings.tunnel_key));
    set_text(dialog.listen, widen(g_app->settings.tunnel_listen));
    set_text(dialog.proxy, widen(g_app->settings.proxy));
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
    kSecTrustedDays,
    kSecWhitelist,
    kSecAllowInput,
    kSecAllowClipboard,
    kSecAllowFile,
    kSecReceivePick,
    kSecAllowAudio,
    kSecAllowMicrophone,
    kSecAllowPrivacy,
    kSecAllowPortForward,
    kSecAllowTerminal,
    kSecAllowTerminalPersist,
    kSecAllowRecord,
    kSecRecordIncoming,
    kSecRecordDirectory,
    kSecRecordPick,
    kSecAllowCamera,
    kSecAllowRestart,
    kSecAllowBlockInput,
    kSecAllowOsLogin,
    kSecAllowSessionSelect,
    kSecAllowRemoteConfig,
    kSecLockAfterEnd,
    kSecKeepAwake,
    kSecClipboardDirection,
    kSecIdleMinutes,
    kSecAllowChange,
    kSecTokenAuth,
    kSecOneTime,
    kSecTokenEdit,
    kSecTokenCopy,
    kSecTokenRotate,
    kSecAudioDevice,
    kSecAudioRefresh,
    kSecAudioAec,
    kSecAudioGate,
    kSecVirtualDisplayPolicy,
    kSecVirtualDisplayCount,
    kSecVirtualDisplayResolution,
    kSecPrivacyModeImpl,
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
    HWND trusted_days = nullptr;
    HWND whitelist = nullptr;
    HWND allow_input = nullptr;
    HWND allow_clipboard = nullptr;
    HWND clipboard_direction = nullptr;
    HWND allow_file = nullptr;
    HWND receive_dir = nullptr;
    // Chosen receive/shared folder in UTF-8; empty keeps the default. It only
    // reaches the settings when the dialog is saved.
    std::string receive_directory;
    HWND allow_port_forward = nullptr;
    HWND allow_terminal = nullptr;
    HWND allow_terminal_persist = nullptr;
    HWND allow_record = nullptr;
    HWND record_incoming = nullptr;
    HWND record_directory = nullptr;
    HWND allow_camera = nullptr;
    HWND allow_restart = nullptr;
    HWND allow_block_input = nullptr;
    HWND allow_os_login = nullptr;
    HWND allow_session_select = nullptr;
    HWND allow_remote_config = nullptr;
    HWND lock_after_end = nullptr;
    HWND keep_awake = nullptr;
    HWND idle_minutes = nullptr;
    HWND allow_audio = nullptr;
    HWND allow_microphone = nullptr;
    HWND allow_privacy = nullptr;
    HWND allow_change = nullptr;
    HWND token_auth = nullptr;
    HWND one_time = nullptr;
    HWND token_edit = nullptr;
    HWND audio_device = nullptr;
    HWND audio_aec = nullptr;
    HWND audio_gate = nullptr;
    HWND virtual_display_policy = nullptr;
    HWND virtual_display_count = nullptr;
    HWND virtual_display_resolution = nullptr;
    HWND privacy_mode_impl = nullptr;
    // Parallel to the combo box: index 0 is the system default.
    std::vector<std::string> audio_device_ids;
    std::string selected_audio_device;
    std::vector<HWND> auth_page;
    std::vector<HWND> access_page;
    std::vector<HWND> advanced_page;
    std::vector<HWND> recording_page;
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
        return tr(L"从未");
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

void refresh_trusted_list(SecurityDialogState& dialog) {
    SendMessageW(dialog.trusted_list, LB_RESETCONTENT, 0, 0);
    for (const TrustedDevice& device : dialog.devices) {
        std::wstring line = device.name.empty() ? std::wstring(tr(L"未命名设备"))
                                                : widen(device.name);
        line += L" · " + platform_label(device.platform);
        const int64_t seen =
            device.last_seen_unix_seconds != 0 ? device.last_seen_unix_seconds
                                               : device.added_unix_seconds;
        line += tr(L" · 最近使用 ") + unix_date(seen);
        SendMessageW(dialog.trusted_list, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(line.c_str()));
    }
}

std::wstring receive_directory_text(const std::string& directory) {
    if (directory.empty() && g_app->machine_settings_editor)
        return tr(L"接收目录：服务账户默认目录");
    const std::string effective =
        directory.empty() ? chast::default_receive_directory() : directory;
    return tr(L"接收目录：") +
           (effective.empty() ? std::wstring(tr(L"（不可用）")) : widen(effective));
}

void show_security_page(SecurityDialogState& dialog, int page) {
    if (page < 0 || page > 3) {
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
    show(dialog.recording_page, page == 3);
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
                 reinterpret_cast<LPARAM>(tr(L"系统默认（跟随 Windows）")));
    dialog.audio_device_ids.emplace_back();
    int chosen = 0;

    std::string error;
    for (const AudioOutputDevice& output : list_audio_outputs(&error)) {
        if (output.id.empty()) {
            continue;
        }
        std::wstring label = widen(output.name.empty() ? output.id : output.name);
        if (output.is_default) {
            label += tr(L"（默认）");
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
                     reinterpret_cast<LPARAM>(tr(L"已选设备（当前不可用）")));
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
    set_text(dialog.two_factor_state, tr(L"待验证：把密钥加入认证器，再输入 6 位验证码"));
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
            MessageBoxW(dialog.window, tr(L"关闭两步验证需要先输入当前密码。"), tr(L"安全设置"),
                        MB_OK | MB_ICONWARNING);
            SetFocus(dialog.password_current);
            return;
        }
    }
    dialog.two_factor_disable_pending = true;
    dialog.pending_totp.clear();
    dialog.pending_totp_confirmed = false;
    set_text(dialog.two_factor_state, tr(L"已标记关闭，点击“保存”生效"));
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
                    tr(L"验证码不正确，请按认证器当前显示的 6 位数字重新输入。"), tr(L"安全设置"),
                    MB_OK | MB_ICONWARNING);
        SendMessageW(dialog.totp_code, EM_SETSEL, 0, -1);
        SetFocus(dialog.totp_code);
        return;
    }
    dialog.pending_totp_confirmed = true;
    set_text(dialog.two_factor_state, tr(L"验证码正确，点击“保存”启用"));
    EnableWindow(dialog.totp_code, FALSE);
    EnableWindow(dialog.totp_confirm, FALSE);
}

bool accept_security_dialog(SecurityDialogState& dialog) {
    const auto complain = [&dialog](const std::wstring& text) {
        MessageBoxW(dialog.window, text.c_str(), tr(L"安全设置"), MB_OK | MB_ICONWARNING);
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
    const auto direction = SendMessageW(dialog.clipboard_direction, CB_GETCURSEL, 0, 0);
    updated.clipboard_direction = direction == 0 ? ClipboardDirection::Both :
        direction == 1 ? ClipboardDirection::HostToViewer :
        direction == 2 ? ClipboardDirection::ViewerToHost : ClipboardDirection::Disabled;
    updated.allow_file_transfer = is_checked(dialog.allow_file);
    updated.allow_port_forward = is_checked(dialog.allow_port_forward);
    updated.allow_terminal = is_checked(dialog.allow_terminal);
    updated.allow_terminal_persist = is_checked(dialog.allow_terminal_persist);
    updated.allow_record = is_checked(dialog.allow_record);
    updated.record_incoming = is_checked(dialog.record_incoming);
    updated.record_directory = trim_ascii(narrow(control_text(dialog.record_directory)));
    if (updated.record_incoming && (updated.record_directory.empty() ||
        !std::filesystem::path(widen(updated.record_directory)).is_absolute())) {
        return complain(tr(L"启用主机录屏前，请选择完整的本机保存目录。"));
    }
    updated.allow_camera = is_checked(dialog.allow_camera);
    updated.allow_restart = is_checked(dialog.allow_restart);
    updated.allow_block_input = is_checked(dialog.allow_block_input);
    updated.allow_os_login = is_checked(dialog.allow_os_login);
    updated.allow_session_select = is_checked(dialog.allow_session_select);
    updated.allow_remote_config = is_checked(dialog.allow_remote_config);
    updated.lock_after_session_end = is_checked(dialog.lock_after_end);
    updated.keep_awake = is_checked(dialog.keep_awake);
    if (dialog.idle_minutes != nullptr) {
        const std::string minutes_text =
            trim_ascii(narrow(control_text(dialog.idle_minutes)));
        unsigned long minutes = 0;
        const bool digits =
            !minutes_text.empty() && minutes_text.size() <= 5 &&
            std::all_of(minutes_text.begin(), minutes_text.end(),
                        [](char c) { return c >= '0' && c <= '9'; });
        if (!digits ||
            (minutes = std::strtoul(minutes_text.c_str(), nullptr, 10)) > 10080) {
            return complain(tr(L"空闲断开必须是 0 到 10080 之间的整数分钟。"));
        }
        updated.auto_disconnect_minutes = static_cast<uint32_t>(minutes);
    }
    if (dialog.trusted_days != nullptr) {
        const std::string days_text = trim_ascii(narrow(control_text(dialog.trusted_days)));
        unsigned long days = 0;
        const bool digits =
            !days_text.empty() && days_text.size() <= 4 &&
            std::all_of(days_text.begin(), days_text.end(),
                        [](char c) { return c >= '0' && c <= '9'; });
        if (!digits || (days = std::strtoul(days_text.c_str(), nullptr, 10)) > 3650) {
            return complain(tr(L"可信设备有效期必须是 0 到 3650 之间的整数天。"));
        }
        updated.trusted_device_days = static_cast<uint32_t>(days);
    }
    updated.allow_audio = is_checked(dialog.allow_audio);
    updated.allow_microphone = is_checked(dialog.allow_microphone);
    updated.allow_privacy_mode = is_checked(dialog.allow_privacy);
    updated.allow_permission_change = is_checked(dialog.allow_change);
    updated.file_receive_directory = dialog.receive_directory;

    std::string whitelist = narrow(control_text(dialog.whitelist));
    std::replace(whitelist.begin(), whitelist.end(), '\r', ',');
    std::replace(whitelist.begin(), whitelist.end(), '\n', ',');
    std::vector<IpRange> ranges;
    std::string whitelist_error;
    if (!parse_ip_whitelist(whitelist, ranges, &whitelist_error)) {
        return complain(tr(L"IP 白名单无效：") + widen(whitelist_error));
    }
    std::string normalized;
    for (const IpRange& range : ranges) {
        if (!normalized.empty()) {
            normalized += ",";
        }
        normalized += format_ip_range(range);
    }
    updated.ip_whitelist = normalized;

    const std::wstring current = control_text(dialog.password_current);
    const std::wstring next = control_text(dialog.password_new);
    const std::wstring confirm = control_text(dialog.password_confirm);
    if (!next.empty() || !confirm.empty()) {
        if (next.size() < kMinPermanentPasswordLength) {
            return complain(tr(L"新密码至少需要 8 个字符。"));
        }
        if (next != confirm) {
            return complain(tr(L"两次输入的新密码不一致。"));
        }
        const std::string encoded = narrow(next);
        if (encoded.size() > 128) {
            return complain(tr(L"新密码太长（最多 128 字节）。"));
        }
        if (!updated.password_hash.empty() &&
            (current.empty() || !verify_password(narrow(current), updated.password_hash))) {
            return complain(tr(L"当前密码不正确。"));
        }
        updated.password_hash = hash_password(encoded);
        // A device trusted under the old secret must prove itself again.
        updated.trusted_devices.clear();
    }

    if (dialog.two_factor_disable_pending) {
        updated.totp_secret.clear();
    } else if (!dialog.pending_totp.empty()) {
        if (!dialog.pending_totp_confirmed) {
            return complain(tr(L"请先用认证器输入 6 位验证码并点击“确认验证码”。"));
        }
        std::string protected_secret;
        std::string error;
        ScopedUserContext original(g_app->identity_user_token.get());
        if (!original.ok() || !protect_totp_secret(dialog.pending_totp, g_app->machine_settings_editor ? SecretScope::LocalMachine : SecretScope::CurrentUser,
                                 protected_secret, &error)) {
            return complain(tr(L"无法保护两步验证密钥：") + widen(error));
        }
        updated.totp_secret = std::move(protected_secret);
    }
    if (g_app->machine_settings_editor && updated.auth_mode == HostAuthMode::Password && updated.password_hash.empty())
        return complain(tr(L"服务的密码模式需要设置永久密码。"));

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
    if (dialog.audio_gate != nullptr) {
        // Keep a custom threshold (host.ini) when the box stays checked; the
        // checkbox only distinguishes "off" from the default 2 s.
        updated.audio_silence_gate_ms =
            is_checked(dialog.audio_gate)
                ? (updated.audio_silence_gate_ms != 0 ? updated.audio_silence_gate_ms : 2000)
                : 0;
    }

    if (dialog.virtual_display_policy != nullptr) {
        const LRESULT selected =
            SendMessageW(dialog.virtual_display_policy, CB_GETCURSEL, 0, 0);
        updated.virtual_display = selected == 1
                                      ? VirtualDisplayPolicy::Auto
                                      : selected == 2 ? VirtualDisplayPolicy::On
                                                      : VirtualDisplayPolicy::Off;
    }
    {
        const std::string count_text =
            trim_ascii(narrow(control_text(dialog.virtual_display_count)));
        const bool digits = !count_text.empty() && count_text.size() <= 1 &&
                            std::all_of(count_text.begin(), count_text.end(), [](char c) {
                                return c >= '0' && c <= '9';
                            });
        const unsigned long count =
            digits ? std::strtoul(count_text.c_str(), nullptr, 10) : 0;
        if (count < 1 || count > kMaxVirtualDisplayMonitors) {
            return complain(tr(L"虚拟显示器数量必须是 1 到 4 之间的整数。"));
        }
        updated.virtual_display_monitors = static_cast<uint32_t>(count);
    }
    if (dialog.virtual_display_resolution != nullptr) {
        wchar_t resolution[64]{};
        const LRESULT selected =
            SendMessageW(dialog.virtual_display_resolution, CB_GETCURSEL, 0, 0);
        if (selected >= 0 &&
            SendMessageW(dialog.virtual_display_resolution, CB_GETLBTEXT, selected,
                         reinterpret_cast<LPARAM>(resolution)) != CB_ERR) {
            std::string normalized;
            if (!parse_virtual_display_resolution(narrow(resolution), normalized)) {
                return complain(tr(L"虚拟显示器分辨率必须是 WIDTHxHEIGHT。"));
            }
            updated.virtual_display_resolution = normalized;
        }
    }
    if (dialog.privacy_mode_impl != nullptr) {
        const LRESULT selected =
            SendMessageW(dialog.privacy_mode_impl, CB_GETCURSEL, 0, 0);
        updated.privacy_mode_impl = selected == 1 ? "virtual_display" : "overlay";
    }

    updated.token_auth = is_checked(dialog.token_auth);
    updated.token_mode = is_checked(dialog.one_time) ? HostTokenMode::OneTime
                                                     : HostTokenMode::Reusable;
    if (dialog.token_edited) {
        const std::string token = trim_ascii(narrow(control_text(dialog.token_edit)));
        if (token.empty() || token.size() > 128) {
            return complain(tr(L"访问令牌不能为空，且不能超过 128 个字符。"));
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
                case kSecRecordPick: {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    BROWSEINFOW browse{};
                    browse.hwndOwner = dialog->window;
                    browse.lpszTitle = tr(L"选择主机录像保存目录");
                    browse.ulFlags = BIF_RETURNONLYFSDIRS | (SUCCEEDED(com) ? BIF_NEWDIALOGSTYLE : 0);
                    const auto item = SHBrowseForFolderW(&browse);
                    wchar_t path[MAX_PATH]{};
                    if (item && SHGetPathFromIDListW(item, path)) set_text(dialog->record_directory, path);
                    if (item) CoTaskMemFree(item);
                    if (SUCCEEDED(com)) CoUninitialize();
                    return 0;
                }
                case kSecReceivePick: {
                    // BIF_NEWDIALOGSTYLE needs an initialized apartment; the
                    // classic dialog still works if COM is unavailable.
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    PIDLIST_ABSOLUTE start = nullptr;
                    const std::string current = dialog->receive_directory.empty() && !g_app->machine_settings_editor
                                                    ? chast::default_receive_directory()
                                                    : dialog->receive_directory;
                    if (!current.empty() && SUCCEEDED(com)) {
                        SHParseDisplayName(widen(current).c_str(), nullptr, &start, 0, nullptr);
                    }
                    BROWSEINFOW browse{};
                    browse.hwndOwner = dialog->window;
                    browse.pidlRoot = start;
                    browse.lpszTitle =
                        tr(L"选择接收目录（查看器可浏览并下载其中的文件）");
                    browse.ulFlags = BIF_RETURNONLYFSDIRS;
                    if (SUCCEEDED(com)) {
                        browse.ulFlags |= BIF_NEWDIALOGSTYLE;
                    }
                    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
                    wchar_t path[MAX_PATH]{};
                    const bool picked =
                        item != nullptr && SHGetPathFromIDListW(item, path) != FALSE;
                    if (item != nullptr) {
                        CoTaskMemFree(item);
                    }
                    if (start != nullptr) {
                        CoTaskMemFree(start);
                    }
                    if (SUCCEEDED(com)) {
                        CoUninitialize();
                    }
                    if (!picked) {
                        return 0;
                    }
                    dialog->receive_directory = narrow(path);
                    set_text(dialog->receive_dir,
                             receive_directory_text(dialog->receive_directory));
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
    const int dialog_width = (chast::host::english_ui() ? 700 : 620) + (g_app->machine_settings_editor ? 80 : 0);
    const int page_text_width = dialog_width - 80;
    RECT frame{0, 0, px(dialog_width), px(784)};
    AdjustWindowRectEx(&frame, style, FALSE, WS_EX_DLGMODALFRAME);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kSecurityDialogClass,
                                  tr(g_app->machine_settings_editor ? L"服务安全设置" : L"安全设置"),
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
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP, px(16), px(12),
                                 px(dialog_width - 32), px(692), window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSecTab)),
                                 g_app->instance, nullptr);
    if (dialog.tab != nullptr) {
        SendMessageW(dialog.tab, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->font), TRUE);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tr(L"认证"));
        TabCtrl_InsertItem(dialog.tab, 0, &item);
        item.pszText = const_cast<wchar_t*>(tr(L"访问控制"));
        TabCtrl_InsertItem(dialog.tab, 1, &item);
        item.pszText = const_cast<wchar_t*>(tr(L"高级"));
        TabCtrl_InsertItem(dialog.tab, 2, &item);
        item.pszText = const_cast<wchar_t*>(tr(L"主机录屏"));
        TabCtrl_InsertItem(dialog.tab, 3, &item);
    }

    // ----- authentication page -----
    add(L"STATIC", tr(L"接入方式"), SS_LEFT, 0, 32, 56, 200, 20, 0, nullptr,
        &dialog.auth_page);
    const wchar_t* mode_password_text = tr(L"密码");
    const wchar_t* mode_click_text = tr(L"点击批准");
    const wchar_t* mode_both_text = tr(L"两者");
    const int mode_password_width = std::max(110, button_px(mode_password_text));
    const int mode_click_width = std::max(130, button_px(mode_click_text));
    dialog.mode_password =
        add(L"BUTTON", mode_password_text, BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 0, 32, 78,
            mode_password_width, 24, kSecModePassword, nullptr, &dialog.auth_page);
    dialog.mode_click =
        add(L"BUTTON", mode_click_text, BS_AUTORADIOBUTTON | WS_TABSTOP, 0,
            32 + mode_password_width + 20, 78, mode_click_width, 24, kSecModeClick, nullptr,
            &dialog.auth_page);
    dialog.mode_both =
        add(L"BUTTON", mode_both_text, BS_AUTORADIOBUTTON | WS_TABSTOP, 0,
            32 + mode_password_width + 20 + mode_click_width + 20, 78,
            std::max(130, button_px(mode_both_text)), 24, kSecModeBoth, nullptr,
            &dialog.auth_page);
    add(L"STATIC",
        tr(L"密码：必须输入密码；点击批准：每次连接都弹窗；两者：有密码就验证，"
        L"空密码请求批准。"),
        SS_LEFT, 0, 32, 104, page_text_width, 36, 0, nullptr, &dialog.auth_page);
    add(L"STATIC", tr(L"修改永久密码（留空表示不修改）"), SS_LEFT, 0, 32, 144, 300, 20, 0,
        nullptr, &dialog.auth_page);
    add(L"STATIC", tr(L"当前密码（修改时需要）"), SS_LEFT, 0, 32, 168, 170, 20, 0, nullptr,
        &dialog.auth_page);
    add(L"STATIC", tr(L"新密码（至少 8 个字符）"), SS_LEFT, 0, 214, 168, 170, 20, 0, nullptr,
        &dialog.auth_page);
    add(L"STATIC", tr(L"确认新密码"), SS_LEFT, 0, 396, 168, 170, 20, 0, nullptr,
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
    add(L"STATIC", tr(L"修改永久密码会清空可信设备；两步验证密钥保留。"), SS_LEFT, 0, 32, 220,
        page_text_width, 20, 0, nullptr, &dialog.auth_page);
    add(L"STATIC", tr(L"两步验证（TOTP）"), SS_LEFT, 0, 32, 248, 200, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.two_factor_state =
        add(L"STATIC", L"", SS_LEFT, 0, 300, 248, 270, 20, kSecTwoFactorState, nullptr,
            &dialog.auth_page);
    const wchar_t* two_factor_enable_text = tr(L"启用两步验证");
    const wchar_t* two_factor_disable_text = tr(L"关闭两步验证");
    const int two_factor_enable_width = std::max(120, button_px(two_factor_enable_text));
    dialog.two_factor_enable =
        add(L"BUTTON", two_factor_enable_text, BS_PUSHBUTTON | WS_TABSTOP, 0, 32, 272,
            two_factor_enable_width, 28, kSecTwoFactorEnable, nullptr, &dialog.auth_page);
    dialog.two_factor_disable =
        add(L"BUTTON", two_factor_disable_text, BS_PUSHBUTTON | WS_TABSTOP, 0,
            32 + two_factor_enable_width + 10, 272,
            std::max(120, button_px(two_factor_disable_text)), 28, kSecTwoFactorDisable, nullptr,
            &dialog.auth_page);
    add(L"STATIC", tr(L"密钥（Base32，手动添加到认证器）"), SS_LEFT, 0, 32, 308, 400, 20, 0,
        nullptr, &dialog.auth_page);
    dialog.totp_secret =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 326, 540, 26,
            kSecTotpSecret, nullptr, &dialog.auth_page);
    add(L"STATIC", tr(L"otpauth 链接"), SS_LEFT, 0, 32, 358, 400, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.totp_uri =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 376, 540, 26,
            kSecTotpUri, nullptr, &dialog.auth_page);
    add(L"STATIC", tr(L"6 位验证码"), SS_LEFT, 0, 32, 408, 100, 20, 0, nullptr,
        &dialog.auth_page);
    dialog.totp_code =
        add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 32, 426, 120, 26,
            kSecTotpCode, nullptr, &dialog.auth_page);
    dialog.totp_confirm =
        add(L"BUTTON", tr(L"确认验证码"), BS_PUSHBUTTON | WS_TABSTOP, 0, 164, 426, 110, 26,
            kSecTotpConfirm, nullptr, &dialog.auth_page);
    add(L"STATIC", tr(L"输入认证器当前显示的 6 位数字"), SS_LEFT, 0, 286, 430, 280, 20, 0,
        nullptr, &dialog.auth_page);

    // ----- access-control page -----
    const int page_right = dialog_width - 36;
    add(L"STATIC", tr(L"可信设备（通过两步验证后可以记住这台查看器）"), SS_LEFT, 0, 32, 56, 500,
        20, 0, nullptr, &dialog.access_page);
    dialog.trusted_list =
        add(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP | WS_BORDER, 0, 32, 78,
            440, 110, kSecTrustedList, nullptr, &dialog.access_page);
    add(L"BUTTON", tr(L"撤销信任"), BS_PUSHBUTTON | WS_TABSTOP, 0, 484, 78, 92, 30, kSecRevoke,
        nullptr, &dialog.access_page);
    const wchar_t* trusted_days_label = tr(L"可信设备有效期（天，0 表示永不过期）");
    const int trusted_days_label_width = text_px(trusted_days_label) + 8;
    add(L"STATIC", trusted_days_label, SS_LEFT, 0, 32, 196, trusted_days_label_width, 20, 0,
        nullptr, &dialog.access_page);
    dialog.trusted_days =
        add(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
            32 + trusted_days_label_width + 12, 192, 80, 26, kSecTrustedDays, nullptr,
            &dialog.access_page);
    const int trusted_note_left = 32 + trusted_days_label_width + 12 + 80 + 16;
    add(L"STATIC", tr(L"超过未使用时长后需要重新两步验证；修改永久密码会清空列表。"),
        SS_LEFT | SS_ENDELLIPSIS, 0, trusted_note_left, 196, page_right - trusted_note_left, 20,
        0, nullptr, &dialog.access_page);
    add(L"STATIC", tr(L"IP 白名单（IPv4 或 CIDR，逗号或换行分隔，留空不限制）"), SS_LEFT, 0,
        32, 224, page_text_width, 20, 0, nullptr, &dialog.access_page);
    dialog.whitelist =
        add(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
            WS_EX_CLIENTEDGE, 32, 246, dialog_width - 76, 64, kSecWhitelist, nullptr,
            &dialog.access_page);
    add(L"STATIC", tr(L"经中继连接时，主机看到的是中继地址。"), SS_LEFT, 0, 32, 316,
        page_text_width, 20, 0, nullptr, &dialog.access_page);
    add(L"STATIC", tr(L"新连接的默认权限"), SS_LEFT, 0, 32, 344, 300, 20, 0, nullptr,
        &dialog.access_page);
    const auto page_check = [&](const wchar_t* text, int left, int top, int id) {
        return add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, left, top, check_px(text), 24,
                   id, nullptr, &dialog.access_page);
    };
    const wchar_t* allow_input_text = tr(L"键盘与鼠标");
    const wchar_t* allow_clipboard_text = tr(L"剪贴板");
    const wchar_t* allow_audio_text = tr(L"系统声音");
    const wchar_t* allow_privacy_text = tr(L"隐私模式（本机黑屏）");
    const int allow_input_width = std::max(120, check_px(allow_input_text));
    const int allow_clipboard_width = std::max(110, check_px(allow_clipboard_text));
    const int allow_audio_width = std::max(110, check_px(allow_audio_text));
    dialog.allow_input = page_check(allow_input_text, 32, 366, kSecAllowInput);
    dialog.allow_clipboard =
        page_check(allow_clipboard_text, 32 + allow_input_width + 12, 366, kSecAllowClipboard);
    dialog.allow_audio = page_check(
        allow_audio_text, 32 + allow_input_width + allow_clipboard_width + 24, 366,
        kSecAllowAudio);
    dialog.allow_privacy = page_check(
        allow_privacy_text,
        32 + allow_input_width + allow_clipboard_width + allow_audio_width + 36, 366,
        kSecAllowPrivacy);

    const wchar_t* allow_file_text = tr(L"文件传输");
    const wchar_t* allow_microphone_text = tr(L"麦克风（上行）");
    const int allow_file_width = std::max(110, check_px(allow_file_text));
    const int allow_microphone_width = std::max(130, check_px(allow_microphone_text));
    dialog.allow_file = page_check(allow_file_text, 32, 394, kSecAllowFile);
    dialog.allow_microphone =
        page_check(allow_microphone_text, 32 + allow_file_width + 8, 394, kSecAllowMicrophone);
    const int receive_left = 32 + allow_file_width + 8 + allow_microphone_width + 10;
    const wchar_t* receive_pick_text = tr(L"更改…");
    const int receive_pick_width = std::max(72, button_px(receive_pick_text));
    dialog.receive_directory = g_app->settings.file_receive_directory;
    const std::wstring receive_text = receive_directory_text(dialog.receive_directory);
    dialog.receive_dir =
        add(L"STATIC", receive_text.c_str(), SS_LEFT | SS_ENDELLIPSIS, 0, receive_left, 398,
            page_right - receive_left - receive_pick_width - 8, 20, 0, nullptr,
            &dialog.access_page);
    add(L"BUTTON", receive_pick_text, BS_PUSHBUTTON | WS_TABSTOP, 0,
        page_right - receive_pick_width, 396, receive_pick_width, 26, kSecReceivePick, nullptr,
        &dialog.access_page);

    const wchar_t* allow_port_forward_text = tr(L"端口转发");
    dialog.allow_port_forward = page_check(allow_port_forward_text, 32, 428, kSecAllowPortForward);
    const int port_forward_left = 32 + std::max(120, check_px(allow_port_forward_text)) + 10;
    add(L"STATIC", tr(L"允许查看器经本机访问你可达的 TCP 地址。"), SS_LEFT, 0, port_forward_left,
        432, page_right - port_forward_left, 20, 0, nullptr, &dialog.access_page);

    const wchar_t* allow_change_text = tr(L"允许在会话中调整权限");
    const wchar_t* allow_record_text = tr(L"允许查看者录制会话");
    const int allow_change_width = std::max(260, check_px(allow_change_text));
    dialog.allow_change = page_check(allow_change_text, 32, 460, kSecAllowChange);
    dialog.allow_record =
        page_check(allow_record_text, 32 + allow_change_width + 16, 460, kSecAllowRecord);

    const wchar_t* allow_terminal_text = tr(L"远程终端");
    dialog.allow_terminal = page_check(allow_terminal_text, 32, 488, kSecAllowTerminal);
    // The persist box shares this row: it only means anything with the
    // permission beside it, and the page has no spare row left.
    const wchar_t* terminal_persist_text = tr(L"断线后保留");
    const int terminal_persist_width = std::max(120, check_px(terminal_persist_text));
    dialog.allow_terminal_persist = page_check(terminal_persist_text,
                                               page_right - terminal_persist_width, 488,
                                               kSecAllowTerminalPersist);
    const int terminal_left = 32 + std::max(120, check_px(allow_terminal_text)) + 10;
    add(L"STATIC", tr(L"允许查看器打开本机 shell。"), SS_LEFT, 0, terminal_left, 492,
        page_right - terminal_left - terminal_persist_width - 16, 20, 0, nullptr,
        &dialog.access_page);

    const wchar_t* allow_camera_text = tr(L"摄像头");
    dialog.allow_camera = page_check(allow_camera_text, 32, 516, kSecAllowCamera);
    const int camera_left = 32 + std::max(120, check_px(allow_camera_text)) + 10;
    add(L"STATIC", tr(L"允许查看器串流本机的一台摄像头（与桌面画面同时）。"), SS_LEFT, 0,
        camera_left, 520, page_right - camera_left, 20, 0, nullptr, &dialog.access_page);

    const wchar_t* allow_restart_text = tr(L"远程重启主机");
    const wchar_t* allow_block_input_text = tr(L"允许阻止本地输入");
    const wchar_t* allow_os_login_text = tr(L"系统登录");
    const int allow_restart_width = std::max(160, check_px(allow_restart_text));
    const int allow_block_input_width = std::max(160, check_px(allow_block_input_text));
    dialog.allow_restart = page_check(allow_restart_text, 32, 544, kSecAllowRestart);
    dialog.allow_block_input =
        page_check(allow_block_input_text, 32 + allow_restart_width + 24, 544,
                   kSecAllowBlockInput);
    dialog.allow_os_login =
        page_check(allow_os_login_text, 32 + allow_restart_width + 24 +
                                            allow_block_input_width + 24,
                   544, kSecAllowOsLogin);
    add(L"STATIC", tr(L"阻止本地输入需要键盘与鼠标权限；系统登录需要键盘权限，且只在登录或锁屏界面生效。"),
        SS_LEFT, 0, 32, 570, page_text_width, 36, 0, nullptr, &dialog.access_page);

    const wchar_t* keep_awake_text = tr(L"远程连接期间防止自动休眠");
    dialog.keep_awake = page_check(keep_awake_text, 32, 612, kSecKeepAwake);
    add(L"STATIC", tr(L"桌面连接同时保持屏幕唤醒；文件、终端等连接仅防止系统休眠。"),
        SS_LEFT, 0, 32, 640, page_text_width, 28, 0, nullptr, &dialog.access_page);

    const wchar_t* direction_label = tr(L"剪贴板方向");
    const int direction_left = 32 + text_px(direction_label) + 16;
    add(L"STATIC", direction_label, SS_LEFT, 0, 32, 676, direction_left - 40, 20, 0,
        nullptr, &dialog.access_page);
    dialog.clipboard_direction = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
        0, direction_left, 672, page_right - direction_left, 150, kSecClipboardDirection,
        nullptr, &dialog.access_page);
    for (const auto* label : {tr(L"双向同步"), tr(L"仅本机 → 查看端"), tr(L"仅查看端 → 本机"), tr(L"禁止同步")}) {
        SendMessageW(dialog.clipboard_direction, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }

    // ----- advanced page -----
    add(L"STATIC", tr(L"访问令牌（仅供 CLI 与自动化，默认关闭）"), SS_LEFT, 0, 32, 56, 500, 20, 0,
        nullptr, &dialog.advanced_page);
    dialog.token_auth =
        add(L"BUTTON", tr(L"允许令牌认证"), BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 80, 200, 24,
            kSecTokenAuth, nullptr, &dialog.advanced_page);
    dialog.one_time =
        add(L"BUTTON", tr(L"一次性令牌（使用后自动更换）"), BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32,
            106, 260, 24, kSecOneTime, nullptr, &dialog.advanced_page);
    dialog.token_edit =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 134, 408, 26,
            kSecTokenEdit, nullptr, &dialog.advanced_page);
    const wchar_t* token_copy_text = tr(L"复制");
    const wchar_t* token_rotate_text = tr(L"生成");
    const int token_copy_width = std::max(60, button_px(token_copy_text));
    add(L"BUTTON", token_copy_text, BS_PUSHBUTTON | WS_TABSTOP, 0, 448, 134, token_copy_width, 26,
        kSecTokenCopy, nullptr, &dialog.advanced_page);
    add(L"BUTTON", token_rotate_text, BS_PUSHBUTTON | WS_TABSTOP, 0,
        448 + token_copy_width + 8, 134, std::max(62, button_px(token_rotate_text)), 26,
        kSecTokenRotate, nullptr, &dialog.advanced_page);
    add(L"STATIC", tr(L"查看器不再使用访问令牌；只有勾选“允许令牌认证”后令牌才有效。"),
        SS_LEFT, 0, 32, 166, 540, 36, 0, nullptr, &dialog.advanced_page);
    add(L"STATIC", tr(L"身份指纹（SHA-256）"), SS_LEFT, 0, 32, 212, 300, 20, 0, nullptr,
        &dialog.advanced_page);
    HWND fingerprint =
        add(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 32, 234,
            dialog_width - 76, 26, 0, nullptr, &dialog.advanced_page);
    add(L"STATIC", tr(L"查看器首次连接时固定此指纹；变更后会提示身份不匹配。"), SS_LEFT, 0, 32,
        266, page_text_width, 20, 0, nullptr, &dialog.advanced_page);

    add(L"STATIC", tr(L"系统声音输出（捕获与查看者麦克风共用）"), SS_LEFT, 0, 32, 300, 500, 20,
        0, nullptr, &dialog.advanced_page);
    dialog.audio_device = add(L"COMBOBOX", L"",
                              CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                              WS_EX_CLIENTEDGE, 32, 322, 460, 200, kSecAudioDevice, nullptr,
                              &dialog.advanced_page);
    add(L"BUTTON", tr(L"刷新"), BS_PUSHBUTTON | WS_TABSTOP, 0, 500, 321, 76, 28,
        kSecAudioRefresh, nullptr, &dialog.advanced_page);
    const wchar_t* audio_aec_text = tr(L"回声消除（以查看者麦克风播放为参考）");
    dialog.audio_aec =
        add(L"BUTTON", audio_aec_text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 358,
            std::max(400, check_px(audio_aec_text)), 24, kSecAudioAec, nullptr,
            &dialog.advanced_page);
    const wchar_t* audio_gate_text = tr(L"静音 2 秒后暂停发送系统声音");
    dialog.audio_gate =
        add(L"BUTTON", audio_gate_text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 386,
            std::max(400, check_px(audio_gate_text)), 24, kSecAudioGate, nullptr,
            &dialog.advanced_page);
    add(L"STATIC", tr(L"留空跟随系统默认；固定设备不可用时系统声音不会启动。"), SS_LEFT, 0, 32,
        414, page_text_width, 20, 0, nullptr, &dialog.advanced_page);

    const wchar_t* allow_session_select_text = tr(L"允许切换 Windows 会话（仅服务模式）");
    dialog.allow_session_select =
        add(L"BUTTON", allow_session_select_text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 450,
            std::max(360, check_px(allow_session_select_text)), 24, kSecAllowSessionSelect,
            nullptr, &dialog.advanced_page);
    add(L"STATIC",
        tr(L"查看器可列出并切换到本机其他已登录的 Windows 会话；手工运行主机时该项不会生效。"),
        SS_LEFT, 0, 32, 478, page_text_width, 36, 0, nullptr, &dialog.advanced_page);

    const wchar_t* allow_remote_config_text = tr(L"允许查看者修改本页的开关（远程配置）");
    dialog.allow_remote_config =
        add(L"BUTTON", allow_remote_config_text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 520,
            std::max(400, check_px(allow_remote_config_text)), 24, kSecAllowRemoteConfig,
            nullptr, &dialog.advanced_page);
    const wchar_t* lock_after_end_text = tr(L"最后一个查看者离开后锁定本机");
    dialog.lock_after_end =
        add(L"BUTTON", lock_after_end_text, BS_AUTOCHECKBOX | WS_TABSTOP, 0, 32, 548,
            std::max(400, check_px(lock_after_end_text)), 24, kSecLockAfterEnd, nullptr,
            &dialog.advanced_page);
    add(L"STATIC",
        tr(L"远程配置需要单独授予权限；关闭某个开关会在本会话立即撤销对应权限，开启则从下次连接生效。"),
        SS_LEFT, 0, 32, 576, page_text_width, 24, 0, nullptr, &dialog.advanced_page);
    const wchar_t* virtual_display_label = tr(L"虚拟显示器");
    add(L"STATIC", virtual_display_label, SS_LEFT, 0, 32, 610, text_px(virtual_display_label) + 8,
        20, 0, nullptr, &dialog.advanced_page);
    const int virtual_display_left = 32 + text_px(virtual_display_label) + 12;
    dialog.virtual_display_policy =
        add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
            WS_EX_CLIENTEDGE, virtual_display_left, 606, 260, 200, kSecVirtualDisplayPolicy,
            nullptr, &dialog.advanced_page);
    const wchar_t* privacy_impl_label = tr(L"隐私方式");
    const int privacy_impl_left = virtual_display_left + 260 + 16;
    add(L"STATIC", privacy_impl_label, SS_LEFT, 0, privacy_impl_left, 610,
        text_px(privacy_impl_label) + 8, 20, 0, nullptr, &dialog.advanced_page);
    dialog.privacy_mode_impl =
        add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
            WS_EX_CLIENTEDGE, privacy_impl_left + text_px(privacy_impl_label) + 8, 606, 120,
            200, kSecPrivacyModeImpl, nullptr, &dialog.advanced_page);
    const wchar_t* virtual_count_label = tr(L"数量（1–4）");
    const int virtual_count_label_width = text_px(virtual_count_label);
    add(L"STATIC", virtual_count_label, SS_LEFT, 0, 32, 640, virtual_count_label_width + 8, 20, 0,
        nullptr, &dialog.advanced_page);
    dialog.virtual_display_count =
        add(L"EDIT", L"1", ES_NUMBER | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
            32 + virtual_count_label_width + 12, 636, 60, 26, kSecVirtualDisplayCount, nullptr,
            &dialog.advanced_page);
    const wchar_t* virtual_resolution_label = tr(L"分辨率");
    const int virtual_resolution_left =
        std::max(32 + virtual_count_label_width + 12 + 60 + 20, 208);
    add(L"STATIC", virtual_resolution_label, SS_LEFT, 0, virtual_resolution_left, 640,
        text_px(virtual_resolution_label) + 8, 20, 0, nullptr, &dialog.advanced_page);
    dialog.virtual_display_resolution =
        add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
            WS_EX_CLIENTEDGE,
            virtual_resolution_left + text_px(virtual_resolution_label) + 12, 636, 180, 200,
            kSecVirtualDisplayResolution, nullptr, &dialog.advanced_page);
    const std::wstring virtual_display_line =
        virtual_display_text(probe_virtual_display());
    add(L"STATIC", virtual_display_line.c_str(), SS_LEFT | SS_ENDELLIPSIS, 0, 32, 674,
        page_text_width, 20, 0, nullptr, &dialog.advanced_page);

    // ----- local recording page -----
    const wchar_t* recording_text = tr(L"自动保存远程会话视频到本机");
    dialog.record_incoming = add(L"BUTTON", recording_text, BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 32, 60, page_text_width, 26, kSecRecordIncoming, nullptr, &dialog.recording_page);
    add(L"STATIC", tr(L"录像保存目录"), SS_LEFT, 0, 32, 108, page_text_width, 22,
        0, nullptr, &dialog.recording_page);
    dialog.record_directory = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
        32, 138, page_text_width - 100, 28, kSecRecordDirectory, nullptr, &dialog.recording_page);
    add(L"BUTTON", tr(L"浏览…"), BS_PUSHBUTTON | WS_TABSTOP, 0, 32 + page_text_width - 90,
        138, 90, 28, kSecRecordPick, nullptr, &dialog.recording_page);
    add(L"STATIC", tr(L"启用前请选择保存目录。服务模式下，服务账户必须有该目录的写入权限。"),
        SS_LEFT, 0, 32, 184, page_text_width, 48, 0, nullptr, &dialog.recording_page);
    add(L"STATIC", tr(L"录制当前被观看的屏幕、合并桌面和摄像头，仅保存视频，不包含声音。"),
        SS_LEFT, 0, 32, 250, page_text_width, 48, 0, nullptr, &dialog.recording_page);
    add(L"STATIC", tr(L"多人观看同一画面只保存一份。切换格式或每满 10 分钟会另存一段 MP4，最后一个观看者离开后保存完成。"),
        SS_LEFT, 0, 32, 316, page_text_width, 64, 0, nullptr, &dialog.recording_page);
    add(L"STATIC", tr(L"此设置只允许在本机修改，与允许查看者录制相互独立。录像不会自动删除；保存失败会显示在主机日志中。"),
        SS_LEFT, 0, 32, 398, page_text_width, 64, 0, nullptr, &dialog.recording_page);

    const int sec_cancel_left = dialog_width - 20 - 90;
    const int save_y = 720;
    const int save_width = g_app->machine_settings_editor ? 220 : 90;
    add(L"BUTTON", tr(g_app->machine_settings_editor ? L"保存并重启服务" : L"保存"), BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
        sec_cancel_left - 10 - save_width, save_y, save_width, 32, kSecSave, nullptr, nullptr);
    add(L"BUTTON", tr(L"取消"), BS_PUSHBUTTON | WS_TABSTOP, 0, sec_cancel_left, save_y, 90, 32,
        kSecCancel, nullptr, nullptr);

    // The advanced page has no free row left, so the idle timeout sits beside
    // the buttons. It applies to every session, not just the advanced tab, so
    // it stays visible on all pages like save and cancel.
    const wchar_t* idle_label = tr(L"空闲断开（分钟，0 表示关闭）");
    const int idle_label_width = text_px(idle_label) + 8;
    add(L"STATIC", idle_label, SS_LEFT, 0, 32, 724, idle_label_width, 20, 0, nullptr,
        nullptr);
    dialog.idle_minutes =
        add(L"EDIT", L"", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
            32 + idle_label_width + 12, 720, 80, 26, kSecIdleMinutes, nullptr, nullptr);
    add(L"STATIC", tr(L"无输入、聊天或剪贴板操作达到此时长即关闭会话。"), SS_LEFT, 0, 32,
        756, page_text_width, 20, 0, nullptr, nullptr);

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
    SendMessageW(dialog.clipboard_direction, CB_SETCURSEL,
        settings.clipboard_direction == ClipboardDirection::Both ? 0 :
        settings.clipboard_direction == ClipboardDirection::HostToViewer ? 1 :
        settings.clipboard_direction == ClipboardDirection::ViewerToHost ? 2 : 3, 0);
    set_check(dialog.allow_file, settings.allow_file_transfer);
    set_check(dialog.allow_port_forward, settings.allow_port_forward);
    set_check(dialog.allow_terminal, settings.allow_terminal);
    set_check(dialog.allow_terminal_persist, settings.allow_terminal_persist);
    set_check(dialog.allow_record, settings.allow_record);
    set_check(dialog.record_incoming, settings.record_incoming);
    set_text(dialog.record_directory, widen(settings.record_directory));
    set_check(dialog.allow_camera, settings.allow_camera);
    set_check(dialog.allow_restart, settings.allow_restart);
    set_check(dialog.allow_block_input, settings.allow_block_input);
    set_check(dialog.allow_os_login, settings.allow_os_login);
    set_check(dialog.allow_session_select, settings.allow_session_select);
    set_check(dialog.allow_remote_config, settings.allow_remote_config);
    set_check(dialog.lock_after_end, settings.lock_after_session_end);
    set_check(dialog.keep_awake, settings.keep_awake);
    set_text(dialog.idle_minutes, widen(std::to_string(settings.auto_disconnect_minutes)));
    set_text(dialog.trusted_days, widen(std::to_string(settings.trusted_device_days)));
    set_check(dialog.allow_audio, settings.allow_audio);
    set_check(dialog.allow_microphone, settings.allow_microphone);
    set_check(dialog.allow_privacy, settings.allow_privacy_mode);
    set_check(dialog.allow_change, settings.allow_permission_change);
    set_check(dialog.token_auth, settings.token_auth);
    set_check(dialog.one_time, settings.token_mode == HostTokenMode::OneTime);
    set_text(dialog.token_edit, widen(settings.access_token));
    set_text(fingerprint,
             g_app->server ? widen(format_fingerprint(g_app->server->status().fingerprint))
                           : std::wstring(tr(g_app->machine_settings_editor ? L"请在服务连接管理中查看当前指纹" : L"共享未启动")));
    dialog.selected_audio_device = settings.audio_device;
    set_check(dialog.audio_aec, settings.audio_aec);
    set_check(dialog.audio_gate, settings.audio_silence_gate_ms != 0);
    for (const wchar_t* item :
         {tr(L"关闭"), tr(L"无显示器时自动启用"), tr(L"总是启用")}) {
        SendMessageW(dialog.virtual_display_policy, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(item));
    }
    const int policy_index = settings.virtual_display == VirtualDisplayPolicy::Auto
                                 ? 1
                                 : settings.virtual_display == VirtualDisplayPolicy::On ? 2 : 0;
    SendMessageW(dialog.virtual_display_policy, CB_SETCURSEL, policy_index, 0);
    for (const wchar_t* item : {tr(L"黑屏遮罩"), tr(L"虚拟显示器")}) {
        SendMessageW(dialog.privacy_mode_impl, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(dialog.privacy_mode_impl, CB_SETCURSEL,
                 settings.privacy_mode_impl == "virtual_display" ? 1 : 0, 0);
    set_text(dialog.virtual_display_count,
             widen(std::to_string(settings.virtual_display_monitors)));
    int resolution_index = -1;
    int resolution_count = 0;
    for (const VirtualDisplayMode& mode : usbmmidd_default_modes()) {
        const std::wstring item =
            widen(std::to_string(mode.width) + "x" + std::to_string(mode.height));
        SendMessageW(dialog.virtual_display_resolution, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(item.c_str()));
        if (item == widen(settings.virtual_display_resolution)) {
            resolution_index = resolution_count;
        }
        ++resolution_count;
    }
    if (resolution_index < 0) {
        const std::wstring item = widen(settings.virtual_display_resolution);
        SendMessageW(dialog.virtual_display_resolution, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(item.c_str()));
        resolution_index = resolution_count;
    }
    SendMessageW(dialog.virtual_display_resolution, CB_SETCURSEL, resolution_index, 0);
    refresh_audio_devices(dialog);

    if (!settings.totp_secret.empty()) {
        set_text(dialog.two_factor_state, tr(L"已启用"));
        set_text(dialog.totp_secret, tr(L"（已启用；密钥只在设置时显示）"));
        EnableWindow(dialog.two_factor_enable, FALSE);
        EnableWindow(dialog.two_factor_disable, TRUE);
    } else {
        set_text(dialog.two_factor_state, tr(L"已关闭"));
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
    wcsncpy_s(data.szTip, tr(L"chast 主机 · 已停止"), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        g_app->tray_added = true;
        g_app->tray_tip = tr(L"chast 主机 · 已停止");
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

bool session_is_live();

void show_tray_menu() {
    POINT point{};
    GetCursorPos(&point);
    const bool running = g_app->server && g_app->server->running();
    HostStatus status;
    if (g_app->server) {
        status = g_app->server->status();
    }
    const std::wstring line =
        tr(L"状态：") + (g_app->server ? state_text(status.state, status) : std::wstring(tr(L"已停止")));

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kMenuShow, tr(L"显示 chast 主机"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, line.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCopyIdButton, tr(L"复制设备 ID"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_input ? MF_CHECKED : 0), kInputCheck,
                tr(L"接受远程输入"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_clipboard ? MF_CHECKED : 0),
                kClipboardCheck, tr(L"同步剪贴板"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_file_transfer ? MF_CHECKED : 0),
                kFileTransferCheck, tr(L"接受文件传输"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_port_forward ? MF_CHECKED : 0),
                kPortForwardCheck, tr(L"允许端口转发（查看器访问本机 TCP）"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_terminal ? MF_CHECKED : 0),
                kTerminalCheck, tr(L"允许远程终端（查看器在本机开 shell）"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_record ? MF_CHECKED : 0),
                kRecordCheck, tr(L"允许查看者录制会话"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_camera ? MF_CHECKED : 0),
                kCameraCheck, tr(L"允许查看器串流本机摄像头"));
    const auto viewers = g_app->server ? g_app->server->viewers() : std::vector<HostViewer>{};
    const bool can_send = std::any_of(viewers.begin(), viewers.end(), [](const auto& viewer) {
        return (viewer.permissions & kPermissionFileTransfer) != 0;
    });
    AppendMenuW(menu, MF_STRING | (can_send ? 0 : MF_GRAYED), kSendFileButton,
                tr(L"发送文件到查看器…"));
    const bool can_chat = running;
    AppendMenuW(menu, MF_STRING | (can_chat ? 0 : MF_GRAYED), kChatButton,
                tr(L"管理连接与聊天…"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_audio ? MF_CHECKED : 0), kAudioCheck,
                tr(L"共享系统声音"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_microphone ? MF_CHECKED : 0),
                kMicrophoneCheck, tr(L"接收查看者麦克风"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_privacy_mode ? MF_CHECKED : 0),
                kPrivacyModeCheck, tr(L"允许查看者开启隐私模式"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_restart ? MF_CHECKED : 0),
                kRestartCheck, tr(L"允许查看者远程重启主机"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_block_input ? MF_CHECKED : 0),
                kBlockInputCheck, tr(L"允许查看者阻止本地输入"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_os_login ? MF_CHECKED : 0),
                kOsLoginCheck, tr(L"允许查看者进行系统登录（输入密码）"));
    AppendMenuW(menu,
                MF_STRING | (g_app->settings.allow_session_select ? MF_CHECKED : 0),
                kSessionSelectCheck, tr(L"允许查看者切换 Windows 会话（仅服务模式）"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_remote_config ? MF_CHECKED : 0),
                kRemoteConfigCheck, tr(L"允许查看者修改主机设置"));
    AppendMenuW(menu,
                MF_STRING | (g_app->settings.lock_after_session_end ? MF_CHECKED : 0),
                kLockAfterEndCheck, tr(L"会话结束后锁定本机"));
    AppendMenuW(menu, MF_STRING | (g_app->settings.carrier ? MF_CHECKED : 0), kCarrierCheck,
                tr(L"弱网回退（TCP/WebSocket）"));
    AppendMenuW(menu,
                MF_STRING | (g_app->portable ? MF_GRAYED
                                             : (g_app->settings.autostart ? MF_CHECKED : 0)),
                kAutostartCheck, tr(L"开机自动启动"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kSecurityButton, tr(L"安全设置…"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, tunnel_text().c_str());
    const VirtualDisplayProbe vdisplay_probe = probe_virtual_display();
    const VirtualDisplayStatus vdisplay_status =
        classify_virtual_display_probe(vdisplay_probe);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, virtual_display_text(vdisplay_probe).c_str());
    if (!vdisplay_probe.conflicting_driver.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                    tr(L"安装虚拟显示器（检测到驱动冲突）"));
    } else if (vdisplay_status.state == VirtualDisplayState::NotInstalled) {
        const bool have_package = path_is_directory(default_virtual_display_package_dir());
        AppendMenuW(menu, MF_STRING | (have_package ? 0 : MF_GRAYED), kVdisplayInstallButton,
                    have_package ? tr(L"安装虚拟显示器…")
                                 : tr(L"安装虚拟显示器（缺少驱动包）"));
    } else if (vdisplay_status.state == VirtualDisplayState::Installed ||
               vdisplay_status.state == VirtualDisplayState::Active) {
        VirtualDisplayInstallRecord record;
        const bool owned = read_install_record(&record) && record.owned;
        if (owned && session_is_live()) {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                        tr(L"卸载虚拟显示器（请先结束会话）"));
        } else if (owned) {
            AppendMenuW(menu, MF_STRING, kVdisplayUninstallButton, tr(L"卸载虚拟显示器…"));
        } else {
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                        tr(L"虚拟显示器由其他程序安装，chast 不会卸载。"));
        }
    }
    AppendMenuW(menu, MF_STRING, kTunnelSettingsButton, tr(L"连接服务设置…"));
    AppendMenuW(menu, MF_STRING, kTunnelRegisterButton, tr(L"重新注册连接服务"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kToggleButton, running ? tr(L"停止共享") : tr(L"开始共享"));
    AppendMenuW(menu, MF_STRING, kOpenConfigButton, tr(L"打开设置文件"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitButton, tr(L"退出"));

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
        append_log_line(tr(L"已重新创建设置文件：") + widen(g_app->config_path));
    } else {
        append_log_line(tr(L"设置已重新加载"));
    }
    if (!g_app->startup_warning.empty()) {
        append_log_line(tr(L"设置文件有警告：") + widen(g_app->startup_warning));
    }
    if (!g_app->startup_save_error.empty()) {
        append_log_line(tr(L"保存设置失败：") + widen(g_app->startup_save_error));
    }
    start_server();
    refresh_ui();
}

bool session_is_live() {
    return g_app->server && g_app->server->running() &&
           g_app->server->status().state == HostState::Streaming;
}

void handle_command(int id) {
    if (g_app->elevation_transferring) return;
    switch (id) {
        case kMenuShow:
            show_window();
            return;
        case kCopyIdButton:
            chast::host::copy_text(g_app->window, widen(g_app->settings.tunnel_id));
            append_log_line(tr(L"设备 ID 已复制到剪贴板"));
            break;
        case kCopyPasswordButton: {
            std::string password;
            if (g_app->server) {
                password = g_app->server->status().temporary_password;
            }
            if (password.empty()) {
                append_log_line(tr(L"当前没有临时密码（点击批准模式）"));
            } else {
                chast::host::copy_text(g_app->window, widen(password));
                append_log_line(tr(L"临时密码已复制到剪贴板"));
            }
            break;
        }
        case kRefreshPasswordButton: {
            if (!g_app->server) {
                append_log_line(tr(L"共享未启动，无法生成临时密码"));
                break;
            }
            const std::string password = g_app->server->refresh_temporary_password();
            append_log_line(password.empty() ? std::wstring(tr(L"无法生成临时密码"))
                                             : tr(L"已生成新的临时密码 ") + widen(password));
            break;
        }
        case kSecurityButton: {
            HostSettings updated;
            if (edit_security_settings(updated)) {
                g_app->settings = updated;
                save_settings();
                restart_server();
                append_log_line(tr(L"安全设置已保存，共享已重新启动"));
            }
            break;
        }
        case kInputCheck:
            g_app->settings.allow_input = !g_app->settings.allow_input;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_input
                                    ? tr(L"远程输入已开启（本次会话实时生效）")
                                    : tr(L"远程输入已关闭（本次会话实时生效）"));
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_input ? tr(L"远程输入已开启")
                                                            : tr(L"远程输入已关闭"));
            }
            break;
        case kClipboardCheck:
            g_app->settings.allow_clipboard = !g_app->settings.allow_clipboard;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_clipboard
                                    ? tr(L"剪贴板同步已开启（本次会话实时生效）")
                                    : tr(L"剪贴板同步已关闭（本次会话实时生效）"));
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_clipboard ? tr(L"剪贴板同步已开启")
                                                                : tr(L"剪贴板同步已关闭"));
            }
            break;
        case kSendFileButton:
            connection_window().show();
            break;
        case kChatButton:
            connection_window().show();
            break;
        case kFileTransferCheck:
            g_app->settings.allow_file_transfer = !g_app->settings.allow_file_transfer;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_file_transfer
                                    ? tr(L"文件传输已开启（本次会话实时生效）")
                                    : tr(L"文件传输已关闭（本次会话实时生效）"));
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_file_transfer ? tr(L"文件传输已开启")
                                                                    : tr(L"文件传输已关闭"));
            }
            break;
        case kPortForwardCheck:
            g_app->settings.allow_port_forward = !g_app->settings.allow_port_forward;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_port_forward
                                    ? tr(L"端口转发已开启（本次会话实时生效）")
                                    : tr(L"端口转发已关闭（本次会话实时生效）"));
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_port_forward
                                    ? tr(L"端口转发已开启")
                                    : tr(L"端口转发已关闭"));
            }
            break;
        case kTerminalCheck:
            g_app->settings.allow_terminal = !g_app->settings.allow_terminal;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_terminal
                                    ? tr(L"远程终端已开启（本次会话实时生效）")
                                    : tr(L"远程终端已关闭（本次会话实时生效）"));
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_terminal ? tr(L"远程终端已开启")
                                                               : tr(L"远程终端已关闭"));
            }
            break;
        case kRecordCheck:
            g_app->settings.allow_record = !g_app->settings.allow_record;
            save_settings();
            if (session_is_live()) {
                const HostStatus status = g_app->server->status();
                g_app->server->set_permissions(current_gate_mask());
                if (g_app->settings.allow_record &&
                    (status.session_permissions & kPermissionRecord) == 0) {
                    // The permission ceiling was fixed at handshake, so a
                    // session that started without consent cannot gain it.
                    append_log_line(
                        tr(L"已允许查看者录制会话；当前会话未获授权，将在下次连接时生效"));
                } else if (g_app->settings.allow_record) {
                    append_log_line(tr(L"已允许查看者录制会话（本次会话实时生效）"));
                } else {
                    append_log_line(
                        tr(L"已撤回录制权限（本次会话实时生效，查看者将停止新的录制）"));
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_record
                                    ? tr(L"已允许查看者录制会话")
                                    : tr(L"已撤回查看者的录制权限"));
            }
            break;
        case kCameraCheck:
            g_app->settings.allow_camera = !g_app->settings.allow_camera;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_camera
                                    ? tr(L"摄像头已开启（本次会话实时生效）")
                                    : tr(L"摄像头已关闭（本次会话实时生效）"));
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_camera ? tr(L"摄像头已开启")
                                                             : tr(L"摄像头已关闭"));
            }
            break;
        case kAudioCheck:
            g_app->settings.allow_audio = !g_app->settings.allow_audio;
            save_settings();
            if (session_is_live()) {
                g_app->server->set_permissions(current_gate_mask());
                append_log_line(g_app->settings.allow_audio
                                    ? tr(L"系统声音共享已开启（本次会话实时生效）")
                                    : tr(L"系统声音共享已关闭（本次会话实时生效）"));
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_audio ? tr(L"系统声音共享已开启")
                                                            : tr(L"系统声音共享已关闭"));
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
                        tr(L"已开启接收查看者麦克风；当前监听器未启用，将在下次连接时生效"));
                } else {
                    append_log_line(g_app->settings.allow_microphone
                                        ? tr(L"接收查看者麦克风已开启（本次会话实时生效）")
                                        : tr(L"接收查看者麦克风已关闭（本次会话实时生效）"));
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_microphone
                                    ? tr(L"接收查看者麦克风已开启")
                                    : tr(L"接收查看者麦克风已关闭"));
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
                        tr(L"已允许隐私模式；当前监听器未启用，将在下次连接时生效"));
                } else if (g_app->settings.allow_privacy_mode) {
                    append_log_line(tr(L"已允许隐私模式（本次会话实时生效）"));
                } else {
                    append_log_line(tr(L"隐私模式已关闭（本次会话实时生效，遮罩会立即撤下）"));
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_privacy_mode ? tr(L"已允许隐私模式")
                                                                   : tr(L"隐私模式已关闭"));
            }
            break;
        case kRestartCheck: {
            g_app->settings.allow_restart = !g_app->settings.allow_restart;
            save_settings();
            if (session_is_live()) {
                const HostStatus status = g_app->server->status();
                g_app->server->set_permissions(current_gate_mask());
                if (g_app->settings.allow_restart &&
                    (status.session_permissions & kPermissionRestart) == 0) {
                    // The permission ceiling was fixed at handshake, so a
                    // session that started without consent cannot gain it.
                    append_log_line(
                        tr(L"已允许远程重启；当前会话未获授权，将在下次连接时生效"));
                } else if (g_app->settings.allow_restart) {
                    append_log_line(tr(L"已允许远程重启（本次会话实时生效）"));
                } else {
                    append_log_line(tr(L"已撤回远程重启权限（本次会话实时生效）"));
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_restart ? tr(L"已允许远程重启")
                                                              : tr(L"已撤回远程重启权限"));
            }
            break;
        }
        case kBlockInputCheck: {
            g_app->settings.allow_block_input = !g_app->settings.allow_block_input;
            save_settings();
            if (session_is_live()) {
                const HostStatus status = g_app->server->status();
                g_app->server->set_permissions(current_gate_mask());
                if (g_app->settings.allow_block_input &&
                    (status.session_permissions & kPermissionBlockInput) == 0) {
                    append_log_line(
                        tr(L"已允许阻止本地输入；当前会话未获授权，将在下次连接时生效"));
                } else if (g_app->settings.allow_block_input) {
                    append_log_line(tr(L"已允许阻止本地输入（本次会话实时生效）"));
                } else {
                    append_log_line(
                        tr(L"已撤回阻止本地输入权限（本次会话实时生效，输入会立即恢复）"));
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_block_input
                                    ? tr(L"已允许阻止本地输入")
                                    : tr(L"已撤回阻止本地输入权限"));
            }
            break;
        }
        case kOsLoginCheck: {
            g_app->settings.allow_os_login = !g_app->settings.allow_os_login;
            save_settings();
            if (session_is_live()) {
                const HostStatus status = g_app->server->status();
                g_app->server->set_permissions(current_gate_mask());
                if (g_app->settings.allow_os_login &&
                    (status.session_permissions & kPermissionOsLogin) == 0) {
                    // The permission ceiling was fixed at handshake; the host
                    // setting also needs a session opened after the change.
                    append_log_line(
                        tr(L"已允许系统登录；当前会话未获授权，将在下次连接时生效"));
                } else if (g_app->settings.allow_os_login) {
                    append_log_line(tr(L"已允许系统登录（本次会话实时生效）"));
                } else {
                    append_log_line(tr(L"已撤回系统登录权限（本次会话实时生效）"));
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_os_login ? tr(L"已允许系统登录")
                                                               : tr(L"已撤回系统登录权限"));
            }
            break;
        }
        case kSessionSelectCheck: {
            g_app->settings.allow_session_select = !g_app->settings.allow_session_select;
            save_settings();
            // A manual host has no service switch channel, so the worker never
            // advertises the permission; the setting only takes effect under
            // the service. Restarting is enough for the next handshake.
            restart_server();
            append_log_line(g_app->settings.allow_session_select
                                ? tr(L"已允许切换 Windows 会话（仅在服务模式下生效）")
                                : tr(L"已撤回切换 Windows 会话权限"));
            break;
        }
        case kRemoteConfigCheck: {
            g_app->settings.allow_remote_config = !g_app->settings.allow_remote_config;
            save_settings();
            if (session_is_live()) {
                const HostStatus status = g_app->server->status();
                g_app->server->set_permissions(current_gate_mask());
                if (g_app->settings.allow_remote_config &&
                    (status.session_permissions & kPermissionRemoteConfig) == 0) {
                    append_log_line(
                        tr(L"已允许远程修改主机设置；当前会话未获授权，将在下次连接时生效"));
                } else if (g_app->settings.allow_remote_config) {
                    append_log_line(tr(L"已允许远程修改主机设置（本次会话实时生效）"));
                } else {
                    append_log_line(tr(L"已撤回远程修改主机设置的权限（本次会话实时生效）"));
                }
            } else {
                restart_server();
                append_log_line(g_app->settings.allow_remote_config
                                    ? tr(L"已允许远程修改主机设置")
                                    : tr(L"已撤回远程修改主机设置的权限"));
            }
            break;
        }
        case kLockAfterEndCheck: {
            g_app->settings.lock_after_session_end =
                !g_app->settings.lock_after_session_end;
            save_settings();
            if (g_app->server) {
                g_app->server->set_lock_after_session_end(
                    g_app->settings.lock_after_session_end);
            }
            append_log_line(g_app->settings.lock_after_session_end
                                ? tr(L"最后一个查看者离开后将锁定本机")
                                : tr(L"会话结束后不再锁定本机"));
            break;
        }
        case kCarrierCheck:
            g_app->settings.carrier = !g_app->settings.carrier;
            save_settings();
            if (session_is_live()) {
                // The fallback listener is bound at start-up; changing it now
                // would drop the viewer, so the switch waits for the next run.
                append_log_line(g_app->settings.carrier
                                    ? tr(L"已开启弱网回退；将在下次开始共享时生效")
                                    : tr(L"已关闭弱网回退；将在下次开始共享时生效"));
            } else {
                restart_server();
                if (!g_app->settings.carrier) {
                    append_log_line(tr(L"弱网回退已关闭"));
                } else if (!g_app->server) {
                    append_log_line(tr(L"弱网回退已开启"));
                } else {
                    const HostStatus status = g_app->server->status();
                    append_log_line(status.carrier_listening
                                        ? tr(L"弱网回退已开启（端口 ") +
                                              std::to_wstring(status.carrier_port) + tr(L"）")
                                        : tr(L"弱网回退已开启，但监听失败：") +
                                              widen(status.carrier_error));
                }
            }
            break;
        case kAutostartCheck: {
            const bool enabled = !g_app->settings.autostart;
            std::string error;
            ScopedUserContext original(g_app->identity_user_token.get());
            if (!original.ok() || !chast::host::set_autostart(enabled, &error)) {
                append_log_line(tr(L"开机启动：") + widen(error));
                const std::wstring message = tr(L"无法修改开机启动项：\n") + widen(error);
                MessageBoxW(g_app->window, message.c_str(), window_title(),
                            MB_ICONERROR | MB_OK);
                break;
            }
            g_app->settings.autostart = enabled;
            save_settings();
            append_log_line(enabled ? tr(L"已加入开机启动") : tr(L"已从开机启动移除"));
            break;
        }
        case kFirewallButton: {
            append_log_line(tr(L"正在请求管理员权限以添加防火墙规则…"));
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            uint16_t tunnel_port = 0;
            if (g_app->settings.tunnel) {
                (void)tunnel_listen_port(g_app->settings.tunnel_listen, tunnel_port);
            }
            std::string error;
            if (chast::host::request_firewall_rule(g_app->settings.port, tunnel_port, &error)) {
                std::wstring ports = std::to_wstring(g_app->settings.port);
                if (tunnel_port != 0) {
                    ports += tr(L" 和 ") + std::to_wstring(tunnel_port);
                }
                append_log_line(tr(L"已允许 chast 在 UDP 端口 ") + ports + tr(L" 上通过防火墙"));
            } else {
                append_log_line(tr(L"防火墙：") + widen(error));
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            break;
        }
        case kVdisplayInstallButton: {
            if (MessageBoxW(g_app->window,
                            tr(L"安装虚拟显示器会改变本机的显示拓扑，并需要管理员权限。继续吗？"),
                            window_title(),
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
                break;
            }
            append_log_line(tr(L"正在请求管理员权限以安装虚拟显示器…"));
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            const std::wstring arguments =
                L"--vdisplay install --package \"" + default_virtual_display_package_dir() +
                L"\" --count " + std::to_wstring(g_app->settings.virtual_display_monitors);
            bool cancelled = false;
            std::string error;
            const int code = chast::host::run_elevated(arguments, 180000, &cancelled, &error);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            std::wstring message;
            if (code == 0) {
                const VirtualDisplayStatus status =
                    classify_virtual_display_probe(probe_virtual_display());
                message = tr(L"虚拟显示器已安装，当前启用 ") +
                          std::to_wstring(status.active_monitors) +
                          tr(L" 台活动显示器。");
                append_log_line(tr(L"虚拟显示器安装完成"));
            } else if (cancelled) {
                message = tr(L"已取消管理员授权。");
            } else {
                message = tr(L"虚拟显示器安装失败");
                if (code > 0) {
                    message += L" (" + std::to_wstring(code) + L")";
                }
                if (!error.empty()) {
                    message += L"\n\n" + widen(error);
                }
                const std::string tail = virtual_display_log_tail();
                if (!tail.empty()) {
                    message += L"\n\n" + widen(tail);
                }
                message += std::wstring(L"\n\n") + tr(L"日志：") + virtual_display_log_path();
            }
            MessageBoxW(g_app->window, message.c_str(), window_title(),
                        MB_ICONINFORMATION | MB_OK);
            break;
        }
        case kVdisplayUninstallButton: {
            if (session_is_live()) {
                MessageBoxW(g_app->window, tr(L"请先结束远程会话，再卸载虚拟显示器。"),
                            window_title(), MB_ICONINFORMATION | MB_OK);
                break;
            }
            if (MessageBoxW(g_app->window,
                            tr(L"卸载虚拟显示器会移除 chast 安装的驱动。请先结束远程会话。继续吗？"),
                            window_title(),
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
                break;
            }
            append_log_line(tr(L"正在请求管理员权限以卸载虚拟显示器…"));
            SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            bool cancelled = false;
            std::string error;
            const int code =
                chast::host::run_elevated(L"--vdisplay uninstall", 120000, &cancelled, &error);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            std::wstring message;
            if (code == 0) {
                message = tr(L"虚拟显示器已卸载。");
                append_log_line(message);
            } else if (cancelled) {
                message = tr(L"已取消管理员授权。");
            } else {
                message = tr(L"虚拟显示器卸载失败");
                if (code > 0) {
                    message += L" (" + std::to_wstring(code) + L")";
                }
                if (!error.empty()) {
                    message += L"\n\n" + widen(error);
                }
                const std::string tail = virtual_display_log_tail();
                if (!tail.empty()) {
                    message += L"\n\n" + widen(tail);
                }
                message += std::wstring(L"\n\n") + tr(L"日志：") + virtual_display_log_path();
            }
            MessageBoxW(g_app->window, message.c_str(), window_title(),
                        MB_ICONINFORMATION | MB_OK);
            break;
        }
        case kTunnelSettingsButton: {
            HostSettings updated;
            if (edit_tunnel_settings(updated)) {
                g_app->settings = updated;
                save_settings();
                restart_server();
                append_log_line(tr(L"连接服务设置已保存"));
            }
            break;
        }
        case kTunnelRegisterButton:
            if (!g_app->settings.tunnel) {
                append_log_line(tr(L"连接服务已关闭，请先在“连接服务设置…”中启用"));
            } else {
                append_log_line(tr(L"正在重新注册连接服务…"));
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
                poll_elevation();
                if (!IsWindow(hwnd) || g_app->elevation_transferring) return 0;
                // An already-open default tray yields after service install.
                // Portable/custom profiles retain their explicitly chosen mode.
                static ULONGLONG service_probe_at = 0;
                const auto now = GetTickCount64();
                if (now >= service_probe_at && !g_app->portable && g_app->config_path == host_settings_path()) {
                    service_probe_at = now + 2000;
                    if (chast::host::service_installed()) {
                        g_app->service_handoff = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
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
        case kPasswordHashMessage: {
            auto* hash = reinterpret_cast<std::string*>(lparam);
            g_app->settings.password_hash = *hash;
            delete hash;
            save_settings();
            return 0;
        }
        case kSettingsMessage: {
            auto* update = reinterpret_cast<HostSettings*>(lparam);
            apply_remote_switches(*update, g_app->settings);
            delete update;
            save_settings();
            append_log_line(tr(L"查看者修改了主机设置，已保存"));
            refresh_ui();
            return 0;
        }
        case kChatMessage: {
            std::unique_ptr<ViewerChatMessage> chat(reinterpret_cast<ViewerChatMessage*>(lparam));
            connection_window().receive_chat(chat->id, chat->text);
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
            append_log_line(tr(L"连接服务已断开：") + widen(g_app->tunnel_error));
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
            g_app->elevation->shutdown();
            g_app->quitting.store(true);
            g_app->connections.reset();
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

int run_machine_settings_editor(chast::host::ServiceSettingsPage page) {
    const auto argument = page == chast::host::ServiceSettingsPage::Security ? L"--service-settings" : L"--service-network-settings";
    if (!chast::host::service_settings_admin()) {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas",
            chast::host::executable_path().c_str(), argument, nullptr, SW_SHOWNORMAL));
        return result > 32 ? 0 : 1;
    }
    g_app->machine_settings_editor = true;
    g_app->config_path = machine_settings_path();
    const auto loaded = load_host_settings(g_app->config_path);
    if (!loaded.existed || !loaded.warning.empty()) {
        MessageBoxW(nullptr, tr(L"无法完整读取机器设置。请先安装主机服务，或修复配置文件中的错误。"),
            tr(L"主机服务设置"), MB_OK | MB_ICONERROR);
        return 1;
    }
    g_app->settings = loaded.settings;
    HDC screen = GetDC(nullptr); g_app->dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(nullptr, screen);
    create_fonts();
    g_app->window = CreateWindowExW(0, L"STATIC", tr(L"主机服务设置"), WS_OVERLAPPED | WS_CAPTION,
        (GetSystemMetrics(SM_CXSCREEN) - px(640)) / 2, (GetSystemMetrics(SM_CYSCREEN) - px(220)) / 2,
        px(640), px(220), nullptr, nullptr, g_app->instance, nullptr);
    if (!g_app->window) { DeleteObject(g_app->font); DeleteObject(g_app->title_font); return 1; }
    HostSettings edited;
    const bool accepted = page == chast::host::ServiceSettingsPage::Security ? edit_security_settings(edited) : edit_tunnel_settings(edited);
    int code = 0;
    if (accepted) {
        add_control(L"STATIC", tr(L"正在保存机器设置并重启主机服务…"), SS_LEFT, 0, 24, 40, 590, 55, 0, g_app->font);
        ShowWindow(g_app->window, SW_SHOWNORMAL); UpdateWindow(g_app->window);
        const auto original = loaded.settings;
        auto saving = std::async(std::launch::async, [original, edited, page] {
            return chast::host::save_service_settings(original, edited, page);
        });
        while (saving.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        const auto result = saving.get();
        if (!result.error.empty()) {
            const auto message = std::wstring(result.saved ? tr(L"设置已保存，但服务启动未完成：\n") : tr(L"设置未保存：\n")) + widen(result.error);
            MessageBoxW(g_app->window, message.c_str(), tr(L"主机服务设置"), MB_OK | MB_ICONERROR);
            code = 1;
        } else {
            MessageBoxW(g_app->window, result.running ? tr(L"机器设置已保存，主机服务已重启。") :
                tr(L"机器设置已保存，将在下次启动主机服务时生效。"), tr(L"主机服务设置"), MB_OK | MB_ICONINFORMATION);
        }
    }
    DestroyWindow(g_app->window); g_app->window = nullptr;
    DeleteObject(g_app->font); DeleteObject(g_app->title_font);
    g_app->font = g_app->title_font = nullptr;
    return code;
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
    load_ui_language();
    static HostApp app;
    g_app = &app;
    app.instance = instance;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string config_path = host_settings_path();
    bool config_overridden = false;
    bool start_hidden = false;
    bool portable = false;
    bool service_settings = false, service_network_settings = false, service_manager = false;
    std::wstring elevation_id;
    DWORD elevation_parent = 0;
    uint64_t elevation_birth = 0;
    for (int i = 1; argv != nullptr && i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--elevated-handoff") {
            if (argc != 5 || i != 1) { LocalFree(argv); return 2; }
            elevation_id = argv[++i];
            wchar_t* end = nullptr;
            elevation_parent = wcstoul(argv[++i], &end, 10);
            if (!end || *end || !elevation_parent) { LocalFree(argv); return 2; }
            elevation_birth = _wcstoui64(argv[++i], &end, 10);
            if (!end || *end || !elevation_birth) { LocalFree(argv); return 2; }
        } else if (arg == L"--config" && i + 1 < argc) {
            config_path = narrow(absolute_path(argv[++i]));
            config_overridden = true;
        } else if (arg == L"--portable") {
            portable = true;
        } else if (arg == L"--tray") {
            start_hidden = true;
        } else if (arg == L"--service-manager") {
            service_manager = true;
        } else if (arg == L"--service-settings") {
            service_settings = true;
        } else if (arg == L"--service-network-settings") {
            service_network_settings = true;
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
        } else if (arg == L"--vdisplay") {
            std::vector<std::wstring> vdisplay_args;
            for (int next = i + 1; next < argc; ++next) {
                vdisplay_args.emplace_back(argv[next]);
            }
            LocalFree(argv);
            return chast::run_vdisplay_command(vdisplay_args);
        } else if (arg == L"--help" || arg == L"-h") {
            MessageBoxW(nullptr,
                        tr(L"chast 主机\n\n"
                        L"--config <路径>     使用其他设置文件\n"
                        L"--portable          便携模式：设置保存在程序目录下，不写开机启动\n"
                        L"--tray              启动后隐藏窗口\n"
                        L"--service-manager   仅管理主机服务，不启动普通共享\n"
                        L"--autostart on|off  设置开机启动\n"
                        L"--service-settings  编辑服务安全设置（需要管理员）\n"
                        L"--service-network-settings  编辑服务连接设置（需要管理员）\n"
                        L"--vdisplay <命令>   虚拟显示器：install --package <目录> [--count 1-4]、uninstall、set-count <0-4>、status（安装/卸载需要管理员）\n"
                        L"--firewall <端口> [隧道端口]  添加防火墙规则（含局域网发现 21129/UDP，需要管理员）"),
                        window_title(), MB_OK | MB_ICONINFORMATION);
            LocalFree(argv);
            return 0;
        }
    }
    LocalFree(argv);
    if (!elevation_id.empty()) {
        std::string error;
        app.elevated_child = chast::host::ElevatedChild::receive(elevation_id, elevation_parent, elevation_birth, &error);
        if (!app.elevated_child) return 1;
        const auto& snapshot = app.elevated_child->snapshot;
        app.identity_user_token = app.elevated_child->identity_user_token;
        app.settings = snapshot.settings;
        app.instance_mutex = snapshot.instance_mutex;
        portable = snapshot.portable;
        start_hidden = snapshot.hidden;
        config_path = snapshot.config_path;
        config_overridden = true;
    }
    if (service_manager) {
        if (config_overridden || portable || service_settings || service_network_settings) return 2;
        return chast::host::run_service_manager(instance, start_hidden);
    }
    if (service_settings || service_network_settings) {
        if (config_overridden || portable || (service_settings && service_network_settings)) return 2;
        return run_machine_settings_editor(service_network_settings ? chast::host::ServiceSettingsPage::Connection :
                                                                   chast::host::ServiceSettingsPage::Security);
    }
    if (portable) {
        g_app->portable = true;
        if (!config_overridden) {
            config_path = chast::portable_settings_path(
                narrow(chast::host::executable_path()));
        }
    }
    if (config_path.empty()) {
        MessageBoxW(nullptr, tr(L"无法确定设置文件的位置。"), window_title(), MB_ICONERROR | MB_OK);
        return 1;
    }
    if (!app.elevated_child && !portable && config_path == host_settings_path() && chast::host::service_installed()) {
        return chast::host::run_service_manager(instance, start_hidden);
    }
    app.config_path = config_path;

    HANDLE instance_mutex = nullptr;
    if (!app.elevated_child && config_path == host_settings_path()) {
        instance_mutex = CreateMutexW(nullptr, FALSE, L"Local\\chast-host-tray");
        if (instance_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
                ShowWindow(existing, SW_SHOWNORMAL);
                SetForegroundWindow(existing);
            } else {
                MessageBoxW(nullptr, tr(L"chast 主机已经在运行。请查看通知区域。"), window_title(),
                            MB_ICONINFORMATION | MB_OK);
            }
            if (instance_mutex != nullptr) {
                CloseHandle(instance_mutex);
            }
            return 0;
        }
    }
    if (instance_mutex) app.instance_mutex = instance_mutex;

    HDC screen = GetDC(nullptr);
    app.dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (app.dpi <= 0) {
        app.dpi = 96;
    }

    if (!app.elevated_child) load_settings();

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
    RECT rect{0, 0, px(main_window_width()), px(main_window_height())};
    AdjustWindowRectEx(&rect, style, FALSE, 0);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    const HWND window = CreateWindowExW(0, kWindowClass, window_title(), style, x, y, width,
                                        height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        MessageBoxW(nullptr, tr(L"无法创建主机窗口。"), window_title(), MB_ICONERROR | MB_OK);
        return 1;
    }
    g_app->window = window;
    add_tray_icon();

    if (!g_app->startup_save_error.empty()) {
        append_log_line(tr(L"保存默认设置失败：") + widen(g_app->startup_save_error));
    }
    append_log_line(tr(L"设置文件：") + widen(g_app->config_path));
    if (!g_app->startup_warning.empty()) {
        append_log_line(tr(L"设置文件有警告：") + widen(g_app->startup_warning));
    }
    if (g_app->settings.autostart && !app.elevated_child) {
        std::string error;
        if (!chast::host::set_autostart(true, &error)) {
            append_log_line(tr(L"开机启动：") + widen(error));
        }
    }
    start_server();
    if (app.elevated_child) {
        std::string error;
        if (!app.server || !app.elevated_child->announce_ready(app.server->status().fingerprint, &error)) {
            DestroyWindow(window);
            return 1;
        }
    }
    if (g_app->server) {
        const HostStatus status = g_app->server->status();
        if (!status.fingerprint.empty()) {
            append_log_line(tr(L"身份 SHA256 ") + widen(format_fingerprint(status.fingerprint)));
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
        if (g_app->connections && g_app->connections->translate(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (instance_mutex != nullptr) {
        CloseHandle(instance_mutex);
    }
    if (g_app->service_handoff) return chast::host::run_service_manager(instance, start_hidden);
    return static_cast<int>(message.wParam);
}
