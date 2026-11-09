#include "chast/clipboard_listener.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace chast {
namespace {
constexpr wchar_t kWindowClass[] = L"ChastClipboardListener";

struct State {
    std::mutex mutex;
    std::map<uint64_t, std::function<void()>> subscribers;
    uint64_t next_id = 1;
    std::thread thread;
    HWND window = nullptr;
    bool running = false;
};

State& state() {
    // Leaked on purpose: the listener thread must not race static teardown.
    static State* value = new State();
    return *value;
}

std::mutex start_mutex;
std::atomic<uint64_t> generation{0};
std::atomic<bool> running{false};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLIPBOARDUPDATE) {
        generation.fetch_add(1, std::memory_order_acq_rel);
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            callbacks.reserve(state().subscribers.size());
            for (const auto& entry : state().subscribers) {
                if (entry.second) callbacks.push_back(entry.second);
            }
        }
        for (const auto& callback : callbacks) callback();
        return 0;
    }
    if (message == WM_DESTROY) {
        RemoveClipboardFormatListener(window);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterWindowClass() {
    WNDCLASSEXW info{};
    info.cbSize = sizeof(info);
    info.lpfnWndProc = WindowProc;
    info.hInstance = GetModuleHandleW(nullptr);
    info.lpszClassName = kWindowClass;
    if (RegisterClassExW(&info)) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void ThreadMain(HANDLE ready) {
    HWND window = nullptr;
    bool listening = false;
    if (RegisterWindowClass()) {
        window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                 nullptr, GetModuleHandleW(nullptr), nullptr);
        if (window != nullptr) {
            listening = AddClipboardFormatListener(window) != FALSE;
            if (!listening) {
                DestroyWindow(window);
                window = nullptr;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().window = window;
        state().running = listening;
    }
    running.store(listening, std::memory_order_release);
    SetEvent(ready);
    if (!listening) return;
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    running.store(false, std::memory_order_release);
}
} // namespace

uint64_t ClipboardListener::Generation() {
    return generation.load(std::memory_order_acquire);
}

bool ClipboardListener::Running() {
    return running.load(std::memory_order_acquire);
}

uint64_t ClipboardListener::Subscribe(std::function<void()> changed) {
    std::lock_guard<std::mutex> starting(start_mutex);
    State& shared = state();
    std::thread stale;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.thread.joinable()) {
            if (shared.running) {
                const uint64_t id = shared.next_id++;
                shared.subscribers.emplace(id, std::move(changed));
                return id;
            }
            // A listener that lost its message loop is replaced, not reused.
            stale = std::move(shared.thread);
        }
    }
    if (stale.joinable()) stale.join();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready == nullptr) return 0;
    std::thread thread(ThreadMain, ready);
    WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
    std::thread failed;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.thread = std::move(thread);
        if (!shared.running) {
            failed = std::move(shared.thread);
        } else {
            id = shared.next_id++;
            shared.subscribers.emplace(id, std::move(changed));
        }
    }
    if (failed.joinable()) {
        failed.join();
        return 0;
    }
    return id;
}

void ClipboardListener::Unsubscribe(uint64_t subscription) {
    if (subscription == 0) return;
    std::lock_guard<std::mutex> starting(start_mutex);
    State& shared = state();
    HWND window = nullptr;
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.subscribers.erase(subscription);
        if (!shared.subscribers.empty() || !shared.thread.joinable()) return;
        window = shared.window;
        shared.window = nullptr;
        shared.running = false;
        thread = std::move(shared.thread);
    }
    if (window != nullptr) PostMessageW(window, WM_CLOSE, 0, 0);
    if (thread.joinable()) thread.join();
    running.store(false, std::memory_order_release);
}

} // namespace chast
