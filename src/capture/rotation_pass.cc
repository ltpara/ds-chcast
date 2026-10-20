#include "capture/detail.h"
#include "capture/rotation.h"

namespace chast::detail {

namespace {

D3D11_VIDEO_PROCESSOR_ROTATION to_processor_rotation(DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: return D3D11_VIDEO_PROCESSOR_ROTATION_90;
        case DXGI_MODE_ROTATION_ROTATE180: return D3D11_VIDEO_PROCESSOR_ROTATION_180;
        case DXGI_MODE_ROTATION_ROTATE270: return D3D11_VIDEO_PROCESSOR_ROTATION_270;
        default: return D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    }
}

}  // namespace

Error RotationPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                               DXGI_MODE_ROTATION rotation, UINT input_width,
                               UINT input_height, UINT output_width, UINT output_height) {
    if (!device || !context) {
        return Error(Status::InvalidCall, "rotation pass needs a device and context");
    }
    if (rotation == DXGI_MODE_ROTATION_IDENTITY || rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
        return Error::success();  // ready() stays false; the caller passes frames through
    }

    // These belong to the capturer: take a reference instead of adopting one.
    // (attach() would release a reference the capturer still holds, and the
    // pass outliving its last use would then free the device under it.)
    device_ = ComPtr<ID3D11Device>(device);
    device_.add_ref();
    context_ = ComPtr<ID3D11DeviceContext>(context);
    context_.add_ref();
    rotation_ = to_processor_rotation(rotation);
    input_width_ = input_width;
    input_height_ = input_height;
    output_width_ = output_width;
    output_height_ = output_height;

    // The video processor lives on the D3D11 device itself; DXVA2 is not
    // involved and no hardware decoder is required. Devices without the
    // feature simply report no rotation caps, which is why this is not fatal.
    video_device_ = com_query<ID3D11VideoDevice>(device);
    video_context_ = com_query<ID3D11VideoContext>(context);
    if (!video_device_ || !video_context_) {
        return Error(Status::Unsupported, "device has no D3D11 video interfaces");
    }

    // Input is always the raw desktop, whose width/height are already swapped
    // for 90/270 by the caller, so the content desc uses duplication size.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = DXGI_RATIONAL{60, 1};
    content.OutputFrameRate = DXGI_RATIONAL{60, 1};
    content.InputWidth = input_width;
    content.InputHeight = input_height;
    content.OutputWidth = output_width;
    content.OutputHeight = output_height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    Error err = check(video_device_->CreateVideoProcessorEnumerator(&content, enumerator_.put()),
                      "CreateVideoProcessorEnumerator");
    if (!err.ok()) {
        return err;
    }

    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    err = check(enumerator_->GetVideoProcessorCaps(&caps), "GetVideoProcessorCaps");
    if (!err.ok()) {
        return err;
    }
    if ((caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) == 0) {
        enumerator_.reset();
        return Error(Status::Unsupported, "video processor has no rotation capability");
    }

    err = check(video_device_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()),
                "CreateVideoProcessor");
    if (!err.ok()) {
        return err;
    }

    // Rotation happens on the stream; source/dest rects are in the
    // unrotated/rotated frames respectively, and both cover the whole surface.
    video_context_->VideoProcessorSetStreamRotation(processor_.get(), 0, TRUE, rotation_);
    video_context_->VideoProcessorSetStreamFrameFormat(
        processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);
    const RECT source_rect{0, 0, static_cast<LONG>(input_width),
                           static_cast<LONG>(input_height)};
    const RECT dest_rect{0, 0, static_cast<LONG>(output_width), static_cast<LONG>(output_height)};
    video_context_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &source_rect);
    video_context_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &dest_rect);
    return Error::success();
}

Error RotationPass::rotate(ID3D11Texture2D* source, ID3D11Texture2D** out) {
    *out = nullptr;
    if (!ready()) {
        return Error(Status::Unsupported, "rotation pass is not ready");
    }
    if (!source) {
        return Error(Status::InvalidCall, "rotation pass got a null source texture");
    }

    // The rotated texture must match the duplication format (FP16 for HDR) so
    // the HDR pass can consume it; it is shared for encoder import.
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    Error err = ensure_target(desc);
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
                "CreateVideoProcessorInputView");
    if (!err.ok()) {
        return err;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_view_desc{};
    out_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    out_view_desc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11VideoProcessorOutputView> out_view;
    err = check(video_device_->CreateVideoProcessorOutputView(
                    target_.get(), enumerator_.get(), &out_view_desc, out_view.put()),
                "CreateVideoProcessorOutputView");
    if (!err.ok()) {
        return err;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in_view.get();

    err = check(video_context_->VideoProcessorBlt(processor_.get(), out_view.get(), 0, 1, &stream),
                "VideoProcessorBlt(rotation)");
    if (!err.ok()) {
        return err;
    }
    *out = target_.get();
    return Error::success();
}

HANDLE RotationPass::shared_handle() const {
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

Error RotationPass::ensure_target(const D3D11_TEXTURE2D_DESC& source_desc) {
    if (target_ && target_width_ == output_width_ && target_height_ == output_height_ &&
        target_format_ == source_desc.Format) {
        return Error::success();
    }

    D3D11_TEXTURE2D_DESC desc = source_desc;
    desc.Width = output_width_;
    desc.Height = output_height_;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ComPtr<ID3D11Texture2D> target;
    Error err = check(device_->CreateTexture2D(&desc, nullptr, target.put()),
                      "CreateTexture2D(rotated)");
    if (!err.ok()) {
        return err;
    }
    target_ = std::move(target);
    target_width_ = output_width_;
    target_height_ = output_height_;
    target_format_ = source_desc.Format;
    return Error::success();
}

}  // namespace chast::detail
