// Merged multi-display capturer.
//
// The picture is the union of every member's virtual-desktop rect. Each member
// is still duplicated by its own DxgiCapturer, but all of them run on one
// D3D11 device and immediate context: a frame is copied into the shared canvas
// with CopySubresourceRegion, so the compositor needs no keyed mutex and no
// copy through system memory. A single NV12 pass converts the canvas for the
// encoder, which keeps the merged view on the same one-blit-per-frame budget as
// a single display.
//
// A member is polled with a zero timeout before the first one blocks for the
// caller's timeout. A display that is producing frames therefore never delays
// the others, and an idle desktop still waits efficiently. Child capturers run
// without NV12 (the canvas owns the only conversion); their cursor decoding is
// kept on so the system pointer's shape comes from the display under it.

// windows.h's min/max macros mangle std::min/std::max in template contexts.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/detail.h"
#include "chast/capture.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace chast {
namespace {

// The encoded picture is capped so a wall of 4K panels still fits what the
// hardware encoders and the protocol accept. The cap scales the whole canvas
// uniformly; adaptive resolution can shrink it further, never grow past it.
constexpr UINT kMergedMaxLongSide = 4096;
constexpr UINT kMergedMaxArea = 4096u * 2304u;
// A D3D11 texture has a hard dimension limit. Beyond this the union is not a
// usable desktop picture and the host falls back to a single display.
constexpr int64_t kMergedHardMaxSide = 16384;

UINT scaled_even(UINT value, UINT percent) {
    const UINT scaled = static_cast<UINT>((static_cast<uint64_t>(value) * percent + 50) / 100);
    return std::max<UINT>(2, scaled) & ~1u;
}

class MergedCapturer final : public IDxgiCapturer {
public:
    MergedCapturer(std::vector<DisplayInfo> displays, CaptureConfig config)
        : displays_(std::move(displays)), config_(config) {}

    Error initialize();
    CaptureResult capture(std::chrono::milliseconds timeout) override;
    const FrameInfo& frame() const override { return frame_; }
    const CursorInfo& cursor() const override { return cursor_; }
    const DisplayInfo& display() const override { return display_; }
    AdapterDevice adapter_device() const override {
        return AdapterDevice{device_.get(), vendor_id_, adapter_luid_};
    }
    Error reinitialize() override;
    Error set_nv12_scale(UINT percent) override;
    bool merged() const override { return true; }
    size_t merged_display_count() const override { return children_.size(); }
    UINT nv12_scale_percent() const override;
    const Error& last_error() const override { return last_error_; }

private:
    struct Child {
        DisplayInfo display;
        std::unique_ptr<IDxgiCapturer> capturer;
        LONG offset_x = 0;
        LONG offset_y = 0;
        // The first frame after (re)creation must be copied even if DXGI calls
        // it pointer-only; the canvas has nothing for this display yet.
        bool needs_copy = true;
    };

    Error create_canvas();
    Error build_children();
    Error create_nv12_pass();
    // Copies the child's newest frame into the canvas. False when the frame no
    // longer matches the layout (mode change), which the host handles by
    // reinitializing the compositor.
    bool composite(Child& child, const FrameInfo& frame);
    CaptureResult handle_child(size_t index, CaptureResult result, bool& active,
                               bool& composited);
    // Fills cursor_ from the system pointer and the child under it. Returns
    // true when visibility, position or shape changed.
    bool pump_cursor();
    void publish_common(bool desktop_updated);
    void release_nv12();
    void clear_public_state();
    UINT auto_percent() const;
    UINT effective_percent(UINT requested) const;
    void note(const Error& err);

    std::vector<DisplayInfo> displays_;
    CaptureConfig config_;
    std::vector<Child> children_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> canvas_;
    ComPtr<ID3D11RenderTargetView> canvas_rtv_;
    HANDLE canvas_handle_ = nullptr;
    std::unique_ptr<detail::Nv12Pass> nv12_pass_;

