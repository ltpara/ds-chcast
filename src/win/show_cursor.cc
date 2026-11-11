#include "chast/show_cursor.h"

#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <thread>
#include <vector>

#include "win/cursor_hint_paint.h"

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
constexpr UINT_PTR kRippleTimer = 0x61;
constexpr int kRippleDurationMs = 420;
constexpr int kRippleFrameMs = 15;
constexpr int kMaxLabelChars = 48;

struct MoveRequest {
    int32_t x;
    int32_t y;
    bool pressed;
    std::string* error;
};

struct LabelRaster {
    std::vector<uint8_t> mask;
    int width = 0;
    int height = 0;
};

std::wstring to_wide_label(const std::string& label) {
    if (label.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, label.data(), static_cast<int>(label.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, label.data(), static_cast<int>(label.size()), text.data(),
                        size);
    std::wstring cleaned;
    cleaned.reserve(std::min<size_t>(text.size(), kMaxLabelChars));
    for (const wchar_t character : text) {
        if (character < 0x20 || character == 0x7f) {
            continue;
        }
        cleaned.push_back(character);
        if (cleaned.size() >= kMaxLabelChars) {
            break;
        }
    }
    return cleaned;
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

// Renders the viewer name as white text on black and keeps the red channel as
// glyph coverage. GDI writes no alpha into a DIB, so the painter tints the
// mask instead of copying the pixels.
bool render_label(const std::wstring& text, int font_height, LabelRaster* out,
                  std::string* error) {
    if (text.empty()) {
        return true;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        *error = "CreateCompatibleDC: " + win32_error(GetLastError());
        return false;
    }
    HFONT font = CreateFontW(-font_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (font == nullptr) {
        *error = "CreateFontW: " + win32_error(GetLastError());
        DeleteDC(dc);
        return false;
    }
    const HGDIOBJ previous_font = SelectObject(dc, font);
    SIZE extent{};
    if (!GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent) ||
        extent.cx <= 0 || extent.cy <= 0 || extent.cx > 4096 || extent.cy > 256) {
        *error = "the viewer name cannot be measured";
        SelectObject(dc, previous_font);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    const int width = extent.cx + 2;
    const int height = extent.cy + 2;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        *error = "CreateDIBSection: " + win32_error(GetLastError());
        SelectObject(dc, previous_font);
        DeleteObject(font);
        DeleteDC(dc);
        return false;
    }
    const HGDIOBJ previous_bitmap = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 1, 1, text.c_str(), static_cast<int>(text.size()));
    out->width = width;
    out->height = height;
    out->mask.resize(static_cast<size_t>(width) * height);
    const auto* pixels = static_cast<const uint32_t*>(bits);
    for (size_t i = 0; i < out->mask.size(); ++i) {
        out->mask[i] = static_cast<uint8_t>((pixels[i] >> 16) & 0xFFu);
    }
    SelectObject(dc, previous_bitmap);
    DeleteObject(bitmap);
    SelectObject(dc, previous_font);
    DeleteObject(font);
    DeleteDC(dc);
    return true;
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
    void* surface_bits = nullptr;
    int cursor_width = 0;
    int cursor_height = 0;
    POINT hotspot{};
    CursorHintLayout layout;
    uint32_t color = 0xFFFFFF;
    LabelRaster label;

    int32_t position_x = 0;
    int32_t position_y = 0;
    bool ripple_active = false;
    std::chrono::steady_clock::time_point ripple_start{};

    void run(std::wstring label_text, std::string label_key);
    bool place(HWND hwnd, int32_t x, int32_t y, bool pressed, std::string* error);
    double ripple_progress() const;
    bool render(HWND hwnd, double ripple, std::string* error);
    void tick(HWND hwnd);
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
    surface_bits = nullptr;
}

bool CursorHintOverlay::Impl::render(HWND hwnd, double ripple, std::string* error) {
    CursorHintSurface surface;
    surface.width = layout.width;
    surface.height = layout.height;
    surface.arrow_x = layout.arrow_x;
    surface.arrow_y = layout.arrow_y;
    surface.arrow_width = cursor_width;
    surface.arrow_height = cursor_height;
    surface.tip_x = layout.tip_x;
    surface.tip_y = layout.tip_y;
    surface.color = color;
    surface.ripple = ripple;
    surface.label_mask = label.mask.empty() ? nullptr : label.mask.data();
    surface.label_width = label.width;
    surface.label_height = label.height;
    surface.label_x = layout.label_x;
    surface.label_y = layout.label_y;
    paint_cursor_hint(static_cast<uint32_t*>(surface_bits), surface);

    POINT destination{position_x - layout.tip_x, position_y - layout.tip_y};
    SIZE size{layout.width, layout.height};
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

double CursorHintOverlay::Impl::ripple_progress() const {
    if (!ripple_active) {
        return -1.0;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - ripple_start)
                             .count();
    return std::min(1.0, static_cast<double>(elapsed) / kRippleDurationMs);
}

bool CursorHintOverlay::Impl::place(HWND hwnd, int32_t x, int32_t y, bool pressed,
                                    std::string* error) {
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
    position_x = x;
    position_y = y;
    // A click restarts the wave even if the previous one is still fading; the
    // following moves keep it running, so the ring follows the pointer.
    if (pressed) {
        ripple_active = true;
        ripple_start = std::chrono::steady_clock::now();
        SetTimer(hwnd, kRippleTimer, kRippleFrameMs, nullptr);
    }
    return render(hwnd, ripple_progress(), error);
}

void CursorHintOverlay::Impl::tick(HWND hwnd) {
    if (!ripple_active) {
        KillTimer(hwnd, kRippleTimer);
        return;
    }
    const double progress = ripple_progress();
    std::string error;
    if (!render(hwnd, progress, &error)) {
        ripple_active = false;
        KillTimer(hwnd, kRippleTimer);
        return;
    }
    if (progress >= 1.0) {
        ripple_active = false;
        KillTimer(hwnd, kRippleTimer);
    }
}

void CursorHintOverlay::Impl::hide(HWND hwnd) {
    KillTimer(hwnd, kRippleTimer);
    ripple_active = false;
    ShowWindow(hwnd, SW_HIDE);
    visible = false;
}

void CursorHintOverlay::Impl::run(std::wstring label_text, std::string label_key) {
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
    if (cursor_width <= 0) cursor_width = 32;
    if (cursor_height <= 0) cursor_height = 32;
    hotspot = POINT{static_cast<LONG>(std::lround(1.5 * cursor_width / 32)),
                    static_cast<LONG>(std::lround(1.0 * cursor_height / 32))};
    color = cursor_hint_color(label_key);

    const int font_height = std::max(11, cursor_height * 14 / 32);
    std::string label_error;
    if (!render_label(label_text, font_height, &label, &label_error)) {
        // A name that cannot be drawn must not hide the pointer; fall back to
        // the bare arrow.
        label = LabelRaster{};
    }
    layout = cursor_hint_layout(cursor_width, cursor_height, hotspot.x, hotspot.y, label.width,
                                label.height);

    surface_dc = CreateCompatibleDC(nullptr);
    if (surface_dc == nullptr) {
        create_error = "CreateCompatibleDC: " + win32_error(GetLastError());
        SetEvent(ready);
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = layout.width;
    info.bmiHeader.biHeight = -layout.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    surface_bitmap =
        CreateDIBSection(surface_dc, &info, DIB_RGB_COLORS, &surface_bits, nullptr, 0);
    if (surface_bitmap == nullptr || surface_bits == nullptr) {
        create_error = "CreateDIBSection: " + win32_error(GetLastError());
        release_surface();
        SetEvent(ready);
        return;
    }
    previous_bitmap = SelectObject(surface_dc, surface_bitmap);

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
                impl->place(hwnd, request->x, request->y, request->pressed, request->error);
            }
            return 0;
        case kHideMessage:
            if (impl != nullptr) {
                impl->hide(hwnd);
            }
            return 0;
        case WM_TIMER:
            if (impl != nullptr && wparam == kRippleTimer) {
                impl->tick(hwnd);
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

std::unique_ptr<CursorHintOverlay> CursorHintOverlay::create(std::string* error,
                                                             const std::string& label) {
    std::unique_ptr<CursorHintOverlay> overlay(new CursorHintOverlay());
    Impl& impl = *overlay->impl_;
    impl.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl.ready == nullptr) {
        *error = "CreateEvent: " + win32_error(GetLastError());
        return nullptr;
    }
    const std::wstring wide_label = to_wide_label(label);
    const std::string label_key = label;
    try {
        impl.thread = std::thread([&impl, wide_label, label_key] { impl.run(wide_label, label_key); });
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

bool CursorHintOverlay::move(int32_t x, int32_t y, bool pressed, std::string* error) {
    MoveRequest request{x, y, pressed, error};
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
