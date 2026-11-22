#ifndef RUNNER_SESSION_TAB_VISIBILITY_H_
#define RUNNER_SESSION_TAB_VISIBILITY_H_

#include <windows.h>

#include <vector>

class SessionTabVisibility {
 public:
  void Register(const void* session, HWND window);
  void Remove(const void* session, HWND window);
  void SetActive(const void* session);
  bool OwnsActiveWindow(HWND window) const;

 private:
  struct OwnedWindow {
    const void* session;
    HWND window;
  };
  const void* active_ = nullptr;
  std::vector<OwnedWindow> windows_;
};

#endif  // RUNNER_SESSION_TAB_VISIBILITY_H_
