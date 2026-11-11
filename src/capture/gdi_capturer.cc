// GDI desktop capture fallback.
//
// BitBlt writes the selected display into a top-down BGRA DIB. The frame is
// uploaded once to a shared D3D11 texture, then follows the normal GPU NV12
// conversion and encoder path. This is intentionally slower than Desktop
// Duplication, but keeps a host usable when DuplicateOutput is denied or lost.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/detail.h"
#include "chast/capture.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace chast {
namespace {

UINT scaled_even(UINT value, UINT percent) {
    const UINT scaled = static_cast<UINT>((static_cast<uint64_t>(value) * percent + 50) / 100);
    return std::max<UINT>(2, scaled) & ~1u;
}

class GdiCapturer final : public IDxgiCapturer {
public:
    GdiCapturer(DisplayInfo display, CaptureConfig config)
        : display_(std::move(display)), config_(config) {}

    GdiCapturer(DisplayInfo display, CaptureConfig config, ID3D11Device* device,
                ID3D11DeviceContext* context, UINT vendor_id)
        : display_(std::move(display)), config_(config), device_(device), context_(context),
          vendor_id_(vendor_id), external_device_(true) {
        device_.add_ref();
        context_.add_ref();
    }

    ~GdiCapturer() override { release_gdi(); }

    Error initialize() {
        const LONG rect_width = display_.rect.right - display_.rect.left;
        const LONG rect_height = display_.rect.bottom - display_.rect.top;
        if (rect_width <= 0 || rect_height <= 0) {
            return Error(Status::InvalidCall, "GDI capture needs a non-empty display rect");
        }
        width_ = static_cast<UINT>(rect_width);
        height_ = static_cast<UINT>(rect_height);
        if ((width_ & 1u) != 0 || (height_ & 1u) != 0) {
            return Error(Status::Unsupported, "GDI NV12 capture needs even display dimensions");
        }

        // BitBlt returns the display in desktop orientation, including panel
        // rotation, so consumers must not rotate it a second time.
        display_.width = width_;
        display_.height = height_;
        display_.rotation_degrees = 0;

        if (!device_ && config_.backend != CaptureBackend::GdiCpu) {
            detail::CaptureDevice created;
            Error err = detail::create_capture_device(display_.adapter_luid, created);
            if (!err.ok() && external_device_) return err;
            device_ = std::move(created.device);
            context_ = std::move(created.context);
            vendor_id_ = created.vendor_id;
        }

        screen_dc_ = CreateDCW(L"DISPLAY", display_.device_name.c_str(), nullptr, nullptr);
        if (!screen_dc_) return check_win32(FALSE, "CreateDCW(display)");
        memory_dc_ = CreateCompatibleDC(screen_dc_);
        if (!memory_dc_) return check_win32(FALSE, "CreateCompatibleDC");

        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = static_cast<LONG>(width_);
        bitmap.bmiHeader.biHeight = -static_cast<LONG>(height_);  // top-down BGRA
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        dib_ = CreateDIBSection(screen_dc_, &bitmap, DIB_RGB_COLORS, &pixels_, nullptr, 0);
        if (!dib_ || !pixels_) return check_win32(FALSE, "CreateDIBSection");
        previous_bitmap_ = SelectObject(memory_dc_, dib_);
        if (!previous_bitmap_ || previous_bitmap_ == HGDI_ERROR) {
            return check_win32(FALSE, "SelectObject(capture DIB)");
        }

        if (device_) {
            const auto upload = [&]() -> Error {
                D3D11_TEXTURE2D_DESC texture{};
                texture.Width = width_;
                texture.Height = height_;
                texture.MipLevels = 1;
                texture.ArraySize = 1;
                texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                texture.SampleDesc.Count = 1;
                texture.Usage = D3D11_USAGE_DEFAULT;
                texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                auto err = check(device_->CreateTexture2D(&texture, nullptr, texture_.put()),
                                 "CreateTexture2D(GDI upload)");
                if (!err.ok()) return err;
                auto resource = com_query<IDXGIResource>(texture_);
                if (!resource) return Error(Status::CaptureFailed, "GDI upload has no DXGI resource");
                err = check(resource->GetSharedHandle(&shared_handle_), "GetSharedHandle(GDI upload)");
                return err.ok() ? create_nv12_pass() : err;
            }();
            if (!upload.ok()) {
                if (external_device_) return upload;
                nv12_pass_.reset(); texture_.reset(); context_.reset(); device_.reset();
                shared_handle_ = nullptr; vendor_id_ = 0;
            }
        }

        frame_.device = device_.get();
        frame_.context = context_.get();
        frame_.texture = texture_.get();
        frame_.shared_handle = shared_handle_;
        frame_.width = width_;
        frame_.height = height_;
        frame_.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        frame_.adapter_luid = display_.adapter_luid;
        frame_.rotation_degrees = 0;
        next_capture_ = {};
        last_error_ = Error::success();
        return Error::success();
    }