    DisplayInfo display_;  // synthetic union description
    FrameInfo frame_;
    CursorInfo cursor_;
    Error last_error_ = Error::success();

    LUID adapter_luid_{};
    UINT vendor_id_ = 0;
    LONG union_left_ = 0;
    LONG union_top_ = 0;
    UINT canvas_width_ = 0;
    UINT canvas_height_ = 0;
    // Requested NV12 scale; nv12_scale_percent() reports what the cap allows.
    UINT nv12_percent_ = 100;
    // Scale the live pass was built with; 0 when there is no pass.
    UINT active_percent_ = 0;
    uint64_t frame_number_ = 0;
    uint64_t cursor_revision_ = 0;
    uint64_t published_content_id_ = 0;
    // The canvas must be reconverted even without a desktop update (startup,
    // scale change).
    bool frame_dirty_ = true;
};

Error MergedCapturer::initialize() {
    if (displays_.empty()) {
        return Error(Status::InvalidCall, "a merged view needs at least one display");
    }
    for (size_t i = 1; i < displays_.size(); ++i) {
        const LUID& a = displays_[0].adapter_luid;
        const LUID& b = displays_[i].adapter_luid;
        if (a.LowPart != b.LowPart || a.HighPart != b.HighPart) {
            return Error(Status::Unsupported,
                         "merged view needs every display on one adapter: " +
                             detail::to_utf8(displays_[0].device_name) + " and " +
                             detail::to_utf8(displays_[i].device_name) + " differ");
        }
    }

    LONG left = displays_[0].rect.left;
    LONG top = displays_[0].rect.top;
    LONG right = displays_[0].rect.right;
    LONG bottom = displays_[0].rect.bottom;
    for (const DisplayInfo& display : displays_) {
        if (display.rect.right <= display.rect.left || display.rect.bottom <= display.rect.top) {
            return Error(Status::InvalidCall,
                         "display " + detail::to_utf8(display.device_name) + " has an empty rect");
        }
        left = std::min(left, display.rect.left);
        top = std::min(top, display.rect.top);
        right = std::max(right, display.rect.right);
        bottom = std::max(bottom, display.rect.bottom);
    }
    const int64_t width = static_cast<int64_t>(right) - left;
    const int64_t height = static_cast<int64_t>(bottom) - top;
    if (width <= 0 || height <= 0 || width > kMergedHardMaxSide ||
        height > kMergedHardMaxSide || width * height > kMergedHardMaxSide * 4096) {
        return Error(Status::Unsupported,
                     "the display union is " + std::to_string(width) + "x" +
                         std::to_string(height) + ", too large for one picture");
    }

    union_left_ = left;
    union_top_ = top;
    // NV12 subsamples 2x2, so the canvas itself is padded to an even size.
    canvas_width_ = static_cast<UINT>((width + 1) & ~1);
    canvas_height_ = static_cast<UINT>((height + 1) & ~1);
    adapter_luid_ = displays_[0].adapter_luid;

    detail::CaptureDevice shared;
    Error err = detail::create_capture_device(adapter_luid_, shared);
    if (!err.ok()) {
        return err;
    }
    device_ = std::move(shared.device);
    context_ = std::move(shared.context);
    vendor_id_ = shared.vendor_id;

    err = create_canvas();
    if (!err.ok()) {
        return err;
    }
    err = build_children();
    if (!err.ok()) {
        return err;
    }

    // The host encodes the NV12 picture; a canvas without it would only fail
    // much later as a frame timeout, so report the real reason here.
    const Error nv12 = create_nv12_pass();
    if (!nv12.ok()) {
        return nv12;
    }

    display_ = displays_[0];
    display_.device_name = L"(all displays)";
    display_.rect.left = left;
    display_.rect.top = top;
    display_.rect.right = right;
    display_.rect.bottom = bottom;
    display_.width = canvas_width_;
    display_.height = canvas_height_;
    display_.rotation_degrees = 0;
    display_.primary = false;
    for (const DisplayInfo& member : displays_) {
        display_.primary = display_.primary || member.primary;
    }

    frame_.device = device_.get();
    frame_.context = context_.get();
    frame_.adapter_luid = adapter_luid_;
    frame_.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame_.rotation_degrees = 0;
    frame_.desktop_updated = true;
    frame_.texture = canvas_.get();
    frame_.shared_handle = canvas_handle_;
    frame_.width = canvas_width_;
    frame_.height = canvas_height_;
    frame_number_ = 0;
    frame_dirty_ = true;
    pump_cursor();
    return Error::success();
}

Error MergedCapturer::create_canvas() {
    canvas_.reset();
    canvas_rtv_.reset();
    canvas_handle_ = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = canvas_width_;
    desc.Height = canvas_height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // SHADER_RESOURCE lets a consumer inspect the canvas on the device; the
    // render-target flag is what ClearRenderTargetView needs for the gaps.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    Error err = check(device_->CreateTexture2D(&desc, nullptr, canvas_.put()),
                      "CreateTexture2D(merged canvas)");
    if (!err.ok()) {
        return err;
    }
    ComPtr<IDXGIResource> resource = com_query<IDXGIResource>(canvas_);
    if (!resource || FAILED(resource->GetSharedHandle(&canvas_handle_))) {
        return Error(Status::CaptureFailed, "the merged canvas has no shared handle");
    }
    err = check(device_->CreateRenderTargetView(canvas_.get(), nullptr, canvas_rtv_.put()),
                "CreateRenderTargetView(merged canvas)");
    if (!err.ok()) {
        return err;
    }
    const FLOAT black[4] = {0.f, 0.f, 0.f, 1.f};
    context_->ClearRenderTargetView(canvas_rtv_.get(), black);
    context_->Flush();
    return Error::success();
}

