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
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "chast/host_server.h"
#include "chast/host_settings.h"
#include "chast/identity.h"
#include "chast/protocol.h"
#include "platform.h"
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
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT kTrayIconId = 1;

enum ControlId {
    kStateLabel = 1000,
    kPeerLabel,
    kStreamLabel,
    kTunnelLabel,
    kPathLabel,
    kTokenEdit,
    kFingerprintEdit,
    kCopyButton,
    kRegenerateButton,
    kInputCheck,
    kApproveCheck,
    kOneTimeCheck,
    kAutostartCheck,
    kFirewallButton,
    kOpenConfigButton,
    kToggleButton,
    kTunnelSettingsButton,
    kTunnelRegisterButton,
    kReloadButton,
    kHideButton,
    kQuitButton,
    kLogEdit,
};

struct HostApp {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND state_label = nullptr;
    HWND peer_label = nullptr;
    HWND stream_label = nullptr;
    HWND tunnel_label = nullptr;
    HWND path_label = nullptr;
    HWND token_edit = nullptr;
    HWND fingerprint_edit = nullptr;
    HWND input_check = nullptr;
    HWND approve_check = nullptr;
    HWND onetime_check = nullptr;
    HWND autostart_check = nullptr;
    HWND toggle_button = nullptr;
    HWND log_edit = nullptr;
    HFONT font = nullptr;
    HFONT title_font = nullptr;
    int dpi = 96;
    bool tray_added = false;
    bool quitting = false;
    std::wstring tray_tip;

    std::string config_path;
    HostSettings settings;
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
};

HostApp* g_app = nullptr;

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

bool server_approve(const ClientRequest& request) {
    const std::wstring peer =
        request.peer_address.empty() ? std::wstring(L"未知地址") : widen(request.peer_address);
    std::wstring text = L"来自 " + peer + L" 的查看器请求连接";
    if (request.display != kDisplayAuto) {
        text += L"（显示器 " + std::to_wstring(request.display) + L"）";
    }
    text += L"。\n\n允许这次连接吗？";
    const bool allowed = MessageBoxW(g_app->window, text.c_str(), L"chast 连接请求",
                                     MB_YESNO | MB_ICONQUESTION | MB_TOPMOST |
                                         MB_SETFOREGROUND) == IDYES;
    post_log(allowed ? L"已允许来自 " + peer + L" 的连接"
                     : L"已拒绝来自 " + peer + L" 的连接");
    return allowed;
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
    g_app->server->stop();
    g_app->server.reset();
    append_log_line(L"共享已停止");
}

