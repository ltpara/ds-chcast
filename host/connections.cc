#define NOMINMAX
#define UNICODE
#define _UNICODE
#include "connections.h"
#include "chast/host_server.h"
#include "i18n.h"
#include "platform.h"
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <vector>

namespace chast::host {
namespace {
constexpr wchar_t kClass[] = L"ChastConnectionsWindow";
enum { kList = 6100, kApply, kDisconnect, kFile, kSend, kInput, kPermission = 6200 };
struct Permission { PermissionMask bit; const wchar_t* label; };
const std::array<Permission, 15> permissions{{
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
std::wstring text_of(HWND control) {
    std::wstring text(GetWindowTextLengthW(control) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    return text;
}
}

struct ConnectionsWindow::Impl {
    HWND owner, window = nullptr, list = nullptr, details = nullptr, history = nullptr, input = nullptr;
    HINSTANCE instance;
    HFONT font;
    int dpi;
    ConnectionActions actions;
    std::array<HWND, permissions.size()> checks{};
    struct Conversation {
        HostViewer viewer;
        bool live = false, unread = false;
        std::deque<std::wstring> lines;
        std::wstring draft;
    };
    std::map<uint64_t, Conversation> conversations;
    std::vector<uint64_t> rows;
    std::vector<std::wstring> labels;
    uint64_t selected = 0;
    bool dirty_permissions = false;

    int px(int value) const { return MulDiv(value, dpi, 96); }
    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
             int id = 0, DWORD extended = 0) {
        HWND control = CreateWindowExW(extended, cls, text, WS_CHILD | WS_VISIBLE | style,
            px(x), px(y), px(w), px(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    }
    void append(uint64_t id, const std::wstring& text) {
        if (!id) return;
        auto& conversation = conversations[id];
        SYSTEMTIME now{}; GetLocalTime(&now);
        wchar_t stamp[20]{};
        swprintf_s(stamp, L"[%02u:%02u] ", now.wHour, now.wMinute);
        conversation.lines.push_back(std::wstring(stamp) + text);
        if (conversation.lines.size() > 300) conversation.lines.pop_front();
        if (selected == id) render_history();
        else conversation.unread = true;
    }
    void render_history() {
        if (!history) return;
        std::wstring text;
        if (auto found = conversations.find(selected); found != conversations.end()) {
            for (const auto& line : found->second.lines) text += line + L"\r\n";
            found->second.unread = false;
        }
        SetWindowTextW(history, text.c_str());
        SendMessageW(history, EM_SETSEL, text.size(), text.size());
        SendMessageW(history, EM_SCROLLCARET, 0, 0);
    }
    void select(uint64_t id) {
        if (selected && input) conversations[selected].draft = text_of(input);
        selected = id;
        dirty_permissions = false;
        SetWindowTextW(input, id ? conversations[id].draft.c_str() : L"");
        render_history();
        refresh();
    }
    void refresh() {
        const auto active = actions.viewers();
        for (auto& [id, conversation] : conversations) {
            if (conversation.live && std::none_of(active.begin(), active.end(),
                    [id](const auto& viewer) { return viewer.id == id; }))
                append(id, tr(L"—— 查看者已断开 ——"));
            conversation.live = false;
        }
        for (const auto& viewer : active) {
            auto& conversation = conversations[viewer.id];
            conversation.viewer = viewer;
            conversation.live = true;
        }
        // Bound disconnected history while retaining the operator's selection.
        while (conversations.size() > 32) {
            auto oldest = std::find_if(conversations.begin(), conversations.end(), [&](const auto& item) {
                return !item.second.live && item.first != selected;
            });
            if (oldest == conversations.end()) break;
            conversations.erase(oldest);
        }
        if (!window) return;
        if (!selected && active.size() == 1) { select(active.front().id); return; }
        std::vector<uint64_t> next_rows;
        std::vector<std::wstring> next_labels;
        for (const auto& [id, conversation] : conversations) {
            next_rows.push_back(id);
            next_labels.push_back((conversation.unread ? L"● " : L"") + std::wstring(L"#") +
                std::to_wstring(id) + L" · " + mode_name(conversation.viewer.mode) + L" · " +
                (conversation.live ? widen(conversation.viewer.peer_address) : tr(L"已断开")));
        }
        if (labels != next_labels) {
            SendMessageW(list, WM_SETREDRAW, FALSE, 0);
            SendMessageW(list, LB_RESETCONTENT, 0, 0);
            for (const auto& label : next_labels) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            rows = std::move(next_rows); labels = std::move(next_labels);
            const auto current = std::find(rows.begin(), rows.end(), selected);
            if (current != rows.end()) SendMessageW(list, LB_SETCURSEL, current - rows.begin(), 0);
            SendMessageW(list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(list, nullptr, TRUE);
        }
        const auto found = conversations.find(selected);
        const auto* conversation = found == conversations.end() ? nullptr : &found->second;
        const bool live = conversation && conversation->live && !conversation->viewer.disconnecting;
        std::wstring description = tr(L"请先选择要管理的连接");
        if (conversation) {
            description = L"#" + std::to_wstring(selected) + L" · " + widen(conversation->viewer.peer_address) +
                L" · " + mode_name(conversation->viewer.mode) + L"\r\n" +
                (conversation->viewer.disconnecting ? tr(L"正在断开…") : conversation->live ?
                (conversation->viewer.controls_input ? tr(L"拥有键鼠控制权") : tr(L"已连接")) : tr(L"已断开"));
        }
        SetWindowTextW(details, description.c_str());
        for (size_t i = 0; i < permissions.size(); ++i) {
            if (!dirty_permissions) SendMessageW(checks[i], BM_SETCHECK,
                conversation && (conversation->viewer.permissions & permissions[i].bit) ? BST_CHECKED : BST_UNCHECKED, 0);
            EnableWindow(checks[i], live && (conversation->viewer.approved_permissions & permissions[i].bit));
        }
        for (auto id : {kApply, kDisconnect, kSend}) EnableWindow(GetDlgItem(window, id), live);
        EnableWindow(input, live);
        EnableWindow(GetDlgItem(window, kFile), live && (conversation->viewer.permissions & kPermissionFileTransfer));
    }
    void send() {
        const auto id = selected;
        const auto text = narrow(text_of(input));
        if (!valid_chat_text(text)) { append(id, tr(L"系统：消息不能为空，最长 4096 字节")); return; }
        if (!actions.chat(id, text)) {
            append(id, tr(L"系统：所选连接已结束或待发队列已满，消息未发送")); return;
        }
        append(id, tr(L"[主机] ") + widen(text));
        SetWindowTextW(input, L"");
        conversations[id].draft.clear();
    }
    void send_file() {
        const auto id = selected; // A modal dialog pumps events; preserve the target before opening.
        wchar_t path[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
        const auto filter = std::wstring(tr(L"所有文件")) + L'\0' + L"*.*" + L'\0';
        const auto title = tr(L"发送文件到连接") + std::wstring(L" #") + std::to_wstring(id);
        dialog.lpstrTitle = title.c_str(); dialog.lpstrFilter = filter.c_str();
        dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) return;
        std::string error;
        if (!actions.file(id, narrow(path), &error))
            append(id, tr(L"文件未发送：") + widen(error));
        else append(id, tr(L"文件已加入所选连接的发送队列：") + std::wstring(path));
    }
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wp, lp);
        if (message == WM_TIMER) { self->refresh(); return 0; }
        if (message == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
        if (message == WM_NCDESTROY) { self->window = nullptr; return DefWindowProcW(hwnd, message, wp, lp); }
        if (message == WM_COMMAND) {
            const int id = LOWORD(wp), event = HIWORD(wp);
            if (id == kList && event == LBN_SELCHANGE) {
                const auto row = SendMessageW(self->list, LB_GETCURSEL, 0, 0);
                if (row >= 0 && static_cast<size_t>(row) < self->rows.size()) self->select(self->rows[row]);
            } else if (id >= kPermission && id < kPermission + permissions.size() && event == BN_CLICKED) {
                self->dirty_permissions = true;
            } else if (id == kSend) self->send();
            else if (id == kFile) self->send_file();
            else if (id == kApply) {
                PermissionMask mask = 0;
                for (size_t i = 0; i < permissions.size(); ++i)
                    if (SendMessageW(self->checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED) mask |= permissions[i].bit;
                self->append(self->selected, self->actions.permissions(self->selected, mask)
                    ? tr(L"权限更新已提交；实际权限以连接状态为准") : tr(L"所选连接已结束，操作未执行"));
                self->dirty_permissions = false;
            } else if (id == kDisconnect) {
                if (!self->actions.disconnect(self->selected))
                    self->append(self->selected, tr(L"所选连接已结束，操作未执行"));
            }
            self->refresh();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    void show(bool activate) {
        if (!window) {
            WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.lpfnWndProc = proc;
            cls.hInstance = instance; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.lpszClassName = kClass;
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
            const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT size{0, 0, px(880), px(600)}; AdjustWindowRectEx(&size, style, FALSE, WS_EX_CONTROLPARENT);
            if (!CreateWindowExW(WS_EX_CONTROLPARENT, kClass, tr(L"连接管理与聊天"), style, CW_USEDEFAULT,
                CW_USEDEFAULT, size.right - size.left, size.bottom - size.top, owner, nullptr, instance, this)) return;
            add(L"STATIC", tr(L"选择连接后管理权限、聊天或发送文件"), 0, 16, 14, 850, 22);
            list = add(L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
                16, 44, 270, 538, kList, WS_EX_CLIENTEDGE);
            SendMessageW(list, LB_SETHORIZONTALEXTENT, px(520), 0);
            details = add(L"STATIC", L"", 0, 304, 44, 558, 44);
            for (size_t i = 0; i < permissions.size(); ++i) checks[i] = add(L"BUTTON", tr(permissions[i].label),
                BS_AUTOCHECKBOX | WS_TABSTOP, 304 + static_cast<int>(i % 3) * 186,
                96 + static_cast<int>(i / 3) * 25, 184, 24, kPermission + static_cast<int>(i));
            add(L"BUTTON", tr(L"应用本连接权限"), BS_PUSHBUTTON | WS_TABSTOP, 304, 228, 190, 30, kApply);
            add(L"BUTTON", tr(L"发送文件…"), BS_PUSHBUTTON | WS_TABSTOP, 504, 228, 172, 30, kFile);
            add(L"BUTTON", tr(L"断开本连接"), BS_PUSHBUTTON | WS_TABSTOP, 686, 228, 176, 30, kDisconnect);
            history = add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                304, 272, 558, 262, 0, WS_EX_CLIENTEDGE);
            input = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 304, 546, 454, 30, kInput, WS_EX_CLIENTEDGE);
            SendMessageW(input, EM_SETLIMITTEXT, 4096, 0);
            add(L"BUTTON", tr(L"发送"), BS_PUSHBUTTON | WS_TABSTOP, 768, 546, 94, 30, kSend);
            SetTimer(window, 1, 400, nullptr);
        }
        refresh();
        ShowWindow(window, activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
        if (activate) SetForegroundWindow(window);
        else { FLASHWINFO flash{sizeof(flash), window, FLASHW_TRAY, 2, 0}; FlashWindowEx(&flash); }
    }
};

ConnectionsWindow::ConnectionsWindow(HWND owner, HINSTANCE instance, HFONT font, int dpi,
        std::function<HostServer*()> server) : ConnectionsWindow(owner, instance, font, dpi,
        ConnectionActions{
            [server] { auto* host = server(); return host ? host->viewers() : std::vector<HostViewer>{}; },
            [server](uint64_t id, PermissionMask mask) { auto* host = server(); return host && host->set_permissions(id, mask); },
            [server](uint64_t id, const std::string& text) { auto* host = server(); return host && host->send_chat(id, text); },
            [server](uint64_t id, const std::string& path, std::string* error) { auto* host = server(); return host && host->start_file_download(id, path, error); },
            [server](uint64_t id) { auto* host = server(); return host && host->disconnect_viewer(id); }
        }) {}
ConnectionsWindow::ConnectionsWindow(HWND owner, HINSTANCE instance, HFONT font, int dpi,
        ConnectionActions actions) : impl_(std::make_unique<Impl>()) {
    impl_->owner = owner; impl_->instance = instance; impl_->font = font; impl_->dpi = dpi;
    impl_->actions = std::move(actions);
}
ConnectionsWindow::~ConnectionsWindow() { if (impl_->window) DestroyWindow(impl_->window); }
void ConnectionsWindow::show(bool activate) { impl_->show(activate); }
void ConnectionsWindow::receive_chat(uint64_t id, const std::string& text) {
    impl_->append(id, tr(L"[查看器] ") + widen(text));
    impl_->show(false);
}
void ConnectionsWindow::notify(uint64_t id, const std::string& text) {
    impl_->append(id, widen(text));
    impl_->refresh();
}
bool ConnectionsWindow::translate(MSG& message) {
    if (!impl_->window || !IsWindowVisible(impl_->window)) return false;
    if (message.hwnd == impl_->input && message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
        impl_->send(); return true;
    }
    return IsDialogMessageW(impl_->window, &message) != FALSE;
}
}