Error MergedCapturer::build_children() {
    // The canvas owns the only BGRA -> NV12 conversion. Cursor decoding stays
    // on: the system pointer's sprite is whichever member is under it.
    CaptureConfig child_config = config_;
    child_config.nv12.enabled = false;
    child_config.capture_cursor = true;

    children_.clear();
    children_.reserve(displays_.size());
    for (const DisplayInfo& member : displays_) {
        std::string child_error;
        std::unique_ptr<IDxgiCapturer> capturer =
            detail::create_capturer_on_device(member, child_config, device_.get(), context_.get(),
                                              vendor_id_, &child_error);
        if (!capturer) {
            return Error(Status::CaptureFailed, "cannot capture " +
                                                    detail::to_utf8(member.device_name) + ": " +
                                                    child_error);
        }
        Child child;
        child.display = member;
        child.offset_x = member.rect.left - union_left_;
        child.offset_y = member.rect.top - union_top_;
        child.capturer = std::move(capturer);
        children_.push_back(std::move(child));
    }
    return Error::success();
}

Error MergedCapturer::create_nv12_pass() {
    release_nv12();
    nv12_pass_.reset();
    active_percent_ = 0;
    if (!config_.nv12.enabled) {
        return Error::success();
    }
    const UINT percent = effective_percent(nv12_percent_);
    const UINT width = scaled_even(canvas_width_, percent);
    const UINT height = scaled_even(canvas_height_, percent);
    auto pass = std::make_unique<detail::Nv12Pass>();
    const Error err = pass->initialize(device_.get(), context_.get(), canvas_width_,
                                       canvas_height_, width, height, config_.nv12.colorimetry);
    if (!err.ok()) {
        return err;
    }
    nv12_pass_ = std::move(pass);
    active_percent_ = percent;
    return Error::success();
}

