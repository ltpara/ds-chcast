#include "session_tab_host.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "runner_lifecycle_trace.h"

namespace {

constexpr int kTabsId = 4101;
constexpr int kNewButtonId = 4102;
constexpr int kCloseButtonId = 4103;
constexpr int kTabBarHeight = 48;
constexpr size_t kMaxSessionTabs = 16;

void trace_tab_lifecycle(const char* event, HWND host, uint64_t id,
                         HWND child, size_t count, uint64_t selected) {
  char* testing = nullptr;
  size_t testing_size = 0;
  char* path = nullptr;
  size_t path_size = 0;
  const bool configured =
      _dupenv_s(&testing, &testing_size, "CHAST_NATIVE_TESTING") == 0 &&
      testing != nullptr && std::string(testing) == "1" &&
      _dupenv_s(&path, &path_size, "CHAST_NATIVE_TEST_TABS_TRACE") == 0 &&
      path != nullptr && *path != '\0';
  if (!configured) {
    std::free(testing);
    std::free(path);
    return;
  }
  std::ofstream output(path, std::ios::app);
  if (output) {
    output << event << '\t' << GetCurrentProcessId() << '\t'
           << GetCurrentThreadId() << '\t'
           << reinterpret_cast<uintptr_t>(host) << '\t' << id << '\t'
           << reinterpret_cast<uintptr_t>(child) << '\t' << count << '\t'
           << selected << '\t'
           << (host != nullptr && IsWindow(host) ? 1 : 0) << '\t'
           << (child != nullptr && IsWindow(child) ? 1 : 0) << '\t'
           << (child != nullptr && IsWindowVisible(child) ? 1 : 0) << '\n';
  }
  std::free(testing);
  std::free(path);
}

std::wstring to_wide(const std::string& value) {
  if (value.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), size) != size)
    return {};
  return result;
}

}  // namespace

SessionTabHostWindow::SessionTabHostWindow(
    std::function<void()> show_home,
    std::shared_ptr<SessionTabVisibility> visibility)
    : show_home_(std::move(show_home)), visibility_(std::move(visibility)) {}

bool SessionTabHostWindow::OnCreate() {
  if (!Win32Window::OnCreate()) return false;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES};
  if (!InitCommonControlsEx(&controls)) return false;
  const HWND parent = GetHandle();
  tabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_SINGLELINE | TCS_FOCUSNEVER,
      8, 7, 700, 34, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabsId)),
      GetModuleHandleW(nullptr), nullptr);
  new_button_ = CreateWindowExW(0, L"BUTTON", L"新建连接",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0, 0, 96, 32, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNewButtonId)),
      GetModuleHandleW(nullptr), nullptr);
  close_button_ = CreateWindowExW(0, L"BUTTON", L"关闭标签",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      0, 0, 84, 32, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButtonId)),
      GetModuleHandleW(nullptr), nullptr);
  if (!tabs_ || !new_button_ || !close_button_) return false;
  Layout();
  return true;
}

bool SessionTabHostWindow::CreateSession(
    const std::string& request, const std::string& label,
    OpenSessionWindow open_session,
    FlutterWindow::OpenDisplayWindow open_display) {
  trace_tab_lifecycle("create-begin", GetHandle(), 0, nullptr,
                      entries_.size(), model_.selected().value_or(0));
  if (closing_container_ || GetHandle() == nullptr ||
      entries_.size() >= kMaxSessionTabs || request.empty())
    return false;

  flutter::DartProject project(L"data");
  project.set_dart_entrypoint_arguments({"--session-window", request});
  auto child = std::make_unique<FlutterWindow>(
      project, std::move(open_session), std::move(open_display),
      std::shared_ptr<chast_viewer::SharedViewerSession>{}, std::nullopt, true);
  const RECT client = GetClientArea();
  const RECT content{0, kTabBarHeight, client.right, client.bottom};
  if (!child->CreateChild(L"Chast Session", GetHandle(), content)) return false;
  child->SetQuitOnClose(false);

  const auto id = model_.Add();
  if (!id) return false;
  entries_.push_back({*id, TabLabel(label, entries_.size() + 1),
                      std::move(child)});
  trace_tab_lifecycle("create-done", GetHandle(), *id,
                      entries_.back().window->GetHandle(), entries_.size(),
                      model_.selected().value_or(0));
  RebuildTabs();
  SyncVisibility();
  Show();
  return true;
}

