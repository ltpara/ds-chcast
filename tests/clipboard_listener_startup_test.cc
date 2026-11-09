// Compile the real implementation with only CreateWindowExW delayed. The
// private window station keeps the operator's clipboard out of this test.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
HANDLE allow_create = nullptr;
std::atomic<bool> delay_create{true};
std::atomic<DWORD> listener_thread{0};
HWND WINAPI delayed_create(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style,
                          int x, int y, int w, int h, HWND parent, HMENU menu,
                          HINSTANCE instance, LPVOID param) {
    listener_thread = GetCurrentThreadId();
    if (delay_create.exchange(false)) WaitForSingleObject(allow_create, INFINITE);
    return CreateWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, instance, param);
}
}
#define CreateWindowExW delayed_create
#include "../src/clipboard/clipboard_listener.cc"
#undef CreateWindowExW

int main() {
    const auto station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    if (!station || !SetProcessWindowStation(station)) return 77;
    const auto desktop = CreateDesktopW(L"chast-clipboard-startup", nullptr, nullptr,
                                         0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 77;
    allow_create = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!allow_create) return 1;
    using chast::ClipboardListener;
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();
    const auto timed_out = ClipboardListener::Subscribe();
    const auto elapsed = Clock::now() - begin;
    if (timed_out != 0 || elapsed > std::chrono::seconds(3) || ClipboardListener::Running()) return 1;
    const auto retry_begin = Clock::now();
    if (ClipboardListener::Subscribe() != 0 ||
        Clock::now() - retry_begin > std::chrono::milliseconds(500)) return 1;
    SetEvent(allow_create);
    uint64_t recovered = 0;
    for (int i = 0; i < 200 && !recovered; ++i) {
        Sleep(10);
        recovered = ClipboardListener::Subscribe();
    }
    if (!recovered || !ClipboardListener::Running()) return 1;
    PostThreadMessageW(listener_thread, WM_QUIT, 0, 0);
    for (int i = 0; i < 200 && ClipboardListener::Running(); ++i) Sleep(10);
    if (ClipboardListener::Running()) return 1;
    uint64_t restarted = 0;
    for (int i = 0; i < 200 && !restarted; ++i) {
        Sleep(10);
        restarted = ClipboardListener::Subscribe();
    }
    if (!restarted || !ClipboardListener::Running()) return 1;
    ClipboardListener::Unsubscribe(recovered);
    ClipboardListener::Unsubscribe(restarted);
    CloseHandle(allow_create);
    std::puts("clipboard startup timeout, pending retry and lost-loop recovery passed");
    return ClipboardListener::Running() ? 1 : 0;
}