    CaptureResult capture(std::chrono::milliseconds timeout) override {
        const auto now = std::chrono::steady_clock::now();
        if (next_capture_ != std::chrono::steady_clock::time_point{} && now < next_capture_) {
            if (timeout.count() <= 0) return CaptureResult::NoNewFrame;
            const auto allowed = std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
            if (next_capture_ - now > allowed) {
                std::this_thread::sleep_for(allowed);
                return CaptureResult::NoNewFrame;
            }
            std::this_thread::sleep_until(next_capture_);
        }
        const auto capture_started = std::chrono::steady_clock::now();

        if (!BitBlt(memory_dc_, 0, 0, static_cast<int>(width_), static_cast<int>(height_),
                    screen_dc_, 0, 0, SRCCOPY | CAPTUREBLT)) {
            last_error_ = check_win32(FALSE, "BitBlt(display)");
            return CaptureResult::ReinitNeeded;
        }
        cursor_ = {};
        if (config_.capture_cursor) sample_desktop_cursor(display_, cursor_);
        if (!device_) {
            const auto* begin = static_cast<const uint8_t*>(pixels_);
            frame_.cpu_bgra = std::make_shared<const std::vector<uint8_t>>(
                begin, begin + size_t(width_) * height_ * 4);
            frame_.nv12_width = scaled_even(width_, nv12_percent_);
            frame_.nv12_height = scaled_even(height_, nv12_percent_);
            frame_.desktop_updated = frame_.nv12_updated = true;
            frame_.frame_number = ++frame_number_;
            next_capture_ = capture_started + std::chrono::milliseconds(33);
            return CaptureResult::Frame;
        }

        context_->UpdateSubresource(texture_.get(), 0, nullptr, pixels_, width_ * 4u, 0);
        frame_.desktop_updated = true;
        frame_.frame_number = ++frame_number_;
        frame_.nv12_updated = false;
        if (nv12_pass_) {
            ID3D11Texture2D* nv12 = nullptr;
            Error err = nv12_pass_->convert(texture_.get(), &nv12);
            if (!err.ok()) {
                last_error_ = err;
                clear_nv12();
                return detail::stage_failure_is_permanent(err.status())
                           ? CaptureResult::FatalError
                           : CaptureResult::ReinitNeeded;
            }
            frame_.nv12_texture = nv12;
            frame_.nv12_shared_handle = nv12_pass_->shared_handle();
            frame_.nv12_width = nv12_pass_->width();
            frame_.nv12_height = nv12_pass_->height();
            frame_.nv12_surface_width = nv12_pass_->surface_width();
            frame_.nv12_surface_height = nv12_pass_->surface_height();
            frame_.nv12_updated = true;
        }
        context_->Flush();
        // Conversion time counts against the 30 fps budget. Scheduling from
        // completion would add capture cost to every frame interval.
        next_capture_ = capture_started + std::chrono::milliseconds(33);
        return CaptureResult::Frame;
    }

    const FrameInfo& frame() const override { return frame_; }
    const CursorInfo& cursor() const override { return cursor_; }
    const DisplayInfo& display() const override { return display_; }
    AdapterDevice adapter_device() const override {
        return AdapterDevice{device_.get(), vendor_id_, display_.adapter_luid};
    }
    const char* implementation() const noexcept override { return device_ ? "gdi" : "gdi-cpu"; }

