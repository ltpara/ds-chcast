// Desktop Duplication capturer.
//
// Per frame the pipeline is:
//
//   AcquireNextFrame -> desktop texture (GPU)
//     -> RotationPass  (D3D11 video processor, only on rotated panels)
//     -> HdrPass       (pixel shader, only when the desktop composes in FP16)
//     -> Nv12Pass      (video processor BGRA8 -> NV12, only when configured)
//     -> FrameInfo::texture (BGRA8, D3D11_RESOURCE_MISC_SHARED)
//        FrameInfo::nv12_texture (NV12, D3D11_RESOURCE_MISC_SHARED)
//
// Every stage runs on one D3D11 device and produces a texture on the GPU. The
// desktop image never reaches system memory; cursor shapes are the only pixels
// that do, and DXGI only hands them over on request.
//
// There is no CPU fallback by design. When DuplicateOutput fails (RDP
// sessions, protected content, no GPU) capturer creation fails and says why,
// instead of silently degrading to GDI BitBlt.

#include "capture/detail.h"
#include "capture/rotation.h"
#include "capture/shader.h"
#include "chast/capture.h"

#include <algorithm>
#include <vector>

namespace chast {

namespace {

class DxgiCapturer final : public IDxgiCapturer {
public:
    DxgiCapturer(DisplayInfo display, CaptureConfig config)
        : display_(std::move(display)), config_(config) {}
    ~DxgiCapturer() override { release_frame(); }

    Error initialize();

    CaptureResult capture(std::chrono::milliseconds timeout) override;
    const FrameInfo& frame() const override { return frame_; }
    const CursorInfo& cursor() const override { return cursor_; }
    const DisplayInfo& display() const override { return display_; }
    AdapterDevice adapter_device() const override {
        return AdapterDevice{device_.get(), vendor_id_, display_.adapter_luid};
    }
    Error reinitialize() override;
    const Error& last_error() const override { return last_error_; }

private:
    CaptureResult acquire(UINT timeout_ms, DXGI_OUTDUPL_FRAME_INFO& info,
                          ComPtr<ID3D11Texture2D>& desktop);
    void release_frame();
    Error create_device();
    Error create_duplication();
    // Records a non-fatal setup problem for last_error() without dropping an
    // earlier one (a degraded rotation and a missing NV12 pass can both happen).
    void note(const Error& err);
    Error run_stages(ID3D11Texture2D* desktop, ID3D11Texture2D** out, HANDLE* shared_out);
    Error run_nv12(ID3D11Texture2D* source);
    void release_nv12();
    // Builds (or, on a topology change, rebuilds) the NV12 pass for the size
    // the stages currently publish. A no-op when NV12 is not configured.
    void create_nv12_pass();
    // Size of the texture the stages publish: screen-space when the video
    // processor rotates, panel-native when the encoder has to carry the
    // rotation itself.
    void published_size(UINT& width, UINT& height) const;
    void pump_cursor(const DXGI_OUTDUPL_FRAME_INFO& info);
    void publish_common(ID3D11Texture2D* texture, bool desktop_updated,
                        HANDLE shared_handle);
    void publish_cursor();
    void clear_public_state();
    // Drops the duplication and re-creates it with the legacy DuplicateOutput
    // call, which yields BGRA8 (clipped) instead of FP16. Follows RustDesk's
    // abandon_tonemap: a conversion that failed is not retried against the
    // same duplication.
    Error re_duplicate_without_hdr();

    DisplayInfo display_;
    CaptureConfig config_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<IDXGIOutput5> output1_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    // The resource and texture handed out by AcquireNextFrame stay referenced
    // until ReleaseFrame. Dropping them first lets DXGI free the surface while
    // the frame is still held, and the driver then faults inside ReleaseFrame.
    ComPtr<IDXGIResource> held_resource_;
    ComPtr<ID3D11Texture2D> held_texture_;

    std::unique_ptr<detail::HdrPass> hdr_pass_;
    detail::RotationPass rotation_pass_;
    std::unique_ptr<detail::Nv12Pass> nv12_pass_;
    detail::CursorDecoder cursor_decoder_;
    std::unique_ptr<detail::ColorStateMonitor> color_monitor_;