UINT MergedCapturer::auto_percent() const {
    if (canvas_width_ == 0 || canvas_height_ == 0) {
        return 100;
    }
    UINT percent = 100;
    if (canvas_width_ > kMergedMaxLongSide) {
        percent = std::min<UINT>(
            percent, static_cast<UINT>(static_cast<uint64_t>(kMergedMaxLongSide) * 100 /
                                       canvas_width_));
    }
    if (canvas_height_ > kMergedMaxLongSide) {
        percent = std::min<UINT>(
            percent, static_cast<UINT>(static_cast<uint64_t>(kMergedMaxLongSide) * 100 /
                                       canvas_height_));
    }
    // The rounded-to-even dimensions make the analytic bound approximate, so
    // settle it against the dimensions the pass will really use.
    while (percent > 1) {
        const uint64_t width = scaled_even(canvas_width_, percent);
        const uint64_t height = scaled_even(canvas_height_, percent);
        if (width <= kMergedMaxLongSide && height <= kMergedMaxLongSide &&
            width * height <= kMergedMaxArea) {
            break;
        }
        --percent;
    }
    return std::max<UINT>(1, percent);
}

UINT MergedCapturer::effective_percent(UINT requested) const {
    const UINT requested_clamped = std::max<UINT>(1, std::min<UINT>(100, requested));
    return std::min(requested_clamped, auto_percent());
}

UINT MergedCapturer::nv12_scale_percent() const {
    if (active_percent_ != 0) {
        return active_percent_;
    }
    return effective_percent(nv12_percent_);
}

Error MergedCapturer::set_nv12_scale(UINT percent) {
    if (percent == 0 || percent > 100) {
        return Error(Status::InvalidCall, "NV12 scale is a percentage from 1 to 100");
    }
    if (percent == nv12_percent_) {
        return Error::success();
    }
    if (!config_.nv12.enabled) {
        nv12_percent_ = percent;
        return Error::success();
    }

    const UINT previous = nv12_percent_;
    nv12_percent_ = percent;
    const Error err = create_nv12_pass();
    if (!err.ok()) {
        // Keep the scale that was working rather than leaving no NV12 at all.
        nv12_percent_ = previous;
        const Error reverted = create_nv12_pass();
        if (!reverted.ok()) {
            note(reverted);
        }
    }
    // The rebuilt pass starts with an empty target, so the next capture must
    // reconvert the canvas even if no display changed.
    frame_dirty_ = true;
    return err;
}

bool MergedCapturer::composite(Child& child, const FrameInfo& frame) {
    if (frame.width != child.display.width || frame.height != child.display.height) {
        // The mode or rotation changed under the child; the whole layout has
        // to be recomputed (and re-announced) rather than copied askew.
        return false;
    }
    if (child.offset_x < 0 || child.offset_y < 0 ||
        static_cast<uint64_t>(child.offset_x) + frame.width > canvas_width_ ||
        static_cast<uint64_t>(child.offset_y) + frame.height > canvas_height_) {
        return false;
    }
    const D3D11_BOX box{0, 0, 0, frame.width, frame.height, 1};
    context_->CopySubresourceRegion(canvas_.get(), 0, static_cast<UINT>(child.offset_x),
                                    static_cast<UINT>(child.offset_y), 0, frame.texture, 0, &box);
    return true;
}

CaptureResult MergedCapturer::handle_child(size_t index, CaptureResult result, bool& active,
                                           bool& composited) {
    Child& child = children_[index];
    switch (result) {
        case CaptureResult::Frame: {
            active = true;
            const FrameInfo& captured = child.capturer->frame();
            const std::string name = detail::to_utf8(child.display.device_name);
            if (captured.rotation_degrees != 0) {
                last_error_ = Error(Status::Unsupported,
                                    name + " needs a CPU rotation, which a merged view cannot do");
                return CaptureResult::FatalError;
            }
            if (!captured.texture || captured.format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                last_error_ =
                    Error(Status::CaptureFailed, name + " produced no BGRA8 frame");
                return CaptureResult::FatalError;
            }
            if (child.needs_copy || captured.desktop_updated) {
                if (!composite(child, captured)) {
                    last_error_ = Error(Status::AccessLost,
                                        name + " changed size; the merged layout is stale");
                    return CaptureResult::ReinitNeeded;
                }
                child.needs_copy = false;
                composited = true;
            }
            return CaptureResult::Frame;
        }
        case CaptureResult::NoNewFrame:
            return CaptureResult::NoNewFrame;
        case CaptureResult::ReinitNeeded:
            return CaptureResult::ReinitNeeded;
        case CaptureResult::FatalError:
            last_error_ = child.capturer->last_error();
            if (last_error_.ok()) {
                last_error_ = Error(Status::CaptureFailed,
                                    detail::to_utf8(child.display.device_name) + " stopped");
            }
            return CaptureResult::FatalError;
    }
    return CaptureResult::FatalError;
}

