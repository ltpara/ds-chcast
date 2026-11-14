#include "chast/annotation_overlay.h"

#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

#include "win/annotation_history.h"
#include "win/annotation_paint.h"
#include "win/cursor_hint_paint.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace chast {

namespace {

constexpr DWORD kDefaultDesktopAccess =
    DESKTOP_CREATEMENU | DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
    DESKTOP_SWITCHDESKTOP | GENERIC_WRITE;
constexpr UINT kApplyMessage = WM_APP + 0x71;
constexpr uint64_t kMaxOverlayPixels = 64ull * 1024ull * 1024ull;
constexpr uint32_t kInkAlpha = 225;
constexpr uint32_t kShadowAlpha = 135;
// The host may stay connected for days. Keep undo useful for a long working
// set while putting a hard bound on points retained after they leave the
// screen; the oldest ink is evicted first.
constexpr size_t kMaxStoredStrokes = 2048;
constexpr size_t kMaxStoredPoints = 65536;

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
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size, nullptr, nullptr);
    return out;
}

struct MonitorEntry {
    RECT rect{};
};

struct ApplyRequest {
    const AnnotationMessage* annotation = nullptr;
    std::string* error = nullptr;
    bool applied = false;
};

}  // namespace

struct AnnotationOverlay::Impl {
    struct Plane {
        RECT rect{};
        HWND window = nullptr;
        HDC surface_dc = nullptr;
        HBITMAP surface_bitmap = nullptr;
        HGDIOBJ previous_bitmap = nullptr;
        uint32_t* bits = nullptr;
        int width = 0;
        int height = 0;
    };
    HANDLE ready = nullptr;
    std::thread thread;
    std::string create_error;
    std::atomic<bool> visible{false};
    std::vector<Plane> planes;
    detail::AnnotationHistory history;
    uint32_t color = 0xFFFFFF;
    double ink_radius = 4.5;

    void run(std::string label);
    bool create_planes(std::string* error);
    void release_planes();
    bool update_plane(Plane& plane, std::string* error);
    bool update_changed(const std::vector<bool>& changed, std::string* error);
    void clear_canvas(Plane& plane) const;
    void set_visible(bool show);
    bool render_all(std::string* error);
    bool draw_segment(const AnnotationPoint& from, const AnnotationPoint& to,
                      std::vector<bool>* changed, std::string* error);
    bool draw_point(const AnnotationPoint& point, std::vector<bool>* changed,
                    std::string* error);
    bool trim_history();
    bool apply(const AnnotationMessage& annotation, std::string* error);

    static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC dc, LPRECT rect,
                                         LPARAM context);
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                        LPARAM lparam);
};

BOOL CALLBACK AnnotationOverlay::Impl::collect_monitor(HMONITOR monitor, HDC,
                                                        LPRECT, LPARAM context) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }
    auto* monitors = reinterpret_cast<std::vector<MonitorEntry>*>(context);
    monitors->push_back(MonitorEntry{info.rcMonitor});
    return TRUE;
}

void AnnotationOverlay::Impl::release_planes() {
    for (Plane& plane : planes) {
        if (plane.window != nullptr) {
            DestroyWindow(plane.window);
            plane.window = nullptr;
        }
        if (plane.surface_dc != nullptr && plane.previous_bitmap != nullptr) {
            SelectObject(plane.surface_dc, plane.previous_bitmap);
        }
        if (plane.surface_bitmap != nullptr) {
            DeleteObject(plane.surface_bitmap);
        }
        if (plane.surface_dc != nullptr) {
            DeleteDC(plane.surface_dc);
        }
        plane.surface_bitmap = nullptr;
        plane.surface_dc = nullptr;
        plane.previous_bitmap = nullptr;
        plane.bits = nullptr;
    }
    planes.clear();
}

bool AnnotationOverlay::Impl::update_plane(Plane& plane, std::string* error) {
    const HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        *error = "GetDC: " + win32_error(GetLastError());
        return false;
    }
    POINT destination{plane.rect.left, plane.rect.top};
    SIZE size{plane.width, plane.height};
    POINT source{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL updated = UpdateLayeredWindow(plane.window, screen, &destination, &size,
                                             plane.surface_dc, &source, 0, &blend,
                                             ULW_ALPHA);
    const DWORD update_error = GetLastError();
    ReleaseDC(nullptr, screen);
    if (updated == FALSE) {
        *error = "UpdateLayeredWindow: " + win32_error(update_error);
        return false;
    }
    return true;
}