    detail::ColorState color_;
    UINT vendor_id_ = 0;
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    UINT surface_width_ = 0;  // duplication (unrotated) size
    UINT surface_height_ = 0;
    UINT screen_width_ = 0;   // rotated size, what FrameInfo reports
    UINT screen_height_ = 0;

    FrameInfo frame_;
    CursorInfo cursor_;
    // Content id of the sprite currently copied into cursor_; 0 when none, so
    // the bytes are copied only when the shape actually changed.
    uint64_t published_content_id_ = 0;
    Error last_error_ = Error::success();
    bool frame_held_ = false;
    // Set when the previous desktop texture stopped being valid outside the
    // frame path (a stage was dropped after a failure). The next acquired frame
    // is then treated as a desktop update even if its Info says pointer-only.
    bool frame_dirty_ = false;
    uint64_t frame_number_ = 0;
};

Error DxgiCapturer::initialize() {
    Error err = create_device();
    if (!err.ok()) {
        return err;
    }
    err = create_duplication();
    if (!err.ok()) {
        return err;
    }

    const detail::DuplicationDesc desc =
        detail::describe_duplication(duplication_.get(), output_.get());
    rotation_ = desc.rotation;
    // Screen-space size comes from the output's desktop rect (already
    // oriented as the user sees it). The duplicated surface holds the same
    // image in panel-native orientation, so on a 90/270 panel its dimensions
    // are the swapped pair; the video processor swaps them back.
    screen_width_ = static_cast<UINT>(display_.rect.right - display_.rect.left);
    screen_height_ = static_cast<UINT>(display_.rect.bottom - display_.rect.top);
    if (screen_width_ == 0 || screen_height_ == 0) {
        screen_width_ = desc.desc.ModeDesc.Width;
        screen_height_ = desc.desc.ModeDesc.Height;
    }
    detail::surface_size(screen_width_, screen_height_, rotation_, surface_width_, surface_height_);
    color_ = desc.color;

    if (desc.desc.ModeDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // Only DuplicateOutput1 asks for FP16, and only when a shader exists to
        // normalize it; a duplication that came back FP16 always has a pass.
        hdr_pass_ = std::make_unique<detail::HdrPass>(device_.get(), context_.get());
    }

    if (rotation_ != DXGI_MODE_ROTATION_IDENTITY &&
        rotation_ != DXGI_MODE_ROTATION_UNSPECIFIED) {
        Error rot = rotation_pass_.initialize(device_.get(), context_.get(), rotation_,
                                              surface_width_, surface_height_, screen_width_,
                                              screen_height_);
        if (!rot.ok()) {
            // The encoder can still rotate itself; report the pending rotation
            // on every frame instead of failing capture.
            note(rot);
        }
    }

    color_monitor_ = std::make_unique<detail::ColorStateMonitor>(display_.device_name);

    // NV12 is its own pass, not a replacement for the BGRA frame: the encoder
    // picks the one it needs, and the BGRA texture stays the reference image
    // (the dump tool and any software consumer use it).
    create_nv12_pass();

    display_.width = screen_width_;
    display_.height = screen_height_;
    display_.rotation_degrees = detail::rotation_degrees(rotation_);
    display_.hdr = color_.hdr;
    display_.advanced_color_sdr = color_.wcg;
    display_.sdr_white_level = color_.sdr_white_level;
    display_.max_luminance_nits = color_.max_luminance;

    frame_.device = device_.get();
    frame_.context = context_.get();
    frame_.width = screen_width_;
    frame_.height = screen_height_;
    frame_.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame_.adapter_luid = display_.adapter_luid;
    // Non-zero only when the GPU could not rotate; the encoder then carries
    // it while consuming the unrotated texture.
    frame_.rotation_degrees = rotation_pass_.ready() ? 0 : detail::rotation_degrees(rotation_);
    // A fresh duplication's first acquired frame holds the whole desktop, but
    // DXGI reports it with LastPresentTime zero; force it through the stages so
    // the first Frame is never a null texture.
    frame_dirty_ = true;
    return Error::success();
}

void DxgiCapturer::published_size(UINT& width, UINT& height) const {
    // With a rotation pass the stages publish the rotated (screen-space)
    // image; without one they pass the panel-native surface through and the
    // encoder rotates, so NV12 must match that surface instead.
    if (rotation_pass_.ready()) {
        width = screen_width_;
        height = screen_height_;
    } else {
        width = surface_width_;
        height = surface_height_;
    }
}

void DxgiCapturer::create_nv12_pass() {
    nv12_pass_.reset();
    if (!config_.nv12.enabled) {
        return;
    }
    UINT width = 0;
    UINT height = 0;
    published_size(width, height);
    // NV12 subsamples 2x2, so a frame with an odd dimension cannot be
    // represented; such a mode stays BGRA-only rather than rounding the image
    // up and shifting everything by a pixel.
    if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
        note(Error(Status::Unsupported, "NV12 needs even frame dimensions; " +
                                            std::to_string(width) + "x" +
                                            std::to_string(height) + " stays BGRA8"));
        return;
    }
    auto pass = std::make_unique<detail::Nv12Pass>();
    const Error nv12 = pass->initialize(device_.get(), context_.get(), width, height,
                                        config_.nv12.colorimetry);
    if (nv12.ok()) {
        nv12_pass_ = std::move(pass);
    } else {
        // RustDesk's VRAM encoder has no CPU fallback either: when the GPU
        // cannot convert, the encoder is simply not offered NV12.
        note(nv12);
    }
}

void DxgiCapturer::note(const Error& err) {
    if (!err.ok() && last_error_.ok()) {
        last_error_ = err;
    }
}

Error DxgiCapturer::create_device() {
    ComPtr<IDXGIFactory1> factory;
    Error err = check(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                         reinterpret_cast<void**>(factory.put())),
                      "CreateDXGIFactory1");
    if (!err.ok()) {
        return err;
    }