    Error reinitialize() override {
        DisplayInfo refreshed = display_;
        bool found = false;
        for (const DisplayInfo& candidate : enumerate_displays()) {
            if (candidate.device_name == display_.device_name) {
                refreshed = candidate;
                found = true;
                break;
            }
        }
        if (!found) return Error(Status::Unavailable, "GDI display is no longer attached");

        release_gdi();
        texture_.reset();
        nv12_pass_.reset();
        clear_nv12();
        frame_ = {};
        cursor_ = {};
        display_ = std::move(refreshed);
        shared_handle_ = nullptr;
        if (!external_device_) {
            context_.reset();
            device_.reset();
            vendor_id_ = 0;
        }
        return initialize();
    }

    Error set_nv12_scale(UINT percent) override {
        if (percent == 0 || percent > 100) {
            return Error(Status::InvalidCall, "NV12 scale is a percentage from 1 to 100");
        }
        if (percent == nv12_percent_) return Error::success();
        const UINT previous = nv12_percent_;
        nv12_percent_ = percent;
        clear_nv12();
        nv12_pass_.reset();
        Error err = create_nv12_pass();
        if (!err.ok()) {
            nv12_percent_ = previous;
            nv12_pass_.reset();
            (void)create_nv12_pass();
        }
        return err;
    }

    UINT nv12_scale_percent() const override { return nv12_percent_; }
    const Error& last_error() const override { return last_error_; }

private:
    Error create_nv12_pass() {
        if (!device_ || !config_.nv12.enabled) return Error::success();
        auto next = std::make_unique<detail::Nv12Pass>();
        Error err = next->initialize(device_.get(), context_.get(), width_, height_,
                                     scaled_even(width_, nv12_percent_),
                                     scaled_even(height_, nv12_percent_),
                                     config_.nv12.colorimetry);
        if (!err.ok()) return err;
        nv12_pass_ = std::move(next);
        return Error::success();
    }

    void clear_nv12() {
        frame_.nv12_texture = nullptr;
        frame_.nv12_shared_handle = nullptr;
        frame_.nv12_updated = false;
        frame_.nv12_width = 0;
        frame_.nv12_height = 0;
        frame_.nv12_surface_width = 0;
        frame_.nv12_surface_height = 0;
    }

    void release_gdi() {
        if (memory_dc_ && previous_bitmap_ && previous_bitmap_ != HGDI_ERROR) {
            SelectObject(memory_dc_, previous_bitmap_);
        }
        previous_bitmap_ = nullptr;
        if (dib_) DeleteObject(dib_);
        dib_ = nullptr;
        pixels_ = nullptr;
        if (memory_dc_) DeleteDC(memory_dc_);
        memory_dc_ = nullptr;
        if (screen_dc_) DeleteDC(screen_dc_);
        screen_dc_ = nullptr;
    }

    DisplayInfo display_;
    CaptureConfig config_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    std::unique_ptr<detail::Nv12Pass> nv12_pass_;
    FrameInfo frame_;
    CursorInfo cursor_;
    Error last_error_ = Error::success();
    HDC screen_dc_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    void* pixels_ = nullptr;
    HANDLE shared_handle_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT vendor_id_ = 0;
    UINT nv12_percent_ = 100;
    uint64_t frame_number_ = 0;
    bool external_device_ = false;
    std::chrono::steady_clock::time_point next_capture_{};
};

}  // namespace

namespace detail {

std::unique_ptr<IDxgiCapturer> create_gdi_capturer(const DisplayInfo& display,
                                                   const CaptureConfig& config,
                                                   std::string* error) {
    auto capture = std::make_unique<GdiCapturer>(display, config);
    Error result = capture->initialize();
    if (!result.ok()) {
        if (error) *error = result.to_string();
        return nullptr;
    }
    return capture;
}

std::unique_ptr<IDxgiCapturer> create_gdi_capturer_on_device(
    const DisplayInfo& display, const CaptureConfig& config, ID3D11Device* device,
    ID3D11DeviceContext* context, UINT vendor_id, std::string* error) {
    if (!device || !context) {
        if (error) *error = "GDI shared capture needs a device and context";
        return nullptr;
    }
    auto capture = std::make_unique<GdiCapturer>(display, config, device, context, vendor_id);
    Error result = capture->initialize();
    if (!result.ok()) {
        if (error) *error = result.to_string();
        return nullptr;
    }
    return capture;
}

}  // namespace detail
}  // namespace chast
