#ifndef RUNNER_SESSION_TAB_HOST_H_
#define RUNNER_SESSION_TAB_HOST_H_

#include <windows.h>
#include <commctrl.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "flutter_window.h"
#include "session_tab_model.h"
#include "session_tab_visibility.h"

class SessionTabHostWindow : public Win32Window {
 public:
  using OpenSessionWindow = FlutterWindow::OpenSessionWindow;

  explicit SessionTabHostWindow(
      std::function<void()> show_home,
      std::shared_ptr<SessionTabVisibility> visibility);

  bool CreateSession(const std::string& request, const std::string& label,
                     const std::string& device_hint,
                     OpenSessionWindow open_session,
                     FlutterWindow::OpenDisplayWindow open_display);
  bool OwnsSession(const void* session) const;
  size_t session_count() const { return entries_.size(); }
  void PruneClosed();
  std::shared_ptr<SessionTabVisibility> visibility() const {
    return visibility_;
  }

 protected:
  bool OnCreate() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

 private:
  struct Entry {
    SessionTabModel::Id id;
    // Capped base label drives collision detection. Keep the full, sanitized
    // caption separately for accessibility and the tab tooltip.
    std::wstring label;
    std::wstring full_label;
    std::wstring caption;
    std::wstring device_hint;
    std::unique_ptr<FlutterWindow> window;
  };

  void Layout();
  void RebuildTabs();
  void SyncVisibility();
  void SelectIndex(int index);
  void CloseTab(SessionTabModel::Id id);
  void CloseAll();
  Entry* Find(SessionTabModel::Id id);

  std::function<void()> show_home_;
  std::shared_ptr<SessionTabVisibility> visibility_;
  SessionTabModel model_;
  std::vector<Entry> entries_;
  HWND tabs_ = nullptr;
  HWND new_button_ = nullptr;
  HWND tooltip_ = nullptr;
  HWND tab_tooltip_ = nullptr;
  bool updating_tabs_ = false;
  bool closing_container_ = false;
  bool final_close_posted_ = false;
};

#endif  // RUNNER_SESSION_TAB_HOST_H_
