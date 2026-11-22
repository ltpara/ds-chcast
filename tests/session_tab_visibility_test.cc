#include <windows.h>

#include <cstdio>

#include "session_tab_visibility.h"

namespace {

constexpr wchar_t kTestWindowClass[] = L"ChastSessionTabVisibilityTest";

LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
  return DefWindowProcW(window, message, wparam, lparam);
}

HWND create_window() {
  static bool registered = false;
  if (!registered) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = test_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kTestWindowClass;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return nullptr;
    registered = true;
  }
  return CreateWindowExW(0, kTestWindowClass, L"", WS_OVERLAPPEDWINDOW,
                         CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                         nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool check(bool condition, const char* description) {
  if (!condition) std::fprintf(stderr, "FAIL %s\n", description);
  return condition;
}

}  // namespace

int main() {
  int failures = 0;
  HWND first_display = create_window();
  HWND second_display = create_window();
  if (!check(first_display != nullptr && second_display != nullptr,
             "create test display windows")) {
    if (first_display) DestroyWindow(first_display);
    if (second_display) DestroyWindow(second_display);
    return 1;
  }

  int session_a = 0;
  int session_b = 0;
  SessionTabVisibility visibility;
  visibility.Register(&session_a, first_display);
  visibility.Register(&session_b, second_display);
  visibility.SetActive(&session_a);
  failures += !check(IsWindowVisible(first_display) &&
                         !IsWindowVisible(second_display) &&
                         visibility.OwnsActiveWindow(first_display) &&
                         !visibility.OwnsActiveWindow(second_display),
                     "only the active session display is visible and owned");

  visibility.SetActive(&session_b);
  failures += !check(!IsWindowVisible(first_display) &&
                         IsWindowVisible(second_display) &&
                         !visibility.OwnsActiveWindow(first_display) &&
                         visibility.OwnsActiveWindow(second_display),
                     "switching tabs changes display ownership");

  visibility.SetActive(&session_a);
  visibility.Register(&session_b, first_display);
  failures += !check(!IsWindowVisible(first_display) &&
                         !visibility.OwnsActiveWindow(first_display),
                     "reassigning a window to a background session hides it");
  visibility.SetActive(&session_b);
  failures += !check(IsWindowVisible(first_display) &&
                         visibility.OwnsActiveWindow(first_display),
                     "a reassigned HWND follows its new session");

  visibility.Remove(&session_a, first_display);
  failures += !check(visibility.OwnsActiveWindow(first_display),
                     "stale-owner removal cannot detach the new owner");
  visibility.SetActive(nullptr);
  failures += !check(!IsWindowVisible(first_display) &&
                         !IsWindowVisible(second_display),
                     "clearing active session hides every display window");

  visibility.SetActive(&session_b);
  DestroyWindow(first_display);
  visibility.SetActive(&session_b);
  failures += !check(!visibility.OwnsActiveWindow(first_display) &&
                         IsWindowVisible(second_display),
                     "destroyed display windows are pruned on session switch");
  DestroyWindow(second_display);
  return failures == 0 ? 0 : 1;
}
