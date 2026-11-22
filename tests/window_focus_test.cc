#include <windows.h>

#include <cstdio>
#include <thread>

#include "window_focus.h"

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* name) {
        if (condition) return;
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    };

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
        return 1;
    }

    ShowWindow(host, SW_SHOW);
    SetActiveWindow(host);
    SetFocus(input);
    const DWORD ui_thread = GetCurrentThreadId();
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
    std::thread worker([&] {
        worker_focus = GetFocus();
        worker_sees_ui_focus =
            chast_viewer::thread_focus_is_within(host, ui_thread);
    });
    worker.join();
    check(worker_focus == nullptr, "worker thread has no local focus window");
    check(worker_sees_ui_focus,
          "worker can query the UI thread focus without dropping input");

    DestroyWindow(other);
    DestroyWindow(host);
    return failures ? 1 : 0;
}
