// Real WM_CLIPBOARDUPDATE delivery on a private window station, so the test
// never touches the operator's clipboard.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

#include "chast/clipboard_listener.h"

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
int failures = 0;

void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}

bool wait_for(const std::function<bool()>& predicate,
              std::chrono::milliseconds timeout = 3s) {
    const auto until = Clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < until);
    return predicate();
}

bool write_text(const wchar_t* text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        CloseClipboard();
        return false;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    std::memcpy(target, text, bytes);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

int skip(const char* why) {
    std::printf("skipped: %s (error %lu)\n", why, GetLastError());
    return 77;
}
} // namespace

int main() {
    HWINSTA station = CreateWindowStationW(nullptr, 0, WINSTA_ALL_ACCESS, nullptr);
    if (station == nullptr) return skip("no private window station");
    if (!SetProcessWindowStation(station)) return skip("cannot switch window station");
    HDESK desktop = CreateDesktopW(L"chast-clipboard-listener", nullptr, nullptr, 0,
                                   GENERIC_ALL, nullptr);
    if (desktop == nullptr) return skip("no private desktop");
    if (!SetThreadDesktop(desktop)) return skip("cannot switch desktop");

    std::atomic<int> first{0};
    std::atomic<int> second{0};
    const uint64_t one = chast::ClipboardListener::Subscribe([&] { ++first; });
    if (one == 0) return skip("listener could not start");
    const uint64_t two = chast::ClipboardListener::Subscribe([&] { ++second; });
    check(two != 0, "two sessions share one listener");
    check(chast::ClipboardListener::Running(), "listener reports running");

    const uint64_t before = chast::ClipboardListener::Generation();
    check(write_text(L"chast clipboard listener"), "private clipboard accepts a write");
    check(wait_for([&] { return first.load() > 0 && second.load() > 0; }),
          "both subscribers hear the change");
    check(chast::ClipboardListener::Generation() > before, "generation advances");

    std::this_thread::sleep_for(100ms);
    chast::ClipboardListener::Unsubscribe(two);
    const int second_seen = second.load();
    const int first_before = first.load();
    check(write_text(L"chast clipboard listener 2"), "second private write");
    check(wait_for([&] { return first.load() > first_before; }),
          "remaining subscriber still hears changes");
    std::this_thread::sleep_for(100ms);
    check(second.load() == second_seen, "unsubscribed session is not called");

    chast::ClipboardListener::Unsubscribe(one);
    check(!chast::ClipboardListener::Running(), "last unsubscribe stops the listener");

    std::printf("clipboard listener checks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