void SessionTabHostWindow::PruneClosed() {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->window->GetHandle() != nullptr) {
      ++it;
      continue;
    }
    trace_tab_lifecycle("prune-child", GetHandle(), it->id, nullptr,
                        entries_.size(), model_.selected().value_or(0));
    model_.BeginClose(it->id);
    model_.Remove(it->id);
    it = entries_.erase(it);
  }
  RebuildTabs();
  SyncVisibility();
  if (entries_.empty() && GetHandle() != nullptr && !final_close_posted_) {
    final_close_posted_ = true;
    trace_tab_lifecycle("post-empty-host-close", GetHandle(), 0, nullptr,
                        entries_.size(), model_.selected().value_or(0));
    PostMessageW(GetHandle(), WM_CLOSE, 0, 0);
  }
}

bool SessionTabHostWindow::OwnsSession(const void* session) const {
  return session != nullptr && std::any_of(entries_.begin(), entries_.end(),
      [session](const Entry& entry) {
        return entry.window->session_identity() == session;
      });
}

void SessionTabHostWindow::Layout() {
  const RECT area = GetClientArea();
  const int width = static_cast<int>((std::max)(0L, area.right - area.left));
  const int height = static_cast<int>((std::max)(0L, area.bottom - area.top));
  const int buttons = 96 + 84 + 24;
  if (tabs_) MoveWindow(tabs_, 8, 7, (std::max)(120, width - buttons), 34, TRUE);
  if (new_button_)
    MoveWindow(new_button_, width - 8 - 96 - 84 - 8, 8, 96, 32, TRUE);
  if (close_button_)
    MoveWindow(close_button_, width - 8 - 84, 8, 84, 32, TRUE);
  for (Entry& entry : entries_) {
    SetWindowPos(entry.window->GetHandle(), nullptr, 0, kTabBarHeight, width,
                 (std::max)(0, height - kTabBarHeight),
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void SessionTabHostWindow::RebuildTabs() {
  if (!tabs_) return;
  updating_tabs_ = true;
  TabCtrl_DeleteAllItems(tabs_);
  for (size_t i = 0; i < entries_.size(); ++i) {
    TCITEMW item{};
    item.mask = TCIF_TEXT | TCIF_PARAM;
    item.pszText = entries_[i].label.data();
    item.lParam = static_cast<LPARAM>(entries_[i].id);
    TabCtrl_InsertItem(tabs_, static_cast<int>(i), &item);
  }
  const auto selected = model_.selected();
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (selected == entries_[i].id) {
      TabCtrl_SetCurSel(tabs_, static_cast<int>(i));
      break;
    }
  }
  updating_tabs_ = false;
}

void SessionTabHostWindow::SyncVisibility() {
  const auto selected = model_.selected();
  const void* active_session = nullptr;
  for (Entry& entry : entries_) {
    const bool active = selected == entry.id &&
        std::none_of(model_.tabs().begin(), model_.tabs().end(),
            [&entry](const SessionTabModel::Tab& tab) {
              return tab.id == entry.id && tab.closing;
            });
    entry.window->SetTabVisible(active);
    if (active) active_session = entry.window->session_identity();
  }
  if (visibility_) visibility_->SetActive(active_session);
  if (selected) {
    if (Entry* entry = Find(*selected)) entry->window->FocusChildContent();
  }
}

void SessionTabHostWindow::SelectIndex(int index) {
  if (index < 0 || static_cast<size_t>(index) >= entries_.size()) return;
  if (!model_.Select(entries_[static_cast<size_t>(index)].id)) {
    RebuildTabs();
    return;
  }
  SyncVisibility();
}

void SessionTabHostWindow::CloseTab(SessionTabModel::Id id) {
  Entry* entry = Find(id);
  trace_tab_lifecycle("close-tab-request", GetHandle(), id,
                      entry ? entry->window->GetHandle() : nullptr,
                      entries_.size(), model_.selected().value_or(0));
  const auto selected_before = model_.selected();
  if (!entry || !model_.BeginClose(id)) return;
  entry->window->SetTabVisible(false);
  RebuildTabs();
  SyncVisibility();
  const HWND child = entry->window->GetHandle();
  trace_tab_lifecycle("close-tab-post", GetHandle(), id, child,
                      entries_.size(), model_.selected().value_or(0));
  if (!PostMessageW(child, WM_CLOSE, 0, 0)) {
    const DWORD error = GetLastError();
    trace_tab_lifecycle("close-tab-post-failed", GetHandle(), id, child,
                        entries_.size(), model_.selected().value_or(0));
    TraceRunnerLifecycle("close-tab-post-failed", child, WM_CLOSE, error);
    if (model_.RollbackFailedClose(id, selected_before,
                                   IsWindow(child) != FALSE)) {
      TraceRunnerLifecycle("close-tab-post-rollback", child, WM_CLOSE, error);
      RebuildTabs();
      SyncVisibility();
    }
  }
}

void SessionTabHostWindow::CloseAll() {
  trace_tab_lifecycle("close-all", GetHandle(), 0, nullptr,
                      entries_.size(), model_.selected().value_or(0));
  closing_container_ = true;
  for (Entry& entry : entries_) {
    if (model_.BeginClose(entry.id)) {
      entry.window->SetTabVisible(false);
      PostMessageW(entry.window->GetHandle(), WM_CLOSE, 0, 0);
    }
  }
  RebuildTabs();
  SyncVisibility();
}

SessionTabHostWindow::Entry* SessionTabHostWindow::Find(
    SessionTabModel::Id id) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
      [id](const Entry& entry) { return entry.id == id; });
  return it == entries_.end() ? nullptr : &*it;
}