void start_server() {
    if (g_app->server) {
        return;
    }
    HostServerConfig config;
    config.settings = g_app->settings;
    config.serve_once = false;
    config.first_client_timeout_ms = 0;
    config.approve_client = g_app->settings.require_approval ? server_approve : nullptr;
    config.log = server_log;
    config.token_changed = server_token_changed;
    std::string error;
    g_app->server = HostServer::create(std::move(config), &error);
    if (!g_app->server) {
        append_log_line(L"启动失败：" + widen(error));
        const std::wstring message = L"无法启动共享：\n" + widen(error);
        MessageBoxW(g_app->window, message.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
        return;
    }
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
        case HostState::Listening:
            return L"等待查看器（端口 " + std::to_wstring(g_app->settings.port) + L"）";
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

// Reads the live state and updates every label. Called on a timer, so it only
// writes text that changed: rewriting the token edit would clear a selection
// the operator is about to copy.
void refresh_ui() {
    HostStatus status;
    if (g_app->server) {
        status = g_app->server->status();
    }
    const bool running = g_app->server && g_app->server->running();

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
        if (status.stats.input_events > 0) {
            stream += L" · 输入 " + std::to_wstring(status.stats.input_events);
        }
        set_text(g_app->stream_label, stream);
    } else {
        set_text(g_app->stream_label, L"画面：—");
    }
    set_text(g_app->tunnel_label, tunnel_text());
    set_text(g_app->path_label, path_text(status));

    std::string token = g_app->settings.access_token;
    if (!status.token.empty()) {
        token = status.token;
    }
    if (!token.empty() && token != g_app->settings.access_token) {
        g_app->settings.access_token = token;
        save_settings();
    }
    set_text(g_app->token_edit, widen(token));
    set_text(g_app->fingerprint_edit, widen(format_fingerprint(status.fingerprint)));

    set_check(g_app->input_check, g_app->settings.allow_input);
    set_check(g_app->approve_check, g_app->settings.require_approval);
    set_check(g_app->onetime_check, g_app->settings.token_mode == HostTokenMode::OneTime);
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
    add_control(L"STATIC", L"远程桌面主机 · 访问令牌与连接设置", SS_LEFT, 0, 24, 52, 500, 20,
                0, nullptr);

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

    add_control(L"STATIC", L"访问令牌", SS_LEFT, 0, 24, 214, 200, 20, 0, nullptr);
    g_app->token_edit = add_control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL,
                                    WS_EX_CLIENTEDGE, 24, 236, 340, 26, kTokenEdit, nullptr);
    add_control(L"BUTTON", L"复制令牌", BS_PUSHBUTTON, 0, 376, 235, 88, 28, kCopyButton,
                nullptr);
    add_control(L"BUTTON", L"更换令牌", BS_PUSHBUTTON, 0, 476, 235, 120, 28,
                kRegenerateButton, nullptr);

    add_control(L"STATIC", L"身份指纹（SHA-256）", SS_LEFT, 0, 24, 272, 300, 20, 0, nullptr);
    g_app->fingerprint_edit =
        add_control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 24, 294, 572,
                    26, kFingerprintEdit, nullptr);

    g_app->input_check = add_control(L"BUTTON", L"接受远程输入", BS_AUTOCHECKBOX, 0, 24, 334,
                                     160, 24, kInputCheck, nullptr);
    g_app->approve_check = add_control(L"BUTTON", L"连接前需要确认", BS_AUTOCHECKBOX, 0, 200, 334,
                                       170, 24, kApproveCheck, nullptr);
    g_app->onetime_check = add_control(L"BUTTON", L"一次性令牌", BS_AUTOCHECKBOX, 0, 386, 334,
                                       150, 24, kOneTimeCheck, nullptr);
    g_app->autostart_check = add_control(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX, 0, 24, 368,
                                         160, 24, kAutostartCheck, nullptr);

    add_control(L"BUTTON", L"允许防火墙访问…", BS_PUSHBUTTON, 0, 200, 364, 170, 30,
                kFirewallButton, nullptr);
    add_control(L"BUTTON", L"打开设置文件", BS_PUSHBUTTON, 0, 386, 364, 150, 30,
                kOpenConfigButton, nullptr);

    g_app->toggle_button = add_control(L"BUTTON", L"开始共享", BS_PUSHBUTTON, 0, 24, 406, 120,
                                       32, kToggleButton, nullptr);
    add_control(L"BUTTON", L"连接服务设置…", BS_PUSHBUTTON, 0, 156, 406, 160, 32,
                kTunnelSettingsButton, nullptr);
    add_control(L"BUTTON", L"重新注册", BS_PUSHBUTTON, 0, 328, 406, 120, 32,
                kTunnelRegisterButton, nullptr);
    add_control(L"BUTTON", L"重新加载设置", BS_PUSHBUTTON, 0, 460, 406, 112, 32, kReloadButton,
                nullptr);
    add_control(L"BUTTON", L"隐藏到托盘", BS_PUSHBUTTON, 0, 24, 444, 120, 32, kHideButton,
                nullptr);
    add_control(L"BUTTON", L"退出", BS_PUSHBUTTON, 0, 156, 444, 100, 32, kQuitButton, nullptr);

    add_control(L"STATIC", L"日志", SS_LEFT, 0, 24, 492, 200, 20, 0, nullptr);
    g_app->log_edit =
        add_control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                    WS_EX_CLIENTEDGE, 24, 514, 572, 150, kLogEdit, nullptr);

    set_check(g_app->input_check, g_app->settings.allow_input);
    set_check(g_app->approve_check, g_app->settings.require_approval);
    set_check(g_app->onetime_check, g_app->settings.token_mode == HostTokenMode::OneTime);
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
    AppendMenuW(menu, MF_STRING, kCopyButton, L"复制访问令牌");
    AppendMenuW(menu, MF_STRING, kRegenerateButton, L"更换访问令牌");
    AppendMenuW(menu, MF_STRING | (g_app->settings.allow_input ? MF_CHECKED : 0), kInputCheck,
                L"接受远程输入");
    AppendMenuW(menu, MF_STRING | (g_app->settings.require_approval ? MF_CHECKED : 0),
                kApproveCheck, L"连接前需要确认");
    AppendMenuW(menu,
                MF_STRING |
                    (g_app->settings.token_mode == HostTokenMode::OneTime ? MF_CHECKED : 0),
                kOneTimeCheck, L"一次性令牌");
    AppendMenuW(menu, MF_STRING | (g_app->settings.autostart ? MF_CHECKED : 0), kAutostartCheck,
                L"开机自动启动");
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

void handle_command(int id) {
    switch (id) {
        case kMenuShow:
            show_window();
            return;
        case kCopyButton:
            chast::host::copy_text(g_app->window, widen(g_app->settings.access_token));
            append_log_line(L"访问令牌已复制到剪贴板");
            break;
        case kRegenerateButton:
            g_app->settings.access_token = generate_access_token();
            save_settings();
            restart_server();
            append_log_line(L"已生成新的访问令牌");
            break;
        case kInputCheck:
            g_app->settings.allow_input = !g_app->settings.allow_input;
            save_settings();
            restart_server();
            append_log_line(g_app->settings.allow_input ? L"远程输入已开启"
                                                        : L"远程输入已关闭");
            break;
        case kApproveCheck:
            g_app->settings.require_approval = !g_app->settings.require_approval;
            save_settings();
            restart_server();
            append_log_line(g_app->settings.require_approval ? L"连接前需要确认"
                                                             : L"连接不再需要确认");
            break;
        case kOneTimeCheck:
            g_app->settings.token_mode = g_app->settings.token_mode == HostTokenMode::OneTime
                                             ? HostTokenMode::Reusable
                                             : HostTokenMode::OneTime;
            save_settings();
            restart_server();
            append_log_line(g_app->settings.token_mode == HostTokenMode::OneTime
                                ? L"令牌将在使用后更换"
                                : L"令牌可重复使用");
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
            g_app->tunnel.stop();
            if (g_app->server) {
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
                        L"--firewall <端口> [隧道端口]  添加防火墙规则（需要管理员）",
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
    RECT rect{0, 0, px(620), px(690)};
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
