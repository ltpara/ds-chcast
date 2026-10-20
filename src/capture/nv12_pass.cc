// BGRA8 -> NV12 on the GPU, through the D3D11 video processor.
//
// This mirrors RustDesk's hwcodec NativeDevice::BgraToNv12 / Process, which is
// what its VRAM encoder calls before handing a texture to h264_nvenc,
// hevc_amf, h264_qsv and friends:
//
//   * a video processor enumerator built for the frame size, with
//     auto-processing off and progressive frame format, because the default
//     auto-processing brightens or darkens the image and ignores the color
//     space request;
//   * the stream color space set to RGB_FULL_G22_NONE_P709 and the output to
//     one of the YCbCr spaces, all through ID3D11VideoContext1 (the
//     ID3D11VideoContext color-space methods are the older DXVA2 enum set);
//   * source and destination rects covering the whole frame;
//   * one VideoProcessorBlt into the NV12 render target.
//
// RustDesk hardcodes BT.601 limited output (bt709_ = false, full_range_ =
// false), so that is the default here too. Unlike RustDesk there is no libyuv
// path: the texture never leaves the GPU, which is the whole point.

#include "capture/detail.h"

namespace chast::detail {

namespace {

// RustDesk's two hardcoded knobs, expanded into DXGI's four YCbCr spaces.
DXGI_COLOR_SPACE_TYPE output_space_for(Nv12Colorimetry colorimetry) {
    switch (colorimetry) {
        case Nv12Colorimetry::Bt601Full: return DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601;
        case Nv12Colorimetry::Bt709Limited: return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        case Nv12Colorimetry::Bt709Full: return DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
        case Nv12Colorimetry::Bt601Limited:
        default: return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    }
}

// oneVPL requires 16-pixel alignment for video-memory surfaces, so the NV12
// target is padded up to that. The padding is never part of the image: the
// real frame size stays separately reported, and a hardware encoder takes it
// as its crop rectangle.
UINT align16(UINT value) {
    return (value + 15u) & ~15u;
}

}  // namespace

Error Nv12Pass::initialize(ID3D11Device* device, ID3D11DeviceContext* context, UINT width,
                           UINT height, Nv12Colorimetry colorimetry) {
    if (!device || !context) {
        return Error(Status::InvalidCall, "NV12 pass needs a device and context");
    }
    if (width == 0 || height == 0) {
        return Error(Status::InvalidCall, "NV12 pass needs a non-empty frame");
    }

    // These belong to the capturer: take a reference instead of adopting one.
    device_ = ComPtr<ID3D11Device>(device);
    device_.add_ref();
    context_ = ComPtr<ID3D11DeviceContext>(context);
    context_.add_ref();
    width_ = width;
    height_ = height;
    surface_width_ = align16(width);
    surface_height_ = align16(height);
    input_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    output_space_ = output_space_for(colorimetry);

    video_device_ = com_query<ID3D11VideoDevice>(device);
    video_context_ = com_query<ID3D11VideoContext>(context);
    if (!video_device_ || !video_context_) {
        return Error(Status::Unsupported, "device has no D3D11 video interfaces");
    }
    // The color-space setters used by RustDesk live on the context's successor
    // interface. Without it the conversion could only be requested through the
    // DXVA2-era setters, whose semantics differ; leave the pass not-ready.
    video_context1_ = com_query<ID3D11VideoContext1>(context);
    if (!video_context1_) {
        return Error(Status::Unsupported, "device has no ID3D11VideoContext1");
    }

    // Same content description RustDesk builds for the conversion. The
    // dimensions must be even: NV12 subsamples chroma 2x2, and the video
    // processor refuses odd sizes.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = DXGI_RATIONAL{30, 1};
    content.OutputFrameRate = DXGI_RATIONAL{30, 1};
    content.InputWidth = surface_width_;
    content.InputHeight = surface_height_;
    content.OutputWidth = surface_width_;
    content.OutputHeight = surface_height_;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    Error err = check(video_device_->CreateVideoProcessorEnumerator(&content, enumerator_.put()),
                      "CreateVideoProcessorEnumerator(NV12)");
    if (!err.ok()) {
        return err;
    }
    err = check(video_device_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()),
                "CreateVideoProcessor(NV12)");
    if (!err.ok()) {
        enumerator_.reset();
        return err;
    }