CaptureResult MergedCapturer::capture(std::chrono::milliseconds timeout) {
    if (children_.empty()) {
        last_error_ = Error(Status::InvalidCall, "the merged capturer has no children");
        return CaptureResult::FatalError;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool active = false;
    bool composited = false;

    // Poll everything first so a busy display cannot hide the others' updates
    // behind the first display's blocking wait.
    for (size_t i = 0; i < children_.size(); ++i) {
        const CaptureResult result =
            children_[i].capturer->capture(std::chrono::milliseconds(0));
        const CaptureResult outcome = handle_child(i, result, active, composited);
        if (outcome == CaptureResult::ReinitNeeded || outcome == CaptureResult::FatalError) {
            return outcome;
        }
    }
    if (!active) {
        std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining.count() < 0) {
            remaining = std::chrono::milliseconds(0);
        }
        const CaptureResult result = children_.front().capturer->capture(remaining);
        const CaptureResult outcome = handle_child(0, result, active, composited);
        if (outcome == CaptureResult::ReinitNeeded || outcome == CaptureResult::FatalError) {
            return outcome;
        }
    }

    const bool cursor_changed = pump_cursor();
    if (!active && !cursor_changed && !frame_dirty_) {
        return CaptureResult::NoNewFrame;
    }

    frame_.nv12_updated = false;
    if (nv12_pass_ && (composited || frame_dirty_)) {
        ID3D11Texture2D* nv12 = nullptr;
        const Error err = nv12_pass_->convert(canvas_.get(), &nv12);
        if (!err.ok()) {
            note(err);
            if (detail::stage_failure_is_permanent(err.status())) {
                nv12_pass_.reset();
                active_percent_ = 0;
            }
            release_nv12();
        } else {
            frame_.nv12_texture = nv12;
            frame_.nv12_shared_handle = nv12_pass_->shared_handle();
            frame_.nv12_width = nv12_pass_->width();
            frame_.nv12_height = nv12_pass_->height();
            frame_.nv12_surface_width = nv12_pass_->surface_width();
            frame_.nv12_surface_height = nv12_pass_->surface_height();
            frame_.nv12_updated = true;
        }
    }
    publish_common(composited);
    return CaptureResult::Frame;
}

bool MergedCapturer::pump_cursor() {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    POINT screen{};
    const bool have_position = GetCursorPos(&screen) != FALSE;
    const bool system_visible =
        GetCursorInfo(&info) != FALSE && (info.flags & CURSOR_SHOWING) != 0;

    size_t under = children_.size();
    bool inside = false;
    if (have_position) {
        for (size_t i = 0; i < children_.size(); ++i) {
            const DisplayRect& rect = children_[i].display.rect;
            if (screen.x >= rect.left && screen.x < rect.right && screen.y >= rect.top &&
                screen.y < rect.bottom) {
                under = i;
                inside = true;
                break;
            }
        }
    }

    const bool was_visible = cursor_.visible;
    const bool was_position_valid = cursor_.position_valid;
    const POINT old_position = cursor_.position;
    const uint64_t old_content = cursor_.content_id;

    cursor_.visible = have_position && system_visible && inside;
    cursor_.position_valid = have_position;
    if (have_position) {
        cursor_.position.x = screen.x - union_left_;
        cursor_.position.y = screen.y - union_top_;
    }

    if (inside) {
        const CursorInfo& source = children_[under].capturer->cursor();
        if (source.shape_valid) {
            cursor_.shape_valid = true;
            cursor_.shape_type = source.shape_type;
            cursor_.width = source.width;
            cursor_.height = source.height;
            cursor_.pitch = source.pitch;
            cursor_.origin_offset = source.origin_offset;
            cursor_.hotspot = source.hotspot;
            cursor_.content_id = source.content_id;
            if (cursor_.content_id != published_content_id_ ||
                cursor_.bitmap.empty()) {
                cursor_.bitmap = source.bitmap;
                cursor_.invert = source.invert;
                cursor_.outlined = source.outlined;
                published_content_id_ = cursor_.content_id;
                detail::cursor_shape_cache().put(cursor_);
            }
        }
    }

    const bool changed = cursor_.visible != was_visible ||
                         cursor_.position_valid != was_position_valid ||
                         cursor_.position.x != old_position.x ||
                         cursor_.position.y != old_position.y ||
                         cursor_.content_id != old_content;
    if (changed) {
        cursor_.revision = ++cursor_revision_;
    }
    return changed;
}

