#include "chast/show_cursor.h"

#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>

#include <atomic>
#include <cmath>
#include <exception>
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

// The arrow is painted into a 32bpp DIB and handed to the window with
// UpdateLayeredWindow, so every pixel outside the shape is fully transparent.
// SetLayeredWindowAttributes with LWA_ALPHA cannot do that: it tints the whole
// window rectangle and leaves a black square behind the icon.
constexpr int kDesignSize = 32;
constexpr int kSamplesPerAxis = 4;
constexpr double kOutlineWidth = 1.4;

// Design-space arrow polygon, tip first, in the 32x32 cursor grid Windows
// uses at 100% scaling.
constexpr double kArrow[7][2] = {
    {1.5, 1.0},  {1.5, 23.0}, {7.0, 17.5}, {11.0, 27.5},
    {15.5, 25.5}, {11.5, 16.0}, {19.0, 16.0},
};

bool point_in_polygon(double x, double y) {
    constexpr int kVertices = 7;
    bool inside = false;
    for (int i = 0, j = kVertices - 1; i < kVertices; j = i++) {
        const double xi = kArrow[i][0];
        const double yi = kArrow[i][1];
        const double xj = kArrow[j][0];
        const double yj = kArrow[j][1];
        if (((yi > y) != (yj > y)) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
    }
    return inside;
}

double segment_distance(double x, double y, double x1, double y1, double x2, double y2) {
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double length_sq = dx * dx + dy * dy;
    double t = length_sq > 0.0 ? ((x - x1) * dx + (y - y1) * dy) / length_sq : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const double offset_x = x1 + t * dx - x;
    const double offset_y = y1 + t * dy - y;
    return std::sqrt(offset_x * offset_x + offset_y * offset_y);
}

double outline_distance(double x, double y) {
    constexpr int kVertices = 7;
    double nearest = segment_distance(x, y, kArrow[0][0], kArrow[0][1], kArrow[1][0],
                                      kArrow[1][1]);
    for (int i = 1; i < kVertices; ++i) {
        const int j = (i + 1) % kVertices;
        const double distance =
            segment_distance(x, y, kArrow[i][0], kArrow[i][1], kArrow[j][0], kArrow[j][1]);
        if (distance < nearest) {
            nearest = distance;
        }
    }
    return nearest;
}

void paint_arrow(void* raw_bits, int width, int height) {
    auto* bits = static_cast<uint32_t*>(raw_bits);
    constexpr int kSamples = kSamplesPerAxis * kSamplesPerAxis;
    const double scale_x = static_cast<double>(kDesignSize) / width;
    const double scale_y = static_cast<double>(kDesignSize) / height;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            int covered = 0;
            int white = 0;
            for (int sy = 0; sy < kSamplesPerAxis; ++sy) {
                for (int sx = 0; sx < kSamplesPerAxis; ++sx) {
                    const double x = (px + (sx + 0.5) / kSamplesPerAxis) * scale_x;
                    const double y = (py + (sy + 0.5) / kSamplesPerAxis) * scale_y;
                    if (!point_in_polygon(x, y)) {
                        continue;
                    }
                    ++covered;
                    if (outline_distance(x, y) >= kOutlineWidth) {
                        ++white;
                    }
                }
            }
            // Premultiplied BGRA: white fill, black stroke, nothing outside.
            const uint32_t alpha = static_cast<uint32_t>(kHintAlpha) * covered / kSamples;
            const uint32_t color = static_cast<uint32_t>(kHintAlpha) * white / kSamples;
            bits[py * width + px] = (alpha << 24) | (color << 16) | (color << 8) | color;
        }
    }
}

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

    HDC surface_dc = nullptr;
    HBITMAP surface_bitmap = nullptr;
    HGDIOBJ previous_bitmap = nullptr;
    int cursor_width = 0;
    int cursor_height = 0;
    POINT hotspot{};

    void run();
    bool move(HWND hwnd, int32_t x, int32_t y, std::string* error);
    void hide(HWND hwnd);
    void release_surface();

    static LRESULT window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

void CursorHintOverlay::Impl::release_surface() {
    if (surface_dc != nullptr && previous_bitmap != nullptr) {
        SelectObject(surface_dc, previous_bitmap);
    }
    if (surface_bitmap != nullptr) {
        DeleteObject(surface_bitmap);
    }
    if (surface_dc != nullptr) {
        DeleteDC(surface_dc);
    }
    surface_dc = nullptr;
    surface_bitmap = nullptr;
    previous_bitmap = nullptr;
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

    POINT destination{x - hotspot.x, y - hotspot.y};
    SIZE size{cursor_width, cursor_height};
    POINT source{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    const HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        *error = "GetDC: " + win32_error(GetLastError());
        return false;
    }
    const BOOL updated = UpdateLayeredWindow(hwnd, screen, &destination, &size, surface_dc,
                                             &source, 0, &blend, ULW_ALPHA);
    const DWORD update_error = GetLastError();
    ReleaseDC(nullptr, screen);
    if (updated == FALSE) {
        *error = "UpdateLayeredWindow: " + win32_error(update_error);
        return false;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    if (!SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        *error = "SetWindowPos: " + win32_error(GetLastError());
        return false;
    }
    visible = true;
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

    cursor_width = GetSystemMetrics(SM_CXCURSOR);
    cursor_height = GetSystemMetrics(SM_CYCURSOR);
    if (cursor_width <= 0) cursor_width = kDesignSize;
    if (cursor_height <= 0) cursor_height = kDesignSize;
    hotspot = POINT{static_cast<LONG>(std::lround(1.5 * cursor_width / kDesignSize)),
                    static_cast<LONG>(std::lround(1.0 * cursor_height / kDesignSize))};

    surface_dc = CreateCompatibleDC(nullptr);
    if (surface_dc == nullptr) {
        create_error = "CreateCompatibleDC: " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = cursor_width;
    info.bmiHeader.biHeight = -cursor_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    surface_bitmap = CreateDIBSection(surface_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (surface_bitmap == nullptr || bits == nullptr) {
        create_error = "CreateDIBSection: " + win32_error(GetLastError());
        release_surface();
        SetEvent(ready);
        return;
    }
    previous_bitmap = SelectObject(surface_dc, surface_bitmap);
    paint_arrow(bits, cursor_width, cursor_height);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Impl::window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"chast_cursor_hint";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        create_error = "RegisterClassExW: " + win32_error(GetLastError());
        release_surface();
        SetEvent(ready);
        return;
    }

    window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
            WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (window == nullptr) {
        create_error = "CreateWindowExW: " + win32_error(GetLastError());
        release_surface();
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
    release_surface();
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
