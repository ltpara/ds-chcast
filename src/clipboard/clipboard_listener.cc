#include "chast/clipboard_listener.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace chast {
namespace {
constexpr wchar_t kWindowClass[] = L"ChastClipboardListener";

struct Run {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Run() {
        if (ready) CloseHandle(ready);
        if (cancel) CloseHandle(cancel);
        if (done) CloseHandle(done);
    }
};

struct State {
    std::mutex mutex;
    std::map<uint64_t, std::function<void()>> subscribers;
    uint64_t next_id = 1;
    std::thread thread;
    std::shared_ptr<Run> run;
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

void ThreadMain(std::shared_ptr<Run> run) {
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
        listening = listening && WaitForSingleObject(run->cancel, 0) == WAIT_TIMEOUT;
        state().running = listening;
        running.store(listening, std::memory_order_release);
    }
    SetEvent(run->ready);
    while (listening) {
        const DWORD result = MsgWaitForMultipleObjects(
            1, &run->cancel, FALSE, INFINITE, QS_ALLINPUT);
        if (result != WAIT_OBJECT_0 + 1) break;
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT ||
                WaitForSingleObject(run->cancel, 0) != WAIT_TIMEOUT) {
                listening = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (window != nullptr && IsWindow(window)) DestroyWindow(window);
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().running = false;
        running.store(false, std::memory_order_release);
    }
    SetEvent(run->done);
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
            // A timed-out startup still owns its handles and thread. Do not
            // wait for it here or spawn more workers while Win32 is stalled.
            if (WaitForSingleObject(shared.run->done, 0) != WAIT_OBJECT_0) return 0;
            stale = std::move(shared.thread);
        }
    }
    if (stale.joinable()) stale.join();
    auto run = std::make_shared<Run>();
    if (!run->ready || !run->cancel || !run->done) return 0;
    try {
        shared.thread = std::thread(ThreadMain, run);
        shared.run = run;
    } catch (const std::system_error&) {
        return 0;
    }
    const DWORD ready = WaitForSingleObject(run->ready, 2000);
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (ready != WAIT_OBJECT_0 || !shared.running) {
            SetEvent(run->cancel);
            shared.running = false;
            running.store(false, std::memory_order_release);
        } else {
            id = shared.next_id++;
            shared.subscribers.emplace(id, std::move(changed));
        }
    }
    return id;
}

void ClipboardListener::Unsubscribe(uint64_t subscription) {
    if (subscription == 0) return;
    std::lock_guard<std::mutex> starting(start_mutex);
    State& shared = state();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.subscribers.erase(subscription);
        if (!shared.subscribers.empty() || !shared.thread.joinable()) return;
        shared.running = false;
        running.store(false, std::memory_order_release);
        SetEvent(shared.run->cancel);
        // A callback may unsubscribe itself. The next Subscribe reaps this
        // thread after it exits; joining oneself is never valid.
        if (shared.thread.get_id() == std::this_thread::get_id()) return;
    }
    // Cancellation may arrive while Win32 is still creating the window, or
    // while a callback is running. Leave the run owned by State on timeout;
    // later Subscribe calls reap it once done, without creating extra workers.
    if (WaitForSingleObject(shared.run->done, 2000) != WAIT_OBJECT_0) return;
    thread = std::move(shared.thread);
    if (thread.joinable()) thread.join();
    running.store(false, std::memory_order_release);
}

} // namespace chast
