#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

#include "window_focus.h"

namespace {

constexpr int kSkipCode = 77;

bool restore_desktop(HDESK original, HDESK isolated) {
    if (!SetThreadDesktop(original)) {
        std::fprintf(stderr, "FAIL restore original desktop (error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return false;
    }
    if (!CloseDesktop(isolated)) {
        std::fprintf(stderr, "FAIL close isolated desktop (error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return false;
    }
    return true;
}

}  // namespace

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* name) {
        if (condition) return;
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    };

    const DWORD ui_thread = GetCurrentThreadId();
    const HDESK original_desktop = GetThreadDesktop(ui_thread);
    if (original_desktop == nullptr) {
        std::fprintf(stderr, "SKIP cannot inspect the current desktop (error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return kSkipCode;
    }
    const std::wstring desktop_name =
        L"chast-window-focus-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    const HDESK isolated_desktop = CreateDesktopW(
        desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (isolated_desktop == nullptr) {
        std::fprintf(stderr, "SKIP cannot create an isolated test desktop (error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return kSkipCode;
    }
    if (!SetThreadDesktop(isolated_desktop)) {
        const DWORD error = GetLastError();
        if (!SetThreadDesktop(original_desktop)) {
            std::fprintf(stderr,
                         "FAIL restore original desktop after attach error (error %lu)\n",
                         static_cast<unsigned long>(GetLastError()));
            return 1;
        }
        CloseDesktop(isolated_desktop);
        std::fprintf(stderr, "SKIP cannot attach to isolated test desktop (error %lu)\n",
                     static_cast<unsigned long>(error));
        return kSkipCode;
    }

    const HWND host = CreateWindowExW(0, L"STATIC", L"focus test",
        WS_OVERLAPPEDWINDOW, 0, 0, 240, 160, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    const HWND other = CreateWindowExW(0, L"STATIC", L"other window",
        WS_OVERLAPPEDWINDOW, 260, 0, 240, 160, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (host == nullptr || other == nullptr) {
        std::fprintf(stderr, "FAIL create test windows\n");
        if (host) DestroyWindow(host);
        if (other) DestroyWindow(other);
        restore_desktop(original_desktop, isolated_desktop);
        return 1;
    }

    const HWND view = CreateWindowExW(0, L"STATIC", L"session view",
        WS_CHILD | WS_VISIBLE, 0, 40, 220, 100, host, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    const HWND sibling_view = CreateWindowExW(0, L"STATIC", L"other view",
        WS_CHILD | WS_VISIBLE, 0, 40, 220, 100, host, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    const HWND input = view ? CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 200, 30, view, nullptr,
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    if (input == nullptr || view == nullptr || sibling_view == nullptr) {
        std::fprintf(stderr, "FAIL create focused child\n");
        DestroyWindow(other);
        DestroyWindow(host);
        restore_desktop(original_desktop, isolated_desktop);
        return 1;
    }

    ShowWindow(host, SW_SHOW);
    SetActiveWindow(host);
    SetFocus(input);
    check(GetFocus() == input, "UI thread owns the child focus");
    check(chast_viewer::thread_focus_is_within(host, ui_thread),
          "focused child belongs to the active view");
    check(!chast_viewer::thread_focus_is_within(other, ui_thread),
          "focus in another view is rejected");
    check(chast_viewer::view_owns_active_focus(view, host, input),
          "active visible session view accepts descendant focus");
    check(!chast_viewer::view_owns_active_focus(view, other, input),
          "session view rejects focus under another foreground window");
    check(!chast_viewer::view_owns_active_focus(view, host, sibling_view),
          "session view rejects focus owned by a sibling view");
    ShowWindow(view, SW_HIDE);
    check(!chast_viewer::view_owns_active_focus(view, host, input),
          "hidden background session view rejects input focus");
    ShowWindow(view, SW_SHOW);

    HWND worker_focus = reinterpret_cast<HWND>(1);
    bool worker_sees_ui_focus = false;
    DWORD worker_desktop_error = ERROR_SUCCESS;
    std::thread worker([&] {
        if (!SetThreadDesktop(isolated_desktop)) {
            worker_desktop_error = GetLastError();
            return;
        }
        worker_focus = GetFocus();
        worker_sees_ui_focus =
            chast_viewer::thread_focus_is_within(host, ui_thread);
    });
    worker.join();
    check(worker_desktop_error == ERROR_SUCCESS,
          "worker thread attaches to the isolated test desktop");
    check(worker_focus == nullptr, "worker thread has no local focus window");
    check(worker_sees_ui_focus,
          "worker can query the UI thread focus without dropping input");

    DestroyWindow(other);
    DestroyWindow(host);
    if (!restore_desktop(original_desktop, isolated_desktop)) {
        ++failures;
    }
    return failures ? 1 : 0;
}