    // Auto-processing would apply the processor's own color adjustments and
    // override the color spaces set below; RustDesk turns it off for exactly
    // that reason ("too dark or too light").
    video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
    video_context_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0,
                                                       D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context1_->VideoProcessorSetStreamColorSpace1(processor_.get(), 0, input_space_);
    video_context1_->VideoProcessorSetOutputColorSpace1(processor_.get(), output_space_);

    // The target is 16-aligned but the image is not, so the padding around the
    // destination rect has to be filled deterministically: the encoder never
    // reads it (its crop excludes it), but leaving stale pixels there makes
    // dumped textures confusing to compare. Black is YCbCr (16, 128, 128) for
    // limited range.
    D3D11_VIDEO_COLOR black{};
    black.YCbCr.Y = 16.f / 255.f;
    black.YCbCr.Cb = 128.f / 255.f;
    black.YCbCr.Cr = 128.f / 255.f;
    black.YCbCr.A = 1.f;
    video_context_->VideoProcessorSetOutputBackgroundColor(processor_.get(), TRUE, &black);

    // Source and dest are the real frame, not the padded surface: the extra
    // rows and columns are never part of the image.
    const RECT rect{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    video_context_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &rect);
    video_context1_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &rect);
    return Error::success();
}

Error Nv12Pass::convert(ID3D11Texture2D* source, ID3D11Texture2D** out) {
    *out = nullptr;
    if (!ready()) {
        return Error(Status::Unsupported, "NV12 pass is not ready");
    }
    if (!source) {
        return Error(Status::InvalidCall, "NV12 pass got a null source texture");
    }

    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    if (source_desc.Width < surface_width_ || source_desc.Height < surface_height_) {
        return Error(Status::InvalidCall, "NV12 source is smaller than the pass frame");
    }
    Error err = ensure_target();
    if (!err.ok()) {
        return err;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_view_desc{};
    in_view_desc.FourCC = 0;
    in_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    in_view_desc.Texture2D.MipSlice = 0;
    in_view_desc.Texture2D.ArraySlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> in_view;
    err = check(video_device_->CreateVideoProcessorInputView(
                    source, enumerator_.get(), &in_view_desc, in_view.put()),
                "CreateVideoProcessorInputView(NV12)");
    if (!err.ok()) {
        return err;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view_desc{};
    out_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_view_desc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11VideoProcessorOutputView> out_view;
    err = check(video_device_->CreateVideoProcessorOutputView(
                    target_.get(), enumerator_.get(), &out_view_desc, out_view.put()),
                "CreateVideoProcessorOutputView(NV12)");
    if (!err.ok()) {
        return err;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in_view.get();

    err = check(video_context_->VideoProcessorBlt(processor_.get(), out_view.get(), 0, 1, &stream),
                "VideoProcessorBlt(NV12)");
    if (!err.ok()) {
        return err;
    }
    *out = target_.get();
    return Error::success();
}

HANDLE Nv12Pass::shared_handle() const {
    if (!target_) {
        return nullptr;
    }
    ComPtr<IDXGIResource> resource = com_query<IDXGIResource>(target_);
    HANDLE handle = nullptr;
    if (resource && FAILED(resource->GetSharedHandle(&handle))) {
        return nullptr;
    }
    return handle;
}

Error Nv12Pass::ensure_target() {
    if (target_) {
        return Error::success();
    }

    // D3D11_BIND_RENDER_TARGET is what the video processor writes to. The
    // video bindings are what make the texture a legal shared surface for
    // oneVPL's import: the runtime mirrors the D3D11 desc of the texture it is
    // handed, and a texture without the codec bindings (Intel's own sharing
    // example uses DECODER | VIDEO_ENCODER | SHADER_RESOURCE) is refused at
    // import time.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = surface_width_;
    desc.Height = surface_height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER |
                     D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ComPtr<ID3D11Texture2D> target;
    Error err = check(device_->CreateTexture2D(&desc, nullptr, target.put()),
                      "CreateTexture2D(NV12)");
    if (!err.ok()) {
        return err;
    }
    target_ = std::move(target);
    return Error::success();
}

}  // namespace chast::detail