    // Resolve the adapter by LUID: adapter indices shift when GPUs are
    // hot-plugged or disabled, LUIDs do not.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(index, candidate.put()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
            desc.AdapterLuid.LowPart == display_.adapter_luid.LowPart &&
            desc.AdapterLuid.HighPart == display_.adapter_luid.HighPart) {
            // Exported through adapter_device() so an encoder can pick the
            // vendor's hardware encoder (RustDesk's AdapterDevice does the
            // same); the reinit path re-resolves the adapter, so refresh it.
            vendor_id_ = desc.VendorId;
            adapter = std::move(candidate);
            break;
        }
    }
    if (!adapter) {
        return Error(Status::Unavailable, "the display's adapter is no longer present");
    }

    // BGRA support is mandatory for the video processor. VIDEO_SUPPORT lets
    // the device create the NV12 video-encoder binding the hardware encoder
    // imports (hwcodec's encode device sets it for the same reason). WARP and
    // the debug layer are pointless: a desktop can only be duplicated on a
    // real output of a real adapter.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, device_.put(), &level,
                                   context_.put());
    if (hr == E_INVALIDARG) {
        // Pre-Windows 8 runtimes reject 11_1 in the feature-level list.
        hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &levels[1],
                               1, D3D11_SDK_VERSION, device_.put(), &level, context_.put());
    }
    return check(hr, "D3D11CreateDevice");
}

