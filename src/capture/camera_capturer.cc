#define NOMINMAX
#include "chast/camera.h"
#include "chast/com_ptr.h"
#include <dxgi1_2.h>
#include <algorithm>
#include <cstring>

namespace chast {
namespace {
class CameraCapturer final : public IDxgiCapturer {
public:
    CameraCapturer(CameraDevice camera, LUID adapter, uint32_t fps)
        : camera_(std::move(camera)), requested_adapter_(adapter), fps_(fps) {
        // The device interface path is a stable identity, independent of the
        // camera's enumeration index or friendly name.
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            camera_.symbolic_link.data(), static_cast<int>(camera_.symbolic_link.size()), nullptr, 0);
        if (size > 0) {
            display_.device_name.resize(size);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, camera_.symbolic_link.data(),
                static_cast<int>(camera_.symbolic_link.size()), display_.device_name.data(), size);
        }
    }
    ~CameraCapturer() override { if (capture_) capture_->stop(); }
    Error reinitialize() override {
        if (capture_) capture_->stop();
        capture_.reset(); texture_.reset(); context_.reset(); device_.reset();
        frame_ = {};
        actual_adapter_ = {};
        if (display_.device_name.empty()) return Error(Status::InvalidCall, "camera has no valid device identity");
        ComPtr<IDXGIAdapter1> adapter;
        const bool specified = requested_adapter_.HighPart || requested_adapter_.LowPart;
        if (specified) {
            ComPtr<IDXGIFactory1> factory;
            auto status = check(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                reinterpret_cast<void**>(factory.put())), "camera DXGI factory");
            if (!status.ok()) return status;
            for (UINT index = 0;; ++index) {
                const HRESULT hr = factory->EnumAdapters1(index, adapter.put());
                if (FAILED(hr)) return Error(Status::Unavailable, "the camera's graphics adapter is unavailable");
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                    desc.AdapterLuid.HighPart == requested_adapter_.HighPart &&
                    desc.AdapterLuid.LowPart == requested_adapter_.LowPart) break;
            }
        }
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const auto create = [&](D3D_DRIVER_TYPE driver) {
            HRESULT hr = D3D11CreateDevice(adapter.get(), driver, nullptr, flags, levels, 2,
                D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
            if (hr == E_INVALIDARG)
                hr = D3D11CreateDevice(adapter.get(), driver, nullptr, flags, levels + 1, 1,
                    D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
            return hr;
        };
        HRESULT hr = create(specified ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE);
        if (FAILED(hr) && !specified) hr = create(D3D_DRIVER_TYPE_WARP);
        if (FAILED(hr)) return check(hr, "camera D3D11 device");
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> actual;
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put()))) &&
            SUCCEEDED(dxgi->GetAdapter(actual.put())) && SUCCEEDED(actual->GetDesc(&description)))
            actual_adapter_ = description.AdapterLuid;
        // Camera identity belongs to the device interface, even if Windows
        // chooses a different default graphics adapter during recovery.
        display_.adapter_vendor_id = description.VendorId;

        std::string reason;
        capture_ = open_camera(camera_, fps_, &reason);
        if (!capture_) return Error(Status::Unavailable, reason);
        const auto width = capture_->width(), height = capture_->height();
        if (!width || !height || (width & 1) || (height & 1) ||
            width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            return Error(Status::InvalidCall, "camera needs an even NV12 size within D3D11 limits");
        display_.width = width; display_.height = height;
        display_.rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        surface_width_ = (width + 15u) & ~15u;
        surface_height_ = (height + 15u) & ~15u;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = surface_width_; desc.Height = surface_height_;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = device_->CreateTexture2D(&desc, nullptr, texture_.put());
        if (FAILED(hr)) {
            // Software encoding needs only an uploadable NV12 texture.
            desc.BindFlags = desc.MiscFlags = 0;
            hr = device_->CreateTexture2D(&desc, nullptr, texture_.put());
        }
        if (FAILED(hr)) return check(hr, "camera NV12 texture");
        const auto luma = static_cast<size_t>(surface_width_) * surface_height_;
        upload_.assign(luma + luma / 2, 128);
        std::fill_n(upload_.begin(), luma, uint8_t{16});
        error_ = Error::success();
        return error_;
    }
    CaptureResult capture(std::chrono::milliseconds timeout) override {
        CameraFrame input;
        auto read = capture_->read(timeout, [&](CameraFrame&& frame) { input = std::move(frame); });
        if (read.status() == Status::WaitTimeout) return CaptureResult::NoNewFrame;
        if (!read.ok()) { error_ = read; return CaptureResult::ReinitNeeded; }
        const auto width = display_.width, height = display_.height;
        if (input.width != width || input.height != height) {
            error_ = Error(Status::Unavailable, "camera frame size changed");
            return CaptureResult::ReinitNeeded;
        }
        if (input.data.size() < static_cast<size_t>(width) * height * 3 / 2) {
            error_ = Error(Status::CaptureFailed, "truncated camera NV12 frame");
            return CaptureResult::FatalError;
        }
        for (UINT row = 0; row < height; ++row)
            std::memcpy(upload_.data() + static_cast<size_t>(row) * surface_width_,
                input.data.data() + static_cast<size_t>(row) * width, width);
        auto* chroma = upload_.data() + static_cast<size_t>(surface_width_) * surface_height_;
        const auto* source_chroma = input.data.data() + static_cast<size_t>(width) * height;
        for (UINT row = 0; row < height / 2; ++row)
            std::memcpy(chroma + static_cast<size_t>(row) * surface_width_,
                source_chroma + static_cast<size_t>(row) * width, width);
        context_->UpdateSubresource(texture_.get(), 0, nullptr, upload_.data(), surface_width_, 0);
        const auto device_status = check(device_->GetDeviceRemovedReason(), "camera upload device");
        if (!device_status.ok()) { error_ = device_status; return CaptureResult::ReinitNeeded; }
        frame_.width = width; frame_.height = height;
        frame_.device = device_.get(); frame_.context = context_.get();
        frame_.nv12_texture = texture_.get(); frame_.nv12_updated = frame_.desktop_updated = true;
        frame_.nv12_width = frame_.nv12_surface_width = surface_width_;
        frame_.nv12_height = frame_.nv12_surface_height = surface_height_;
        frame_.adapter_luid = actual_adapter_;
        ++frame_.frame_number;
        return CaptureResult::Frame;
    }
    const FrameInfo& frame() const override { return frame_; }
    const CursorInfo& cursor() const override { return cursor_; }
    const DisplayInfo& display() const override { return display_; }
    AdapterDevice adapter_device() const override {
        return {device_.get(), display_.adapter_vendor_id, actual_adapter_};
    }
    const char* implementation() const noexcept override { return "camera"; }
    UINT max_framerate() const override { return capture_ ? capture_->framerate() : fps_; }
    Error set_nv12_scale(UINT percent) override {
        return percent == 100 ? Error::success() : Error(Status::Unsupported, "camera capture uses its negotiated size");
    }
    const Error& last_error() const override { return error_; }
private:
    CameraDevice camera_;
    LUID requested_adapter_{};
    LUID actual_adapter_{};
    uint32_t fps_;
    std::unique_ptr<ICameraCapture> capture_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    UINT surface_width_ = 0, surface_height_ = 0;
    std::vector<uint8_t> upload_;
    DisplayInfo display_;
    FrameInfo frame_;
    CursorInfo cursor_;
    Error error_ = Error::success();
};
}

std::unique_ptr<IDxgiCapturer> create_camera_capturer(const CameraDevice& camera,
    LUID adapter, uint32_t framerate, std::string* error) {
    auto capture = std::make_unique<CameraCapturer>(camera, adapter, framerate);
    auto result = capture->reinitialize();
    if (!result.ok()) { if (error) *error = result.to_string(); return nullptr; }
    return capture;
}
} // namespace chast