void MergedCapturer::publish_common(bool desktop_updated) {
    frame_.texture = canvas_.get();
    frame_.shared_handle = canvas_handle_;
    frame_.width = canvas_width_;
    frame_.height = canvas_height_;
    frame_.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame_.desktop_updated = desktop_updated;
    frame_.frame_number = ++frame_number_;
    frame_dirty_ = false;
}

void MergedCapturer::release_nv12() {
    frame_.nv12_texture = nullptr;
    frame_.nv12_shared_handle = nullptr;
    frame_.nv12_width = 0;
    frame_.nv12_height = 0;
    frame_.nv12_surface_width = 0;
    frame_.nv12_surface_height = 0;
    frame_.nv12_updated = false;
}

void MergedCapturer::clear_public_state() {
    frame_.texture = nullptr;
    frame_.shared_handle = nullptr;
    frame_.desktop_updated = true;
    release_nv12();
    cursor_ = {};
    published_content_id_ = 0;
    cursor_revision_ = 0;
    frame_number_ = 0;
    frame_dirty_ = true;
}

Error MergedCapturer::reinitialize() {
    last_error_ = Error::success();
    clear_public_state();
    children_.clear();
    nv12_pass_.reset();
    active_percent_ = 0;
    canvas_rtv_.reset();
    canvas_.reset();
    canvas_handle_ = nullptr;
    device_.reset();
    context_.reset();

    // Re-read the topology: a hot-plugged display joins the merged view, an
    // unplugged one leaves it. Only outputs on the compositor's adapter can
    // take part; the rest were refused at creation and stay out.
    std::vector<DisplayInfo> attached = enumerate_displays();
    displays_.clear();
    for (DisplayInfo& display : attached) {
        if (display.adapter_luid.LowPart == adapter_luid_.LowPart &&
            display.adapter_luid.HighPart == adapter_luid_.HighPart) {
            displays_.push_back(std::move(display));
        }
    }
    if (displays_.empty()) {
        last_error_ = Error(Status::Unavailable, "no display remains on the merged adapter");
        return last_error_;
    }

    const Error err = initialize();
    if (!err.ok()) {
        last_error_ = err;
    }
    return err;
}

void MergedCapturer::note(const Error& err) {
    if (!err.ok() && last_error_.ok()) {
        last_error_ = err;
    }
}

}  // namespace

std::unique_ptr<IDxgiCapturer> create_merged_capturer(const std::vector<DisplayInfo>& displays,
                                                      const CaptureConfig& config,
                                                      std::string* error) {
    if (displays.empty()) {
        if (error) {
            *error = "no attached display";
        }
        return nullptr;
    }
    auto capturer = std::make_unique<MergedCapturer>(displays, config);
    const Error err = capturer->initialize();
    if (!err.ok()) {
        if (error) {
            *error = err.to_string();
        }
        return nullptr;
    }
    return capturer;
}

}  // namespace chast