std::wstring SessionTabHostWindow::TabLabel(const std::string& label,
                                            size_t index) {
  std::wstring result = to_wide(label);
  result.erase(std::remove_if(result.begin(), result.end(),
      [](wchar_t c) { return c < 0x20 || c == 0x7f; }), result.end());
  if (result.empty()) result = L"会话 " + std::to_wstring(index);
  constexpr size_t kMaxLabelLength = 32;
  if (result.size() > kMaxLabelLength) {
    result.resize(kMaxLabelLength - 1);
    if (!result.empty() && result.back() >= 0xd800 && result.back() <= 0xdbff)
      result.pop_back();
    result.push_back(L'…');
  }
  return result;
}

LRESULT SessionTabHostWindow::MessageHandler(HWND window, UINT const message,
    WPARAM const wparam, LPARAM const lparam) noexcept {
  if (message == WM_CLOSE || message == WM_DESTROY || message == WM_NCDESTROY) {
    trace_tab_lifecycle(message == WM_CLOSE ? "host-wm-close" :
                        message == WM_DESTROY ? "host-wm-destroy" :
                                                "host-wm-ncdestroy",
                        window, 0, nullptr, entries_.size(),
                        model_.selected().value_or(0));
  }
  if (message == WM_SIZE) {
    Layout();
  } else if (message == WM_NOTIFY && !updating_tabs_) {
    const auto* header = reinterpret_cast<const NMHDR*>(lparam);
    if (header && header->hwndFrom == tabs_ && header->code == TCN_SELCHANGING) {
      const auto selected = model_.selected();
      if (selected) {
        if (Entry* entry = Find(*selected)) entry->window->SetTabInputActive(false);
      }
    } else if (header && header->hwndFrom == tabs_ && header->code == TCN_SELCHANGE) {
      SelectIndex(TabCtrl_GetCurSel(tabs_));
      return 0;
    }
  } else if (message == WM_COMMAND) {
    const int id = LOWORD(wparam);
    if (id == kNewButtonId) {
      const auto selected = model_.selected();
      if (selected) {
        if (Entry* entry = Find(*selected)) entry->window->SetTabInputActive(false);
      }
      if (show_home_) show_home_();
      return 0;
    }
    if (id == kCloseButtonId) {
      const auto selected = model_.selected();
      if (selected) CloseTab(*selected);
      return 0;
    }
  } else if (message == WM_ACTIVATE) {
    if (LOWORD(wparam) == WA_INACTIVE) {
      const HWND next_window = reinterpret_cast<HWND>(lparam);
      if (!visibility_ || !visibility_->OwnsActiveWindow(next_window)) {
        if (visibility_) visibility_->SetActive(nullptr);
        const auto selected = model_.selected();
        if (selected) {
          if (Entry* entry = Find(*selected)) entry->window->SetTabInputActive(false);
        }
      }
    } else {
      SyncVisibility();
    }
  } else if (message == WM_CLOSE) {
    if (!entries_.empty()) {
      if (!closing_container_) CloseAll();
      return 0;
    }
    if (visibility_) visibility_->SetActive(nullptr);
  }
  return Win32Window::MessageHandler(window, message, wparam, lparam);
}