Error DxgiCapturer::create_duplication() {
    // The device's adapter comes from IDXGIDevice, not from a QueryInterface
    // for IDXGIAdapter on the device itself (which is not supported).
    ComPtr<IDXGIDevice> dxgi_device = com_query<IDXGIDevice>(device_);
    ComPtr<IDXGIAdapter> base_adapter;
    if (!dxgi_device ||
        FAILED(dxgi_device->GetAdapter(base_adapter.put()))) {
        return Error(Status::Unavailable, "the capture device has no DXGI adapter");
    }
    ComPtr<IDXGIAdapter1> adapter = com_query<IDXGIAdapter1>(base_adapter);

    ComPtr<IDXGIOutput> output;
    std::vector<std::wstring> seen;
    for (UINT index = 0; adapter; ++index) {
        ComPtr<IDXGIOutput> candidate;
        HRESULT hr = adapter->EnumOutputs(index, candidate.put());
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            seen.emplace_back(L"<EnumOutputs failed>");
            break;
        }
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(candidate->GetDesc(&desc))) {
            seen.emplace_back(L"<undescribable>");
            continue;
        }
        seen.emplace_back(desc.DeviceName);
        if (display_.device_name == desc.DeviceName) {
            output = std::move(candidate);
            break;
        }
    }
    if (!output) {
        // Topology changed since enumeration (unplug, rotation, mode change).
        std::string available;
        for (const std::wstring& name : seen) {
            if (!available.empty()) {
                available += ", ";
            }
            available += detail::to_utf8(name);
        }
        return Error(Status::AccessLost, "output " + detail::to_utf8(display_.device_name) +
                                             " is not on its adapter (found: " +
                                             (available.empty() ? "none" : available) + ")");
    }
    output_ = output;
    // DuplicateOutput1 (the FP16 path) lives on IDXGIOutput5, DuplicateOutput
    // on IDXGIOutput1.
    output1_ = com_query<IDXGIOutput5>(output);
    if (!output1_) {
        return Error(Status::Unsupported,
                     "IDXGIOutput5 unavailable (Windows 7 without the platform update)");
    }

    // DuplicateOutput1 hands over FP16 scRGB on HDR desktops; the legacy call
    // converts to BGRA8 by clipping, which washes out HDR hosts. FP16 is only
    // requested where a shader exists to normalize it, since there is no CPU
    // fallback that could convert it later.
    if (config_.want_hdr && detail::HdrPass::available()) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       DXGI_FORMAT_B8G8R8A8_UNORM};
        HRESULT hr = output1_->DuplicateOutput1(device_.get(), 0, ARRAYSIZE(formats), formats,
                                               duplication_.put());
        if (SUCCEEDED(hr)) {
            return Error::success();
        }
        // Falling through is deliberate: DuplicateOutput1 can fail on stacks
        // that still support the legacy call (e.g. third-party display
        // drivers), and an SDR-quality capture beats none.
    }
    return check(com_query<IDXGIOutput1>(output_)->DuplicateOutput(device_.get(),
                                                                 duplication_.put()),
                 "DuplicateOutput");
}

Error DxgiCapturer::re_duplicate_without_hdr() {
    // The duplication whose frames the FP16 pass could not convert is dropped
    // whole: releasing the frame first keeps DXGI from invalidating it under
    // us, and resetting the duplication makes the next AcquireNextFrame start
    // on a fresh surface instead of a half-converted one.
    release_frame();
    duplication_.reset();
    if (!output_) {
        return Error(Status::InvalidCall, "no output to re-duplicate");
    }

    // The legacy call always yields BGRA8, converted by DXGI with a clip on
    // HDR desktops. That is the same trade-off RustDesk's abandon_tonemap
    // makes; it falls back to GDI there, which is the one thing this capturer
    // never does.
    Error err = check(com_query<IDXGIOutput1>(output_)->DuplicateOutput(device_.get(),
                                                                       duplication_.put()),
                      "DuplicateOutput(legacy)");
    if (!err.ok()) {
        return err;
    }
    hdr_pass_.reset();

    // The new duplication can report a different mode (a display mode changed
    // while the HDR pass was failing) and its color state is a fresh snapshot.
    const detail::DuplicationDesc desc =
        detail::describe_duplication(duplication_.get(), output_.get());
    rotation_ = desc.rotation;
    color_ = desc.color;
    display_.hdr = color_.hdr;
    display_.advanced_color_sdr = color_.wcg;
    display_.sdr_white_level = color_.sdr_white_level;
    display_.max_luminance_nits = color_.max_luminance;
    display_.rotation_degrees = detail::rotation_degrees(rotation_);
    detail::surface_size(screen_width_, screen_height_, rotation_, surface_width_,
                         surface_height_);
    frame_.rotation_degrees =
        rotation_pass_.ready() ? 0 : detail::rotation_degrees(rotation_);

    // The NV12 target is sized for the frame the stages publish, which can
    // differ after a mode or rotation change (and whether the video processor
    // rotates can change with the new caps), so it is rebuilt here alongside
    // the rotation pass. The next desktop frame converts again.
    frame_.nv12_texture = nullptr;
    frame_.nv12_shared_handle = nullptr;
    frame_.nv12_surface_width = 0;
    frame_.nv12_surface_height = 0;
    frame_.nv12_updated = false;
    create_nv12_pass();

    // The pointer shape belongs to the old duplication; the next mouse update
    // re-fetches it.
    cursor_decoder_.clear_shape();

    // The published texture came from the duplication just dropped, so it is
    // gone even if the next frame only reports a pointer update: the caller
    // must be given a full frame, not a stale pointer. The HDR pass was dropped
    // above as well, so any BGRA stage output is gone; the NV12 copy (made from
    // the old desktop) is stale too.
    frame_.texture = nullptr;
    frame_.shared_handle = nullptr;
    release_nv12();
    frame_dirty_ = true;
    return Error::success();
}