bool AnnotationOverlay::Impl::update_changed(const std::vector<bool>& changed,
                                             std::string* error) {
    for (size_t i = 0; i < planes.size() && i < changed.size(); ++i) {
        if (changed[i] && !update_plane(planes[i], error)) {
            return false;
        }
    }
    return true;
}

void AnnotationOverlay::Impl::clear_canvas(Plane& plane) const {
    if (plane.bits != nullptr) {
        std::fill(plane.bits, plane.bits + static_cast<size_t>(plane.width) * plane.height, 0);
    }
}

void AnnotationOverlay::Impl::set_visible(bool show) {
    for (Plane& plane : planes) {
        if (plane.window != nullptr) {
            ShowWindow(plane.window, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
    }
    visible.store(show);
}

bool AnnotationOverlay::Impl::draw_point(const AnnotationPoint& point,
                                           std::vector<bool>* changed,
                                           std::string*) {
    for (size_t i = 0; i < planes.size(); ++i) {
        Plane& plane = planes[i];
        const double shadow = ink_radius + 1.55;
        if (point.x + shadow < plane.rect.left || point.x - shadow >= plane.rect.right ||
            point.y + shadow < plane.rect.top || point.y - shadow >= plane.rect.bottom) {
            continue;
        }
        const int x = point.x - plane.rect.left;
        const int y = point.y - plane.rect.top;
        annotation_draw_segment(plane.bits, plane.width, plane.height, x, y, x, y,
                                shadow, 0x000000, kShadowAlpha);
        annotation_draw_segment(plane.bits, plane.width, plane.height, x, y, x, y,
                                ink_radius, color, kInkAlpha);
        (*changed)[i] = true;
    }
    return true;
}

bool AnnotationOverlay::Impl::draw_segment(const AnnotationPoint& from,
                                             const AnnotationPoint& to,
                                             std::vector<bool>* changed,
                                             std::string*) {
    const double shadow = ink_radius + 1.55;
    const int left = static_cast<int>(std::floor((std::min)(from.x, to.x) - shadow));
    const int right = static_cast<int>(std::ceil((std::max)(from.x, to.x) + shadow));
    const int top = static_cast<int>(std::floor((std::min)(from.y, to.y) - shadow));
    const int bottom = static_cast<int>(std::ceil((std::max)(from.y, to.y) + shadow));
    for (size_t i = 0; i < planes.size(); ++i) {
        Plane& plane = planes[i];
        if (right < plane.rect.left || left >= plane.rect.right || bottom < plane.rect.top ||
            top >= plane.rect.bottom) {
            continue;
        }
        annotation_draw_segment(plane.bits, plane.width, plane.height,
                                from.x - plane.rect.left, from.y - plane.rect.top,
                                to.x - plane.rect.left, to.y - plane.rect.top, shadow,
                                0x000000, kShadowAlpha);
        annotation_draw_segment(plane.bits, plane.width, plane.height,
                                from.x - plane.rect.left, from.y - plane.rect.top,
                                to.x - plane.rect.left, to.y - plane.rect.top, ink_radius,
                                color, kInkAlpha);
        (*changed)[i] = true;
    }
    return true;
}

bool AnnotationOverlay::Impl::render_all(std::string* error) {
    for (Plane& plane : planes) {
        clear_canvas(plane);
    }
    std::vector<bool> changed(planes.size(), true);
    for (const auto& stroke : history.strokes()) {
        if (stroke.points.empty()) {
            continue;
        }
        if (!draw_point(stroke.points.front(), &changed, error)) {
            return false;
        }
        for (size_t i = 1; i < stroke.points.size(); ++i) {
            if (!draw_segment(stroke.points[i - 1], stroke.points[i], &changed, error)) {
                return false;
            }
        }
    }
    if (!update_changed(changed, error)) {
        return false;
    }
    set_visible(!history.strokes().empty());
    return true;
}

bool AnnotationOverlay::Impl::trim_history() {
    return history.trim(kMaxStoredStrokes, kMaxStoredPoints);
}

bool AnnotationOverlay::Impl::apply(const AnnotationMessage& annotation,
                                      std::string* error) {
    switch (annotation.action) {
        case AnnotationAction::Clear:
            history.clear();
            return render_all(error);
        case AnnotationAction::Undo:
            history.undo();
            return render_all(error);
        case AnnotationAction::Begin:
        case AnnotationAction::Move:
        case AnnotationAction::End:
            break;
    }
    if (annotation.action == AnnotationAction::End && annotation.points.empty()) {
        history.end(annotation.stroke_id);
        return true;
    }
    if (annotation.points.empty()) {
        return true;
    }
    std::vector<bool> changed(planes.size(), false);
    bool drew = false;
    if (annotation.action == AnnotationAction::Begin) {
        if (!history.begin(annotation.stroke_id, annotation.points.front())) {
            return true;
        }
        if (!draw_point(annotation.points.front(), &changed, error)) {
            return false;
        }
        drew = true;
    }

    AnnotationPoint previous{};
    bool have_previous = false;
    if (annotation.action != AnnotationAction::Begin) {
        for (auto stroke = history.strokes().rbegin(); stroke != history.strokes().rend();
             ++stroke) {
            if (stroke->id == annotation.stroke_id && history.is_active(stroke->id)) {
                previous = stroke->points.back();
                have_previous = true;
                break;
            }
        }
        if (!have_previous) {
            return true;
        }
    } else {
        previous = annotation.points.front();
        have_previous = true;
    }

    const size_t first =
        annotation.action == AnnotationAction::Begin ? size_t{1} : size_t{0};
    for (size_t i = first; i < annotation.points.size(); ++i) {
        if (!history.append(annotation.stroke_id, annotation.points[i])) {
            return true;
        }
        if (!draw_segment(previous, annotation.points[i], &changed, error)) {
            return false;
        }
        previous = annotation.points[i];
        drew = true;
    }
    if (annotation.action == AnnotationAction::End) {
        history.end(annotation.stroke_id);
    }
    if (!drew) {
        return true;
    }
    if (trim_history()) {
        if (!render_all(error)) {
            return false;
        }
    } else if (!update_changed(changed, error)) {
        return false;
    }
    set_visible(true);
    return true;
}

bool AnnotationOverlay::Impl::create_planes(std::string* error) {
    std::vector<MonitorEntry> monitors;
    if (EnumDisplayMonitors(nullptr, nullptr, &Impl::collect_monitor,
                            reinterpret_cast<LPARAM>(&monitors)) == FALSE ||
        monitors.empty()) {
        *error = "EnumDisplayMonitors found no monitor";
        return false;
    }
    uint64_t total_pixels = 0;
    for (const MonitorEntry& monitor : monitors) {
        const int64_t width = static_cast<int64_t>(monitor.rect.right) - monitor.rect.left;
        const int64_t height = static_cast<int64_t>(monitor.rect.bottom) - monitor.rect.top;
        if (width <= 0 || height <= 0 || width > 32768 || height > 32768) {
            *error = "a monitor has an invalid overlay extent";
            return false;
        }
        total_pixels += static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
        if (total_pixels > kMaxOverlayPixels) {
            *error = "the virtual desktop is too large for a temporary annotation overlay";
            return false;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Impl::window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"chast_annotation_overlay";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        *error = "RegisterClassExW: " + win32_error(GetLastError());
        return false;
    }

    planes.reserve(monitors.size());
    for (const MonitorEntry& monitor : monitors) {
        Plane plane;
        plane.rect = monitor.rect;
        plane.width = monitor.rect.right - monitor.rect.left;
        plane.height = monitor.rect.bottom - monitor.rect.top;
        plane.window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                WS_EX_LAYERED,
            wc.lpszClassName, L"", WS_POPUP, plane.rect.left, plane.rect.top, plane.width,
            plane.height, nullptr, nullptr, wc.hInstance, &plane);
        if (plane.window == nullptr) {
            *error = "CreateWindowExW: " + win32_error(GetLastError());
            planes.push_back(std::move(plane));
            release_planes();
            return false;
        }
        // Keep temporary ink out of screenshots and recordings. A layered
        // window that cannot be excluded would silently leak annotations into
        // the remote picture, so refuse the overlay instead.
        if (!SetWindowDisplayAffinity(plane.window, WDA_EXCLUDEFROMCAPTURE)) {
            *error = "SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE): " +
                     win32_error(GetLastError());
            planes.push_back(std::move(plane));
            release_planes();
            return false;
        }
        plane.surface_dc = CreateCompatibleDC(nullptr);
        if (plane.surface_dc == nullptr) {
            *error = "CreateCompatibleDC: " + win32_error(GetLastError());
            planes.push_back(std::move(plane));
            release_planes();
            return false;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = plane.width;
        info.bmiHeader.biHeight = -plane.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        plane.surface_bitmap = CreateDIBSection(plane.surface_dc, &info, DIB_RGB_COLORS,
                                                &bits, nullptr, 0);
        if (plane.surface_bitmap == nullptr || bits == nullptr) {
            *error = "CreateDIBSection: " + win32_error(GetLastError());
            planes.push_back(std::move(plane));
            release_planes();
            return false;
        }
        plane.bits = static_cast<uint32_t*>(bits);
        plane.previous_bitmap = SelectObject(plane.surface_dc, plane.surface_bitmap);
        clear_canvas(plane);
        if (!update_plane(plane, error)) {
            planes.push_back(std::move(plane));
            SetWindowLongPtrW(planes.back().window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(&planes.back()));
            release_planes();
            return false;
        }
        SetWindowPos(plane.window, HWND_TOPMOST, plane.rect.left, plane.rect.top, plane.width,
                     plane.height, SWP_NOACTIVATE | SWP_HIDEWINDOW);
        planes.push_back(std::move(plane));
        // CreateWindowExW received the address of the stack object. Rebind it
        // to the stable vector slot so WM_DESTROY never writes through a
        // pointer to the moved-from local.
        SetWindowLongPtrW(planes.back().window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(&planes.back()));
    }
    return true;
}

void AnnotationOverlay::Impl::run(std::string label) {
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
    color = cursor_hint_color(label);
    const HDC screen = GetDC(nullptr);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    ink_radius = (std::max)(2.5, 4.5 * static_cast<double>((std::max)(96, dpi)) / 96.0);
    if (!create_planes(&create_error)) {
        SetEvent(ready);
        return;
    }
    SetEvent(ready);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    release_planes();
}

LRESULT CALLBACK AnnotationOverlay::Impl::window_proc(HWND window, UINT message,
                                                       WPARAM wparam, LPARAM lparam) {
    auto* plane = reinterpret_cast<Plane*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case kApplyMessage: {
            auto* request = reinterpret_cast<ApplyRequest*>(lparam);
            auto* impl = reinterpret_cast<Impl*>(GetPropW(window, L"chast_annotation_impl"));
            if (impl != nullptr && request != nullptr && request->annotation != nullptr) {
                request->applied = impl->apply(*request->annotation, request->error);
            }
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (plane != nullptr) {
                plane->window = nullptr;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

AnnotationOverlay::AnnotationOverlay() : impl_(std::make_unique<Impl>()) {}

AnnotationOverlay::~AnnotationOverlay() {
    if (!impl_->planes.empty() && impl_->planes.front().window != nullptr) {
        PostMessageW(impl_->planes.front().window, WM_CLOSE, 0, 0);
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->ready != nullptr) {
        CloseHandle(impl_->ready);
    }
}

std::unique_ptr<AnnotationOverlay> AnnotationOverlay::create(std::string* error,
                                                              const std::string& label) {
    std::unique_ptr<AnnotationOverlay> overlay(new AnnotationOverlay());
    Impl& impl = *overlay->impl_;
    impl.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl.ready == nullptr) {
        *error = "CreateEvent: " + win32_error(GetLastError());
        return nullptr;
    }
    try {
        impl.thread = std::thread([&impl, label] { impl.run(label); });
    } catch (const std::exception& exception) {
        *error = std::string("annotation overlay thread: ") + exception.what();
        return nullptr;
    }
    const DWORD waited = WaitForSingleObject(impl.ready, 5000);
    CloseHandle(impl.ready);
    impl.ready = nullptr;
    if (waited != WAIT_OBJECT_0 || impl.planes.empty()) {
        *error = !impl.create_error.empty() ? impl.create_error
                                            : "the annotation overlay did not start";
        impl.thread.join();
        return nullptr;
    }
    for (Impl::Plane& plane : impl.planes) {
        SetPropW(plane.window, L"chast_annotation_impl", &impl);
    }
    return overlay;
}

bool AnnotationOverlay::apply(const AnnotationMessage& annotation, std::string* error) {
    if (impl_->planes.empty()) {
        *error = "the annotation overlay is not running";
        return false;
    }
    ApplyRequest request{&annotation, error, false};
    SendMessageW(impl_->planes.front().window, kApplyMessage, 0,
                 reinterpret_cast<LPARAM>(&request));
    return request.applied;
}

}  // namespace chast
