#include "session_tab_visibility.h"

#include <algorithm>

void SessionTabVisibility::Register(const void* session, HWND window) {
  if (session == nullptr || window == nullptr || !IsWindow(window)) return;
  const auto existing = std::find_if(windows_.begin(), windows_.end(),
      [window](const OwnedWindow& entry) { return entry.window == window; });
  if (existing == windows_.end()) {
    windows_.push_back({session, window});
  } else {
    existing->session = session;
  }
  ShowWindow(window, session == active_ ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void SessionTabVisibility::Remove(const void* session, HWND window) {
  windows_.erase(std::remove_if(windows_.begin(), windows_.end(),
      [session, window](const OwnedWindow& entry) {
        return entry.session == session && entry.window == window;
      }), windows_.end());
}

void SessionTabVisibility::SetActive(const void* session) {
  active_ = session;
  for (auto it = windows_.begin(); it != windows_.end();) {
    if (!IsWindow(it->window)) {
      it = windows_.erase(it);
      continue;
    }
    ShowWindow(it->window,
               it->session == active_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    ++it;
  }
}

bool SessionTabVisibility::OwnsActiveWindow(HWND window) const {
  return std::any_of(windows_.begin(), windows_.end(),
      [this, window](const OwnedWindow& entry) {
        return entry.window == window && entry.session == active_;
      });
}