CaptureResult DxgiCapturer::capture(std::chrono::milliseconds timeout) {
    UINT ms = 0;
    if (timeout.count() > 0) {
        ms = static_cast<UINT>(std::min<long long>(timeout.count(), UINT_MAX));
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<ID3D11Texture2D> desktop;
    const CaptureResult acquired = acquire(ms, info, desktop);
    if (acquired != CaptureResult::Frame) {
        return acquired;
    }

    // Pointer shape must be copied before ReleaseFrame; the position comes
    // with this frame. Do that first so an error in the pixel path cannot
    // lose it.
    if (config_.capture_cursor) {
        pump_cursor(info);
    }

    const bool desktop_changed = info.AccumulatedFrames > 0 && info.LastPresentTime.QuadPart != 0;
    if (!desktop_changed && !frame_dirty_) {
        // The pointer moved over an otherwise static desktop. A texture this
        // capturer owns (a stage output) still holds the newest image, so it
        // is republished; a pass-through frame was the previous duplication
        // surface, which the acquire above already released, so the texture
        // held right now -- same desktop image, not presented again -- is what
        // gets published. Either way the frame stays held until the next
        // acquire, because ReleaseFrame would invalidate it.
        //
        // The NV12 texture was converted from that same image, so it is still
        // the matching pair and is not converted again.
        ID3D11Texture2D* texture = frame_.texture;
        HANDLE handle = frame_.shared_handle;
        if (handle == nullptr) {
            texture = desktop.get();
        }
        publish_common(texture, /*desktop_updated=*/false, handle);
        frame_.nv12_updated = false;
        return CaptureResult::Frame;
    }

    // run_stages hands back a borrowed pointer: either the duplicated texture
    // itself or a texture the pass owns. Owning it here would over-release the
    // duplication surface, which DXGI reports on the *next* AcquireNextFrame
    // as E_INVALIDARG.
    ID3D11Texture2D* staged = nullptr;
    HANDLE staged_handle = nullptr;
    Error err = run_stages(desktop.get(), &staged, &staged_handle);
    if (!err.ok()) {
        last_error_ = err;
        if (hdr_pass_ && detail::stage_failure_is_permanent(err.status())) {
            // The FP16 conversion is what failed. Stop asking for FP16 in this
            // process, then re-duplicate so DXGI does the conversion itself;
            // the alternative is failing every frame from now on. Like
            // RustDesk's abandon_tonemap, the caller sees a no-new-frame and
            // asks again, which is what its WouldBlock decodes to.
            detail::HdrPass::mark_unavailable();
            hdr_pass_.reset();
            const Error abandon = re_duplicate_without_hdr();
            if (!abandon.ok()) {
                last_error_ = abandon;
                return needs_reinit(abandon.status()) ? CaptureResult::ReinitNeeded
                                                      : CaptureResult::FatalError;
            }
            return CaptureResult::NoNewFrame;
        }
        release_frame();
        // The publishable texture may have been the duplication surface, which
        // ReleaseFrame just invalidated; the NV12 copy was made from it in an
        // earlier frame and is equally stale.
        frame_.texture = nullptr;
        frame_.shared_handle = nullptr;
        frame_.nv12_texture = nullptr;
        frame_.nv12_shared_handle = nullptr;
    frame_.nv12_surface_width = 0;
    frame_.nv12_surface_height = 0;
        frame_.nv12_updated = false;
        frame_dirty_ = true;
        return needs_reinit(err.status()) ? CaptureResult::ReinitNeeded
                                                   : CaptureResult::FatalError;
    }

    // A stage output is a texture we own and survives the release; a direct
    // pass-through of the duplication texture does not, so that frame is kept
    // held until the next acquire. Releasing here also matters on HDR
    // desktops: DXGI keeps the compositor from drawing into the duplicated
    // surface while a frame is held.
    //
    // The NV12 conversion runs before the release because it reads that same
    // duplication surface. It writes its own texture, so it does not affect
    // whether the frame can be released.
    frame_.nv12_updated = false;
    if (nv12_pass_) {
        const Error nv12 = run_nv12(staged);
        if (!nv12.ok()) {
            note(nv12);
            if (detail::stage_failure_is_permanent(nv12.status())) {
                // The enum/processor/blit combination is rejected on every
                // frame, so stop paying for it and stop publishing NV12.
                nv12_pass_.reset();
                frame_.nv12_texture = nullptr;
                frame_.nv12_shared_handle = nullptr;
    frame_.nv12_surface_width = 0;
    frame_.nv12_surface_height = 0;
            }
        } else {
            frame_.nv12_updated = true;
        }
    }

    const bool passthrough = staged == desktop.get();
    if (!passthrough) {
        release_frame();
    }

    // A pass-through frame that could not be converted still publishes the
    // desktop texture: "NV12 is missing" is reported by leaving the NV12
    // pointers null, not by failing the frame.
    publish_common(staged, /*desktop_updated=*/true, staged_handle);
    return CaptureResult::Frame;
}

Error DxgiCapturer::run_nv12(ID3D11Texture2D* source) {
    if (!nv12_pass_) {
        release_nv12();
        return Error(Status::Unsupported, "no NV12 pass");
    }
    ID3D11Texture2D* nv12 = nullptr;
    const Error err = nv12_pass_->convert(source, &nv12);
    if (!err.ok()) {
        // The pass's texture still holds the previous frame's image, which
        // does not go with the BGRA frame just published; withdraw it rather
        // than let a consumer send stale NV12.
        release_nv12();
        return err;
    }
    frame_.nv12_texture = nv12;
    frame_.nv12_shared_handle = nv12_pass_->shared_handle();
    frame_.nv12_surface_width = nv12_pass_->surface_width();
    frame_.nv12_surface_height = nv12_pass_->surface_height();
    return Error::success();
}

void DxgiCapturer::release_nv12() {
    // Not a release of the texture: the pass owns it. This only stops the
    // frame from advertising a copy that no longer matches the BGRA frame.
    frame_.nv12_texture = nullptr;
    frame_.nv12_shared_handle = nullptr;
    frame_.nv12_surface_width = 0;
    frame_.nv12_surface_height = 0;
    frame_.nv12_updated = false;
}

CaptureResult DxgiCapturer::acquire(UINT timeout_ms, DXGI_OUTDUPL_FRAME_INFO& info,
                                    ComPtr<ID3D11Texture2D>& desktop) {
    // A frame is held only until the next acquire, so drop the previous one
    // first. This also covers the early-return paths above.
    release_frame();

    HRESULT hr = duplication_->AcquireNextFrame(timeout_ms, &info, held_resource_.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return CaptureResult::NoNewFrame;
    }
    if (FAILED(hr)) {
        last_error_ = check(hr, "AcquireNextFrame");
        return needs_reinit(status_from_hresult(hr)) ? CaptureResult::ReinitNeeded
                                                             : CaptureResult::FatalError;
    }
    frame_held_ = true;

    hr = held_resource_->QueryInterface(__uuidof(ID3D11Texture2D),
                                        reinterpret_cast<void**>(held_texture_.put()));
    if (FAILED(hr)) {
        last_error_ = check(hr, "IDXGIResource::QueryInterface(ID3D11Texture2D)");
        release_frame();
        return CaptureResult::FatalError;
    }
    desktop = held_texture_;
    return CaptureResult::Frame;
}

void DxgiCapturer::release_frame() {
    if (!frame_held_ || !duplication_) {
        return;
    }
    // Flush before releasing: the duplicated texture is invalidated by
    // ReleaseFrame, and any staged copy still queued on the immediate context
    // must complete while it is still held.
    context_->Flush();
    HRESULT hr = duplication_->ReleaseFrame();
    if (FAILED(hr) && last_error_.ok()) {
        // A failed release makes DXGI refuse later acquires, but the next
        // acquire reports that; record it for diagnostics.
        last_error_ = check(hr, "ReleaseFrame");
    }
    frame_held_ = false;
    held_texture_.reset();
    held_resource_.reset();
}

Error DxgiCapturer::run_stages(ID3D11Texture2D* desktop, ID3D11Texture2D** out,
                               HANDLE* shared_out) {
    *out = nullptr;
    *shared_out = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desktop->GetDesc(&desc);
    ID3D11Texture2D* current = desktop;

    if (rotation_pass_.ready()) {
        Error err = rotation_pass_.rotate(current, &current);
        if (!err.ok()) {
            return err;
        }
        // When the HDR pass follows, its target replaces this one and its
        // handle is the one an encoder must import.
        *shared_out = rotation_pass_.shared_handle();
    }

    if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        if (!hdr_pass_) {
            return Error(Status::Unsupported, "FP16 desktop without an HDR pass");
        }
        detail::HdrPass::Params params;
        params.hdr = color_.hdr;
        params.sdr_white_level = color_.sdr_white_level;
        params.max_luminance = color_.max_luminance;
        Error err = hdr_pass_->convert(current, params, &current);
        if (!err.ok()) {
            return err;
        }
        *shared_out = hdr_pass_->shared_handle();
    }
    *out = current;

    // Advanced Color can be toggled without losing the duplication. Re-read
    // the state (rate-limited) so a desktop that turned HDR on or off is
    // normalized with the right white level from the next frame on.
    if (color_monitor_) {
        bool changed = false;
        detail::ColorState fresh = color_monitor_->refresh(/*force=*/false, &changed);
        if (changed) {
            color_ = fresh;
        }
    }
    return Error::success();
}

