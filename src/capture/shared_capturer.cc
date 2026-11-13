// A single duplication fans out into reader-owned GPU surfaces. Separate
// encoders may scale and pace independently without re-duplicating an output.
#include "capture/detail.h"
#include <d3d10.h>
#include <map>
#include <thread>

namespace chast {
namespace {
struct Source {
    std::mutex mutex;
    std::unique_ptr<IDxgiCapturer> capture;
    ComPtr<ID3D11Texture2D> texture;
    HANDLE handle = nullptr;
    FrameInfo frame;
    CursorInfo cursor;
    uint64_t sequence = 0;
    // Advances only when a capture carried new desktop pixels. Pointer-only
    // wakes keep it unchanged so a reader that missed ticks knows whether the
    // retained surface is new to it.
    uint64_t desktop_sequence = 0;
    uint64_t generation = 1;
    bool lost = false;
    bool want_hdr = true;
    CaptureBackend backend = CaptureBackend::Auto;
};
std::mutex sources_mutex;
std::map<std::wstring, std::weak_ptr<Source>> sources;

Error surface(ID3D11Device* device, ID3D11Texture2D* source,
              ComPtr<ID3D11Texture2D>& target, HANDLE& handle) {
    D3D11_TEXTURE2D_DESC desc{}, existing{};
    source->GetDesc(&desc);
    if (target) target->GetDesc(&existing);
    if (target && desc.Width == existing.Width && desc.Height == existing.Height &&
        desc.Format == existing.Format) return Error::success();
    target.reset();
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    auto result = check(device->CreateTexture2D(&desc, nullptr, target.put()), "shared capture texture");
    if (!result.ok()) return result;
    auto resource = com_query<IDXGIResource>(target);
    return check(resource->GetSharedHandle(&handle), "shared capture handle");
}

Error finish_gpu(ID3D11Device* device, ID3D11DeviceContext* context) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> event;
    auto result = check(device->CreateQuery(&desc, event.put()), "capture fence");
    if (!result.ok()) return result;
    context->End(event.get());
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    HRESULT hr;
    while ((hr = context->GetData(event.get(), nullptr, 0, 0)) == S_FALSE) {
        if (std::chrono::steady_clock::now() >= deadline)
            return Error(Status::Unavailable, "GPU capture fence timed out");
        std::this_thread::yield();
    }
    return check(hr, "capture fence");
}

class Reader final : public IDxgiCapturer {
public:
    Reader(std::shared_ptr<Source> source, CaptureConfig config)
        : source_(std::move(source)), config_(config) {}
    Error initialize(ID3D11Device* device = nullptr, ID3D11DeviceContext* context = nullptr,
                     UINT vendor = 0) {
        external_ = device != nullptr;
        const auto adapter = source_->capture->adapter_device();
        device_ = ComPtr<ID3D11Device>(device ? device : adapter.device);
        device_.add_ref();
        if (context) { context_ = ComPtr<ID3D11DeviceContext>(context); context_.add_ref(); }
        else if (device_) device_->GetImmediateContext(context_.put());
        auto threads = com_query<ID3D10Multithread>(device_);
        if (threads) threads->SetMultithreadProtected(TRUE);
        vendor_ = device ? vendor : adapter.vendor_id;
        display_ = source_->capture->display();
        generation_ = source_->generation;
        frame_.device = device_.get();
        frame_.context = context_.get();
        return Error::success();
    }
    CaptureResult capture(std::chrono::milliseconds timeout) override {
        if (cpu_fallback_) return capture_cpu(timeout);
        std::lock_guard<std::mutex> lock(source_->mutex);
        if (source_->lost || generation_ != source_->generation) return CaptureResult::ReinitNeeded;
        if (seen_ == source_->sequence && !dirty_) {
            const auto result = source_->capture->capture(timeout);
            if (result == CaptureResult::ReinitNeeded || result == CaptureResult::FatalError) {
                source_->lost = true;
                error_ = source_->capture->last_error();
                return result;
            }
            if (result == CaptureResult::NoNewFrame) return result;
            const auto& incoming = source_->capture->frame();
            if (incoming.cpu_bgra) {
                source_->frame = incoming;
                source_->cursor = source_->capture->cursor();
                ++source_->sequence;
                if (incoming.desktop_updated) ++source_->desktop_sequence;
            } else {
                if (!incoming.texture) return CaptureResult::NoNewFrame;
                // A pointer-only wake leaves the desktop image untouched. Keeping
                // the retained copy is not just cheaper: it is what lets every
                // reader report the wake as pointer-only instead of new pixels.
                if (incoming.desktop_updated || !source_->texture) {
                    error_ = surface(incoming.device, incoming.texture, source_->texture, source_->handle);
                    if (!error_.ok()) return CaptureResult::FatalError;
                    incoming.context->CopyResource(source_->texture.get(), incoming.texture);
                    if (incoming.desktop_updated) ++source_->desktop_sequence;
                }
                source_->frame = incoming;
                source_->frame.texture = source_->texture.get();
                source_->frame.shared_handle = source_->handle;
                source_->cursor = source_->capture->cursor();
                ++source_->sequence;
            }
        }
        if (source_->frame.cpu_bgra) {
            frame_ = source_->frame;
            frame_.nv12_width = (std::max)(2u, (frame_.width * percent_ + 50) / 100) & ~1u;
            frame_.nv12_height = (std::max)(2u, (frame_.height * percent_ + 50) / 100) & ~1u;
            cursor_ = config_.capture_cursor ? source_->cursor : CursorInfo{};
            display_ = source_->capture->display();
            seen_ = source_->sequence; dirty_ = false;
            return CaptureResult::Frame;
        }
        if (!source_->texture) { dirty_ = false; return CaptureResult::NoNewFrame; }
        const auto& incoming = source_->frame;
        cursor_ = config_.capture_cursor ? source_->cursor : CursorInfo{};
        display_ = source_->capture->display();
        seen_ = source_->sequence;
        // This reader may have skipped ticks while another read the source.
        // Its texture is fresh when it holds no surface yet or when the source
        // published desktop pixels since this reader last converted them.
        const bool desktop_fresh = dirty_ || !texture_ ||
            seen_desktop_ != source_->desktop_sequence;
        dirty_ = false;
        if (!desktop_fresh) {
            // Cursor moved over an unchanged desktop: reuse this reader's GPU
            // surface and NV12 copy, and let consumers see a pointer-only
            // frame. RustDesk's duplication returns WouldBlock for the same
            // wake and leaves cursor delivery to its own cursor services.
            frame_.frame_number = ++frame_number_;
            frame_.desktop_updated = false;
            frame_.nv12_updated = false;
            frame_.texture = texture_.get();
            frame_.shared_handle = handle_;
            return CaptureResult::Frame;
        }
        error_ = surface(device_.get(), source_->texture.get(), texture_, handle_);
        if (!error_.ok()) return CaptureResult::FatalError;
        const bool cross_device = device_.get() != incoming.device;
        ComPtr<ID3D11Texture2D> imported;
        if (cross_device) {
            error_ = finish_gpu(incoming.device, incoming.context);
            if (!error_.ok()) return CaptureResult::ReinitNeeded;
            error_ = check(device_->OpenSharedResource(source_->handle, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(imported.put())), "import capture surface");
            if (!error_.ok()) return CaptureResult::FatalError;
        }
        context_->CopyResource(texture_.get(), cross_device ? imported.get() : source_->texture.get());
        if (cross_device) {
            error_ = finish_gpu(device_.get(), context_.get());
            if (!error_.ok()) return CaptureResult::ReinitNeeded;
        }
        frame_ = incoming;
        frame_.texture = texture_.get();
        frame_.shared_handle = handle_;
        frame_.device = device_.get();
        frame_.context = context_.get();
        frame_.desktop_updated = true;
        frame_.frame_number = ++frame_number_;
        if (config_.nv12.enabled) {
            D3D11_TEXTURE2D_DESC desc{};
            texture_->GetDesc(&desc);
            if (!nv12_ || width_ != desc.Width || height_ != desc.Height) {
                auto next = std::make_unique<detail::Nv12Pass>();
                const UINT w = std::max<UINT>(2, (desc.Width * percent_ + 50) / 100) & ~1u;
                const UINT h = std::max<UINT>(2, (desc.Height * percent_ + 50) / 100) & ~1u;
                error_ = next->initialize(device_.get(), context_.get(), desc.Width, desc.Height,
                                          w, h, config_.nv12.colorimetry);
                if (!error_.ok()) return fallback_cpu(timeout);
                nv12_ = std::move(next);
                width_ = desc.Width; height_ = desc.Height;
            }
            error_ = nv12_->convert(texture_.get(), &frame_.nv12_texture);
            if (!error_.ok()) return fallback_cpu(timeout);
            frame_.nv12_shared_handle = nv12_->shared_handle();
            frame_.nv12_width = nv12_->width();
            frame_.nv12_height = nv12_->height();
            frame_.nv12_surface_width = nv12_->surface_width();
            frame_.nv12_surface_height = nv12_->surface_height();
            frame_.nv12_updated = true;
        }
        context_->Flush();
        dirty_ = false;
        seen_ = source_->sequence;
        seen_desktop_ = source_->desktop_sequence;
        return CaptureResult::Frame;
    }
    const FrameInfo& frame() const override { return cpu_fallback_ ? cpu_fallback_->frame() : frame_; }
    const CursorInfo& cursor() const override { return cpu_fallback_ ? cpu_fallback_->cursor() : cursor_; }
    const DisplayInfo& display() const override { return cpu_fallback_ ? cpu_fallback_->display() : display_; }
    AdapterDevice adapter_device() const override {
        if (cpu_fallback_ || frame_.cpu_bgra) return {};
        return {device_.get(), vendor_, display_.adapter_luid};
    }
    const char* implementation() const noexcept override {
        return cpu_fallback_ ? "gdi-cpu fallback" : source_->capture->implementation();
    }
    Error reinitialize() override {
        if (cpu_fallback_) return cpu_fallback_->reinitialize();
        std::lock_guard<std::mutex> lock(source_->mutex);
        if (generation_ == source_->generation) {
            source_->texture.reset();
            source_->frame = {};
            error_ = source_->capture->reinitialize();
            source_->lost = !error_.ok();
            if (!error_.ok()) return error_;
            ++source_->generation;
            source_->sequence = 0;
        }
        texture_.reset(); nv12_.reset(); frame_ = {}; cursor_ = {};
        seen_ = 0; seen_desktop_ = 0; dirty_ = false;
        if (external_) {
            generation_ = source_->generation;
            display_ = source_->capture->display();
            return Error::success();
        }
        return initialize();
    }
    Error set_nv12_scale(UINT percent) override {
        if (percent < 1 || percent > 100) return Error(Status::InvalidCall, "scale must be 1..100");
        if (cpu_fallback_) return cpu_fallback_->set_nv12_scale(percent);
        if (!device_ || frame_.cpu_bgra) { percent_ = percent; dirty_ = true; return Error::success(); }
        if (config_.nv12.enabled) {
            std::lock_guard<std::mutex> lock(source_->mutex);
            const UINT width = frame_.width ? frame_.width : display_.width;
            const UINT height = frame_.height ? frame_.height : display_.height;
            auto next = std::make_unique<detail::Nv12Pass>();
            const UINT w = std::max<UINT>(2, (width * percent + 50) / 100) & ~1u;
            const UINT h = std::max<UINT>(2, (height * percent + 50) / 100) & ~1u;
            auto result = next->initialize(device_.get(), context_.get(), width, height,
                                           w, h, config_.nv12.colorimetry);
            if (!result.ok()) return result;
            nv12_ = std::move(next);
            width_ = width; height_ = height;
            frame_.nv12_texture = nullptr;
            frame_.nv12_shared_handle = nullptr;
        }
        percent_ = percent; dirty_ = true;
        return Error::success();
    }
    UINT nv12_scale_percent() const override { return cpu_fallback_ ? cpu_fallback_->nv12_scale_percent() : percent_; }
    const Error& last_error() const override { return cpu_fallback_ ? cpu_fallback_->last_error() : error_; }
private:
    CaptureResult capture_cpu(std::chrono::milliseconds timeout) { return cpu_fallback_->capture(timeout); }
    CaptureResult fallback_cpu(std::chrono::milliseconds timeout) {
        if (external_) return CaptureResult::FatalError;
        auto config = config_; config.backend = CaptureBackend::GdiCpu;
        std::string error;
        auto next = create_capturer(display_, config, &error);
        if (!next) return CaptureResult::FatalError;
        if (!next->set_nv12_scale(percent_).ok()) return CaptureResult::FatalError;
        cpu_fallback_ = std::move(next);
        return capture_cpu(timeout);
    }
    std::unique_ptr<IDxgiCapturer> cpu_fallback_;
    std::shared_ptr<Source> source_;
    CaptureConfig config_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    HANDLE handle_ = nullptr;
    std::unique_ptr<detail::Nv12Pass> nv12_;
    FrameInfo frame_;
    CursorInfo cursor_;
    DisplayInfo display_;
    Error error_ = Error::success();
    uint64_t seen_ = 0, seen_desktop_ = 0, generation_ = 0, frame_number_ = 0;
    UINT width_ = 0, height_ = 0, percent_ = 100, vendor_ = 0;
    bool dirty_ = false, external_ = false;
};

std::unique_ptr<IDxgiCapturer> make_reader(const DisplayInfo& display, const CaptureConfig& config,
    ID3D11Device* device, ID3D11DeviceContext* context, UINT vendor, std::string* error) {
    std::lock_guard<std::mutex> lock(sources_mutex);
    auto source = sources[display.device_name].lock();
    if (!source) {
        source = std::make_shared<Source>();
        CaptureConfig raw;
        raw.want_hdr = config.want_hdr;
        source->want_hdr = config.want_hdr;
        raw.backend = config.backend;
        source->backend = config.backend;
        raw.capture_cursor = true;
        source->capture = create_capturer(display, raw, error);
        if (!source->capture) return nullptr;
        sources[display.device_name] = source;
    }
    if (source->want_hdr != config.want_hdr) {
        if (error) *error = "this output is already captured with a different HDR policy";
        return nullptr;
    }
    if (source->backend != config.backend) {
        if (error) *error = "this output is already captured with a different capture backend";
        return nullptr;
    }
    std::lock_guard<std::mutex> source_lock(source->mutex);
    auto reader = std::make_unique<Reader>(source, config);
    const auto result = reader->initialize(device, context, vendor);
    if (!result.ok()) { if (error) *error = result.to_string(); return nullptr; }
    return reader;
}
}
std::unique_ptr<IDxgiCapturer> create_shared_capturer(const DisplayInfo& display,
    const CaptureConfig& config, std::string* error) {
    return make_reader(display, config, nullptr, nullptr, 0, error);
}
std::unique_ptr<IDxgiCapturer> create_shared_capturer(size_t index,
    const CaptureConfig& config, std::string* error) {
    const auto displays = enumerate_displays();
    if (displays.empty()) { if (error) *error = "no attached display"; return nullptr; }
    return create_shared_capturer(displays[index < displays.size() ? index : 0], config, error);
}
namespace detail {
std::unique_ptr<IDxgiCapturer> create_shared_capturer_on_device(const DisplayInfo& display,
    const CaptureConfig& config, ID3D11Device* device, ID3D11DeviceContext* context,
    UINT vendor, std::string* error) {
    return make_reader(display, config, device, context, vendor, error);
}
}
}
