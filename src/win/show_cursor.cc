#include "chast/show_cursor.h"

#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>

namespace chast {

namespace {

// The overlay must sit on the interactive desktop even when the host thread
// has followed the input desktop to Winlogon. A fresh thread inherits its
// creator's desktop, so attach to Default explicitly.
constexpr DWORD kDefaultDesktopAccess =
    DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
    DESKTOP_SWITCHDESKTOP | GENERIC_WRITE;

constexpr UINT kMoveMessage = WM_APP + 0x61;
constexpr UINT kHideMessage = WM_APP + 0x62;
constexpr BYTE kHintAlpha = 235;

struct MoveRequest {
    int32_t x;
    int32_t y;
    std::string* error;
};

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

struct CursorHintOverlay::Impl {
    HANDLE ready = nullptr;
    std::thread thread;
    HWND window = nullptr;
    std::string create_error;
    std::atomic<bool> visible{false};

    HCURSOR cursor = nullptr;
    int cursor_width = 0;
    int cursor_height = 0;
    POINT hotspot{};
    std::mutex state_mutex;

    void run();
    bool move(HWND hwnd, int32_t x, int32_t y, std::string* error);
    void hide(HWND hwnd);
    void paint(HWND hwnd) const;

    static LRESULT window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

void CursorHintOverlay::Impl::paint(HWND hwnd) const {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(hwnd, &paint);
    if (dc == nullptr) {
        return;
    }
    if (cursor != nullptr) {
        DrawIconEx(dc, 0, 0, cursor, cursor_width, cursor_height, 0, nullptr, DI_NORMAL);
    }
    EndPaint(hwnd, &paint);
}

bool CursorHintOverlay::Impl::move(HWND hwnd, int32_t x, int32_t y, std::string* error) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    constexpr int kMaxVirtualExtent = 32768;
    constexpr int kMaxVirtualOffset = 65536;
    if (width <= 0 || height <= 0 || width > kMaxVirtualExtent ||
        height > kMaxVirtualExtent || left < -kMaxVirtualOffset ||
        left > kMaxVirtualOffset || top < -kMaxVirtualOffset || top > kMaxVirtualOffset) {
        *error = "the virtual desktop metrics are unavailable";
        return false;
    }
    if (x < left) x = left;
    if (y < top) y = top;
    if (x > left + width - 1) x = left + width - 1;
    if (y > top + height - 1) y = top + height - 1;

    POINT hotspot{};
    int cursor_width = 0;
    int cursor_height = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        hotspot = this->hotspot;
        cursor_width = this->cursor_width;
        cursor_height = this->cursor_height;
    }
    if (!SetWindowPos(hwnd, HWND_TOPMOST, x - hotspot.x, y - hotspot.y, cursor_width,
                      cursor_height, SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        *error = "SetWindowPos: " + win32_error(GetLastError());
        return false;
    }
    visible = true;
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

void CursorHintOverlay::Impl::hide(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
    visible = false;
}

void CursorHintOverlay::Impl::run() {
    HDESK desktop = OpenDesktopW(L"Default", 0, FALSE, kDefaultDesktopAccess);
    if (desktop == nullptr) {
        create_error = "OpenDesktopW(Default): " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }
    const BOOL attached = SetThreadDesktop(desktop);
    const DWORD attach_error = attached ? ERROR_SUCCESS : GetLastError();
    CloseDesktop(desktop);
    if (attached == FALSE) {
        create_error = "SetThreadDesktop(Default): " + win32_error(attach_error);
        SetEvent(ready);
        return;
    }

    // LoadCursorW returns a shared system cursor, so no DestroyCursor is
    // needed. GetIconInfo on a cursor needs no copy and yields the hotspot the
    // drawn arrow must be offset by.
    HCURSOR shared_cursor = LoadCursorW(nullptr, IDC_ARROW);
    ICONINFO icon_info{};
    if (shared_cursor == nullptr || !GetIconInfo(shared_cursor, &icon_info)) {
        create_error = "LoadCursorW(IDC_ARROW): " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }
    if (icon_info.hbmMask != nullptr) DeleteObject(icon_info.hbmMask);
    if (icon_info.hbmColor != nullptr) DeleteObject(icon_info.hbmColor);
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        cursor = shared_cursor;
        cursor_width = GetSystemMetrics(SM_CXCURSOR);
        cursor_height = GetSystemMetrics(SM_CYCURSOR);
        if (cursor_width <= 0) cursor_width = 32;
        if (cursor_height <= 0) cursor_height = 32;
        hotspot = POINT{static_cast<LONG>(icon_info.xHotspot),
                        static_cast<LONG>(icon_info.yHotspot)};
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Impl::window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = shared_cursor;
    wc.lpszClassName = L"chast_cursor_hint";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        create_error = "RegisterClassExW: " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }

    window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
            WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (window == nullptr) {
        create_error = "CreateWindowExW: " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }
    if (!SetLayeredWindowAttributes(window, 0, kHintAlpha, LWA_ALPHA)) {
        create_error = "SetLayeredWindowAttributes: " + win32_error(GetLastError());
        DestroyWindow(window);
        window = nullptr;
        SetEvent(ready);
        return;
    }
    SetEvent(ready);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    window = nullptr;
}

LRESULT CALLBACK CursorHintOverlay::Impl::window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                                      LPARAM lparam) {
    auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case kMoveMessage:
            if (impl != nullptr) {
                auto* request = reinterpret_cast<MoveRequest*>(lparam);
                impl->move(hwnd, request->x, request->y, request->error);
            }
            return 0;
        case kHideMessage:
            if (impl != nullptr) {
                impl->hide(hwnd);
            }
            return 0;
        case WM_PAINT:
            if (impl != nullptr) {
                impl->paint(hwnd);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

CursorHintOverlay::CursorHintOverlay() : impl_(std::make_unique<Impl>()) {}

CursorHintOverlay::~CursorHintOverlay() {
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

std::unique_ptr<CursorHintOverlay> CursorHintOverlay::create(std::string* error) {
    std::unique_ptr<CursorHintOverlay> overlay(new CursorHintOverlay());
    Impl& impl = *overlay->impl_;
    impl.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl.ready == nullptr) {
        *error = "CreateEvent: " + win32_error(GetLastError());
        return nullptr;
    }
    try {
        impl.thread = std::thread([&impl] { impl.run(); });
    } catch (const std::exception& e) {
        *error = std::string("cursor hint thread: ") + e.what();
        return nullptr;
    }
    const DWORD waited = WaitForSingleObject(impl.ready, 5000);
    CloseHandle(impl.ready);
    impl.ready = nullptr;
    if (waited != WAIT_OBJECT_0 || impl.window == nullptr) {
        *error = !impl.create_error.empty() ? impl.create_error
                                            : "the cursor hint window did not start";
        impl.thread.join();
        return nullptr;
    }
    return overlay;
}

bool CursorHintOverlay::move(int32_t x, int32_t y, std::string* error) {
    MoveRequest request{x, y, error};
    SendMessageW(impl_->window, kMoveMessage, 0, reinterpret_cast<LPARAM>(&request));
    return error->empty();
}

void CursorHintOverlay::hide() {
    SendMessageW(impl_->window, kHideMessage, 0, 0);
}

bool CursorHintOverlay::visible() const {
    return impl_->visible.load();
}

}  // namespace chast