void DxgiCapturer::pump_cursor(const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (info.PointerPosition.Visible) {
        cursor_decoder_.update_position(info.PointerPosition.Position, true);
    } else if (cursor_decoder_.has_position()) {
        // Hidden pointers still have a position; keep the last one but mark
        // the pointer invisible.
        cursor_decoder_.update_position(cursor_decoder_.position(), false);
    }
    // The shape arrives on the frame that reports a mouse update. Calling
    // this unconditionally is also correct (DXGI answers "unchanged" with
    // zero bytes) but costs a round trip per frame.
    if (info.LastMouseUpdateTime.QuadPart != 0 || info.PointerPosition.Visible) {
        cursor_decoder_.update_shape(duplication_.get());
    }
}

void DxgiCapturer::publish_common(ID3D11Texture2D* texture, bool desktop_updated,
                                  HANDLE shared_handle) {
    frame_.texture = texture;
    frame_.shared_handle = shared_handle;
    if (texture) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        frame_.width = desc.Width;
        frame_.height = desc.Height;
        frame_.format = desc.Format;
    }
    frame_.desktop_updated = desktop_updated;
    frame_.frame_number = ++frame_number_;
    frame_dirty_ = false;
    publish_cursor();
}

void DxgiCapturer::publish_cursor() {
    if (!config_.capture_cursor) {
        cursor_ = {};
        published_content_id_ = 0;
        return;
    }
    const auto& decoded = cursor_decoder_;
    cursor_.visible = decoded.visible();
    cursor_.position_valid = decoded.has_position();
    // DXGI reports the pointer in the duplicated surface's space, which on a
    // rotated panel is the unrotated one; the published texture has the
    // rotation applied, so the position follows it.
    cursor_.position = decoded.position();
    if (rotation_pass_.ready()) {
        cursor_.position = detail::surface_to_output(decoded.position(), surface_width_,
                                                     surface_height_, rotation_);
    }
    cursor_.shape_valid = decoded.has_shape();
    cursor_.shape_type = static_cast<DXGI_OUTDUPL_POINTER_SHAPE_TYPE>(decoded.shape().Type);
    // As drawn: an outlined sprite is two pixels larger than the shape DXGI
    // reported, and the bitmap below is exactly width*height pixels.
    cursor_.width = decoded.bitmap().width;
    cursor_.height = decoded.bitmap().height;
    cursor_.pitch = decoded.shape().Pitch;
    cursor_.origin_offset = decoded.origin_offset();
    cursor_.hotspot = decoded.hotspot();
    cursor_.content_id = decoded.content_id();
    cursor_.revision = decoded.revision();

    // The sprite bytes are the one copy in this path that scales with the
    // pointer size, and they change far less often than frames arrive, so they
    // are only copied when the content id says the image moved on.
    if (cursor_.content_id != published_content_id_) {
        published_content_id_ = cursor_.content_id;
        cursor_.bitmap = decoded.bitmap().color;
        cursor_.invert = decoded.bitmap().invert;
        cursor_.outlined = decoded.bitmap().outlined;
        // Published shapes are kept process-wide, like RustDesk's
        // CURSOR_SHAPES: a consumer that asks again by content id (a late
        // joiner, an evicted decoder cache) gets the sprite without the host
        // decoding it again.
        detail::cursor_shape_cache().put(cursor_);
    }
}

