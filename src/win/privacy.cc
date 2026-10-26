#include "chast/privacy.h"

#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace chast {

namespace {

// The overlay must be on the local user's desktop even when the host thread
// has followed the input desktop to Winlogon (a session that started at the
// lock screen). A fresh thread inherits its creator's desktop, so attach to
// Default explicitly before creating the window.
constexpr DWORD kDefaultDesktopAccess =
    DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
    DESKTOP_SWITCHDESKTOP | GENERIC_WRITE;

constexpr UINT kSetEnabledMessage = WM_APP + 0x51;
constexpr UINT_PTR kCoverTimer = 0x51;

std::string win32_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length != 0 ? std::wstring(buffer, length)
                                    : L"error " + std::to_wstring(code);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

}  // namespace

struct PrivacyOverlay::Impl {
    HANDLE ready = nullptr;
    std::thread thread;
    HWND window = nullptr;
    std::string create_error;
    std::atomic<bool> enabled{false};

    void run();
    void cover(HWND hwnd);
    void set_enabled(HWND hwnd, bool on, bool* ok);

    static LRESULT window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

bool privacy_mode_supported() {
    // RtlGetVersion is not subject to the compatibility shims that make
    // GetVersionEx lie without a manifest.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version = ntdll != nullptr
                                     ? reinterpret_cast<RtlGetVersionFn>(
                                           GetProcAddress(ntdll, "RtlGetVersion"))
                                     : nullptr;
    if (rtl_get_version == nullptr) {
        return false;
    }
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version(&info) != 0) {
        return false;
    }
    if (info.dwMajorVersion != 10) {
        return info.dwMajorVersion > 10;
    }
    return info.dwBuildNumber >= 19041;
}

void PrivacyOverlay::Impl::cover(HWND hwnd) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        return;
    }
    // HWND_TOPMOST on every tick keeps the mask above windows that are
    // created topmost later; SWP_NOACTIVATE keeps focus where it was.
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PrivacyOverlay::Impl::set_enabled(HWND hwnd, bool on, bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    if (!on) {
        ShowWindow(hwnd, SW_HIDE);
        enabled = false;
        return;
    }
    cover(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (!IsWindowVisible(hwnd)) {
        if (ok != nullptr) {
            *ok = false;
        }
        return;
    }
    enabled = true;
}

void PrivacyOverlay::Impl::run() {
    HDESK desktop = OpenDesktopW(L"Default", 0, FALSE, kDefaultDesktopAccess);
    if (desktop != nullptr) {
        SetThreadDesktop(desktop);
        CloseDesktop(desktop);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Impl::window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"chast_privacy_overlay";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        create_error = "RegisterClassExW: " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }

    // Layered, transparent to the mouse, never activated, absent from
    // Alt+Tab and owned by this process so the capture exclusion applies.
    window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
            WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (window == nullptr) {
        create_error = "CreateWindowExW: " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
        create_error = "SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE): " +
                       win32_error(GetLastError());
        DestroyWindow(window);
        window = nullptr;
        SetEvent(ready);
        return;
    }
    // A display can be added, removed or rotated while the mask is up; the
    // broadcast handles it and the timer covers changes that do not broadcast.
    SetTimer(window, kCoverTimer, 1000, nullptr);
    SetEvent(ready);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    window = nullptr;
}

LRESULT CALLBACK PrivacyOverlay::Impl::window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                                   LPARAM lparam) {
    auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case kSetEnabledMessage:
            if (impl != nullptr) {
                impl->set_enabled(hwnd, wparam != 0, reinterpret_cast<bool*>(lparam));
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
        case WM_TIMER:
            if (impl != nullptr && impl->enabled.load()) {
                impl->cover(hwnd);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kCoverTimer);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

PrivacyOverlay::PrivacyOverlay() : impl_(std::make_unique<Impl>()) {}

PrivacyOverlay::~PrivacyOverlay() {
    if (impl_->window != nullptr) {
        PostMessageW(impl_->window, WM_CLOSE, 0, 0);
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->ready != nullptr) {
        CloseHandle(impl_->ready);
    }
}

std::unique_ptr<PrivacyOverlay> PrivacyOverlay::create(std::string* error) {
    std::unique_ptr<PrivacyOverlay> overlay(new PrivacyOverlay());
    Impl& impl = *overlay->impl_;
    impl.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl.ready == nullptr) {
        *error = "CreateEvent: " + win32_error(GetLastError());
        return nullptr;
    }
    try {
        impl.thread = std::thread([&impl] { impl.run(); });
    } catch (const std::exception& e) {
        *error = std::string("privacy overlay thread: ") + e.what();
        return nullptr;
    }
    const DWORD waited = WaitForSingleObject(impl.ready, 5000);
    CloseHandle(impl.ready);
    impl.ready = nullptr;
    if (waited != WAIT_OBJECT_0 || impl.window == nullptr) {
        *error = !impl.create_error.empty()
                     ? impl.create_error
                     : "the privacy overlay window did not start";
        impl.thread.join();
        return nullptr;
    }
    return overlay;
}

bool PrivacyOverlay::enable(std::string* error) {
    bool ok = false;
    SendMessageW(impl_->window, kSetEnabledMessage, TRUE, reinterpret_cast<LPARAM>(&ok));
    if (!ok) {
        *error = "the privacy overlay could not be shown";
        return false;
    }
    return true;
}

void PrivacyOverlay::disable() {
    bool ok = false;
    SendMessageW(impl_->window, kSetEnabledMessage, FALSE, reinterpret_cast<LPARAM>(&ok));
}

bool PrivacyOverlay::enabled() const {
    return impl_->enabled.load();
}

}  // namespace chast
