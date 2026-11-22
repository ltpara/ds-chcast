#ifndef RUNNER_FLUTTER_VIEW_INITIALIZATION_H_
#define RUNNER_FLUTTER_VIEW_INITIALIZATION_H_

#include <windows.h>
#include <commctrl.h>
#include <cwchar>

// Flutter 3.44 creates FLUTTERVIEW before assigning its binding delegate.
// Accessibility clients can synchronously query that HWND during CreateWindow,
// when Flutter's WM_GETOBJECT handler still dereferences a null delegate.
// Guard only newly created views on this thread until the controller returns.
class ScopedFlutterViewInitialization {
 public:
  ScopedFlutterViewInitialization() : previous_(current_) {
    current_ = this;
    hook_ = SetWindowsHookExW(WH_CBT, OnCreateWindow, nullptr,
                             GetCurrentThreadId());
  }

  ~ScopedFlutterViewInitialization() {
    if (hook_) UnhookWindowsHookEx(hook_);
    current_ = previous_;
    if (view_ && IsWindow(view_)) {
      RemoveWindowSubclass(view_, ViewProc, kSubclassId);
      if (queried_) {
        // Let accessibility clients retry against the now initialized view.
        NotifyWinEvent(EVENT_OBJECT_CREATE, view_, OBJID_CLIENT, CHILDID_SELF);
      }
    }
  }

  ScopedFlutterViewInitialization(const ScopedFlutterViewInitialization&) = delete;
  ScopedFlutterViewInitialization& operator=(const ScopedFlutterViewInitialization&) = delete;
  bool ready() const { return hook_ != nullptr; }

 private:
  static LRESULT CALLBACK OnCreateWindow(int code, WPARAM wparam, LPARAM lparam) {
    auto* guard = current_;
    if (code == HCBT_CREATEWND && guard && !guard->view_) {
      const HWND window = reinterpret_cast<HWND>(wparam);
      wchar_t name[64]{};
      if (GetClassNameW(window, name, 64) && wcscmp(name, L"FLUTTERVIEW") == 0 &&
          SetWindowSubclass(window, ViewProc, kSubclassId,
                            reinterpret_cast<DWORD_PTR>(guard))) {
        guard->view_ = window;
      }
    }
    return CallNextHookEx(guard ? guard->hook_ : nullptr, code, wparam, lparam);
  }

  static LRESULT CALLBACK ViewProc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto* guard = reinterpret_cast<ScopedFlutterViewInitialization*>(data);
    if (message == WM_GETOBJECT) {
      guard->queried_ = true;
      return 0;
    }
    if (message == WM_NCDESTROY) {
      guard->view_ = nullptr;
      RemoveWindowSubclass(window, ViewProc, kSubclassId);
    }
    return DefSubclassProc(window, message, wparam, lparam);
  }

  static constexpr UINT_PTR kSubclassId = 0x43484149;
  inline static thread_local ScopedFlutterViewInitialization* current_ = nullptr;
  ScopedFlutterViewInitialization* previous_;
  HHOOK hook_ = nullptr;
  HWND view_ = nullptr;
  bool queried_ = false;
};

#endif  // RUNNER_FLUTTER_VIEW_INITIALIZATION_H_