void DxgiCapturer::clear_public_state() {
    frame_.texture = nullptr;
    frame_.shared_handle = nullptr;
    frame_.desktop_updated = true;
    frame_.nv12_texture = nullptr;
    frame_.nv12_shared_handle = nullptr;
    frame_.nv12_surface_width = 0;
    frame_.nv12_surface_height = 0;
    frame_.nv12_updated = false;
    cursor_ = {};
    published_content_id_ = 0;
    frame_dirty_ = true;
    frame_number_ = 0;
}

Error DxgiCapturer::reinitialize() {
    release_frame();
    duplication_.reset();
    output_.reset();
    output1_.reset();
    hdr_pass_.reset();
    nv12_pass_.reset();
    rotation_pass_ = detail::RotationPass{};
    cursor_decoder_.reset();
    color_monitor_.reset();
    clear_public_state();
    last_error_ = Error::success();

    // Re-enumerate before recreating: a reinit usually means the topology
    // changed, so the cached size/rotation/LUID may be stale.
    for (const auto& candidate : enumerate_displays()) {
        if (candidate.device_name == display_.device_name) {
            display_ = candidate;
            break;
        }
    }

    device_.reset();
    context_.reset();
    // initialize() reads last_error_ through note() for non-fatal problems, so
    // a stale message from the previous device is cleared above.
    Error err = initialize();
    if (!err.ok()) {
        last_error_ = err;
    }
    return err;
}

}  // namespace

bool duplication_supported() {
    // DuplicateOutput needs an attached output on an adapter DXGI can see;
    // there is no other precondition the process can check cheaply. Sessions
    // without an interactive desktop (RDP, service sessions) have no attached
    // output, or fail at duplication time.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.put())))) {
        return false;
    }
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, adapter.put()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!adapter) {
            break;
        }
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, output.put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (!output) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                return true;
            }
        }
    }
    return false;
}

std::unique_ptr<IDxgiCapturer> create_capturer(size_t display_index, const CaptureConfig& config,
                                               std::string* error) {
    std::vector<DisplayInfo> displays = enumerate_displays();
    if (displays.empty()) {
        if (error) {
            *error = "no attached display";
        }
        return nullptr;
    }
    if (display_index >= displays.size()) {
        display_index = 0;
    }
    return create_capturer(displays[display_index], config, error);
}

std::unique_ptr<IDxgiCapturer> create_capturer(const DisplayInfo& display,
                                               const CaptureConfig& config, std::string* error) {
    auto capturer = std::make_unique<DxgiCapturer>(display, config);
    Error err = capturer->initialize();
    if (!err.ok()) {
        if (error) {
            *error = err.to_string();
        }
        return nullptr;
    }
    return capturer;
}

}  // namespace chast
