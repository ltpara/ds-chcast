// OpenH264 software encoder fallback.
//
// Capture publishes an NV12 D3D11 texture. OpenH264 accepts I420 system
// memory, so this backend reads the texture through a staging resource and
// de-interleaves its UV plane. It is intentionally the last Auto tier: the
// hardware backends keep frames in VRAM, while this path trades CPU time for a
// stream on machines whose vendor encoder cannot be opened.

#include "chast/encoder.h"

#include <d3d11.h>
#include <wels/codec_api.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chast/com_ptr.h"
#include "encoder_internal.h"

namespace chast {
namespace {

Error openh264_error(int code, const char* what) {
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed with OpenH264 status " +
                     std::to_string(code));
}

UINT clamp_framerate(UINT framerate) {
    // The 2.6 implementation supports 60 fps; codec_app_def.h still says 30.
    return (std::clamp)(framerate == 0 ? UINT{30} : framerate, UINT{1}, UINT{60});
}

UINT clamp_bitrate(UINT bitrate, UINT framerate) {
    // OpenH264 requires at least one bit per frame and a signed int bitrate.
    return (std::clamp)(bitrate, framerate,
                        static_cast<UINT>((std::numeric_limits<int>::max)()));
}

class OpenH264Encoder final : public IVideoEncoder {
public:
    OpenH264Encoder(ID3D11Device* device, const EncoderConfig& config)
        : device_(device), config_(config) {
        if (device_) {
            device_.add_ref();
        }
    }

    ~OpenH264Encoder() override { close(); }

    Error initialize();
    Error encode(ID3D11Texture2D* nv12, uint64_t pts_ms, bool force_keyframe,
                 std::vector<EncodedPacket>& out) override;
    Error encode_repeat(uint64_t pts_ms, bool force_keyframe,
                        std::vector<EncodedPacket>& out) override;
    Error set_rate(UINT bits_per_second, UINT framerate,
                   bool* keyframe_required) override;

    UINT bitrate() const noexcept override { return bitrate_; }
    UINT framerate() const noexcept override { return framerate_; }
    VideoCodec codec() const noexcept override { return VideoCodec::H264; }
    UINT width() const noexcept override { return width_; }
    UINT height() const noexcept override { return height_; }
    const std::string& implementation() const noexcept override {
        return implementation_;
    }
    bool shared_import() const noexcept override { return false; }
    EncoderBackend backend() const noexcept override { return EncoderBackend::Software; }

private:
    Error create_staging_texture();
    Error read_nv12(ID3D11Texture2D* texture);
    Error send(uint64_t pts_ms, bool force_keyframe,
               std::vector<EncodedPacket>& out);
    Error device_lost() const;
    void close();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;
    EncoderConfig config_;
    ISVCEncoder* encoder_ = nullptr;
    std::vector<uint8_t> i420_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT bitrate_ = 0;
    UINT bitrate_limit_ = static_cast<UINT>((std::numeric_limits<int>::max)());
    UINT framerate_ = 30;
    bool have_input_ = false;
    bool force_idr_ = true;
    std::string implementation_;
};

Error OpenH264Encoder::initialize() {
    if (device_.is_null()) {
        return Error(Status::InvalidCall,
                     "OpenH264 encoding needs a D3D11 capture device");
    }
    if (config_.width == 0 || config_.height == 0 || (config_.width & 1) != 0 ||
        (config_.height & 1) != 0 ||
        config_.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        config_.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        return Error(Status::InvalidCall,
                     "OpenH264 needs an even, non-zero frame size within D3D11 limits");
    }

    width_ = config_.width;
    height_ = config_.height;
    framerate_ = clamp_framerate(config_.framerate);
    bitrate_ = config_.bitrate != 0
                   ? config_.bitrate
                   : hardware_bitrate(width_, height_, config_.quality_ratio, true);
    bitrate_ = clamp_bitrate(bitrate_, framerate_);

    device_->GetImmediateContext(context_.put());
    if (context_.is_null()) {
        return Error(Status::CaptureFailed,
                     "D3D11 device returned no immediate context");
    }
    Error staging = create_staging_texture();
    if (!staging.ok()) {
        return staging;
    }

    const size_t luma = static_cast<size_t>(width_) * height_;
    try {
        i420_.resize(luma + luma / 2);
    } catch (const std::bad_alloc&) {
        return Error(Status::CaptureFailed,
                     "could not allocate the OpenH264 I420 frame buffer");
    }

    int status = WelsCreateSVCEncoder(&encoder_);
    if (status != cmResultSuccess || !encoder_) {
        return openh264_error(status, "WelsCreateSVCEncoder");
    }

    SEncParamExt params{};
    status = encoder_->GetDefaultParams(&params);
    if (status != cmResultSuccess) {
        return openh264_error(status, "OpenH264 GetDefaultParams");
    }

    const int target_bitrate = static_cast<int>(bitrate_);
    params.iUsageType = SCREEN_CONTENT_REAL_TIME;
    params.iPicWidth = static_cast<int>(width_);
    params.iPicHeight = static_cast<int>(height_);
    params.iTargetBitrate = target_bitrate;
    // Desktop capture is event-driven, so frames do not arrive at a fixed
    // cadence. Account for their real timestamps when enforcing the target.
    params.iRCMode = RC_TIMESTAMP_MODE;
    params.fMaxFrameRate = static_cast<float>(framerate_);
    params.iTemporalLayerNum = 1;
    params.iSpatialLayerNum = 1;
    params.iComplexityMode = MEDIUM_COMPLEXITY;
    params.uiIntraPeriod = config_.gop_size;
    params.iNumRefFrame = 1;
    params.eSpsPpsIdStrategy = CONSTANT_ID;
    params.bPrefixNalAddingCtrl = false;
    params.bEnableSSEI = false;
    params.bSimulcastAVC = false;
    params.iPaddingFlag = 0;
    params.iEntropyCodingModeFlag = 1;
    // Rate control may skip a frame under pressure. IDR requests are retained
    // until an IDR is actually emitted, including on the repeat path.
    params.bEnableFrameSkip = true;
    params.iMaxBitrate = UNSPECIFIED_BIT_RATE;
    params.bEnableLongTermReference = false;
    params.bEnableDenoise = false;
    params.bEnableBackgroundDetection = false;
    params.bEnableAdaptiveQuant = false;
    // Screen mode requires this and may emit an IDR on a scene change even
    // when periodic keyframes are disabled.
    params.bEnableSceneChangeDetect = true;
    // Bound per-display workers so multiple software streams leave CPU time
    // for capture, transport and input handling.
    params.iMultipleThreadIdc = static_cast<unsigned short>(
        (std::clamp)(std::thread::hardware_concurrency(), 1u, 4u));

    SSpatialLayerConfig& layer = params.sSpatialLayers[0];
    layer.iVideoWidth = static_cast<int>(width_);
    layer.iVideoHeight = static_cast<int>(height_);
    layer.fFrameRate = static_cast<float>(framerate_);
    layer.iSpatialBitrate = target_bitrate;
    layer.iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
    layer.uiProfileIdc = PRO_HIGH;
    layer.uiLevelIdc = LEVEL_UNKNOWN;
    layer.sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
    layer.sSliceArgument.uiSliceNum = params.iMultipleThreadIdc;

    status = encoder_->InitializeExt(&params);
    if (status != cmResultSuccess) {
        return openh264_error(status, "OpenH264 InitializeExt");
    }
    int format = videoFormatI420;
    status = encoder_->SetOption(ENCODER_OPTION_DATAFORMAT, &format);
    if (status != cmResultSuccess) {
        return openh264_error(status, "OpenH264 data-format setup");
    }
    SBitrateInfo limit{SPATIAL_LAYER_0, 0};
    status = encoder_->GetOption(ENCODER_OPTION_MAX_BITRATE, &limit);
    if (status != cmResultSuccess) {
        return openh264_error(status, "OpenH264 bitrate limit query");
    }
    if (limit.iBitrate > 0) bitrate_limit_ = static_cast<UINT>(limit.iBitrate);

    const OpenH264Version version = WelsGetCodecVersion();
    implementation_ = "OpenH264 " + std::to_string(version.uMajor) + "." +
                      std::to_string(version.uMinor) + "." +
                      std::to_string(version.uRevision);
    return Error::success();
}

Error OpenH264Encoder::create_staging_texture() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return check(device_->CreateTexture2D(&desc, nullptr, staging_.put()),
                 "create OpenH264 staging texture");
}

Error OpenH264Encoder::device_lost() const {
    if (device_.is_null()) {
        return Error(Status::DeviceRemoved, "the capture device is gone");
    }
    const HRESULT hr = device_->GetDeviceRemovedReason();
    return FAILED(hr) ? check(hr, "D3D11 device check") : Error::success();
}

Error OpenH264Encoder::read_nv12(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC source{};
    texture->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_NV12) {
        return Error(Status::InvalidCall,
                     "OpenH264 needs an NV12 texture, got format " +
                         std::to_string(static_cast<unsigned>(source.Format)));
    }
    if (source.Width < width_ || source.Height < height_) {
        return Error(Status::InvalidCall,
                     "the capture texture is smaller than the encoded picture");
    }
    ComPtr<ID3D11Device> source_device;
    texture->GetDevice(source_device.put());
    if (source_device.get() != device_.get() || source.SampleDesc.Count != 1) {
        return Error(Status::InvalidCall,
                     "OpenH264 needs a non-multisampled texture on its capture device");
    }

    const D3D11_BOX picture{0, 0, 0, width_, height_, 1};
    context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, texture, 0,
                                    &picture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_status = context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0,
                                             &mapped);
    if (FAILED(map_status)) {
        Error lost = device_lost();
        return lost.ok() ? check(map_status, "map OpenH264 staging texture") : lost;
    }
    if (!mapped.pData || mapped.RowPitch < width_) {
        context_->Unmap(staging_.get(), 0);
        return Error(Status::CaptureFailed, "invalid OpenH264 staging row pitch");
    }

    const auto* source_bytes = static_cast<const uint8_t*>(mapped.pData);
    const size_t luma_size = static_cast<size_t>(width_) * height_;
    uint8_t* const y_plane = i420_.data();
    uint8_t* const u_plane = y_plane + luma_size;
    uint8_t* const v_plane = u_plane + luma_size / 4;

    for (UINT row = 0; row < height_; ++row) {
        std::memcpy(y_plane + static_cast<size_t>(row) * width_,
                    source_bytes + static_cast<size_t>(row) * mapped.RowPitch,
                    width_);
    }
    const uint8_t* const uv_source =
        source_bytes + static_cast<size_t>(mapped.RowPitch) * height_;
    const UINT chroma_width = width_ / 2;
    const UINT chroma_height = height_ / 2;
    for (UINT row = 0; row < chroma_height; ++row) {
        const uint8_t* const uv =
            uv_source + static_cast<size_t>(row) * mapped.RowPitch;
        uint8_t* const u = u_plane + static_cast<size_t>(row) * chroma_width;
        uint8_t* const v = v_plane + static_cast<size_t>(row) * chroma_width;
        for (UINT column = 0; column < chroma_width; ++column) {
            u[column] = uv[column * 2];
            v[column] = uv[column * 2 + 1];
        }
    }
    context_->Unmap(staging_.get(), 0);
    return Error::success();
}

Error OpenH264Encoder::encode(ID3D11Texture2D* nv12, uint64_t pts_ms,
                              bool force_keyframe,
                              std::vector<EncodedPacket>& out) {
    if (!encoder_) {
        return Error(Status::NotCurrentlyAvailable, "the encoder is not open");
    }
    if (!nv12) {
        return Error(Status::InvalidCall, "the encoder needs a texture");
    }
    Error lost = device_lost();
    if (!lost.ok()) {
        return lost;
    }
    Error read = read_nv12(nv12);
    if (!read.ok()) {
        return read;
    }
    const auto result = send(pts_ms, force_keyframe, out);
    have_input_ = result.ok();
    return result;
}

Error OpenH264Encoder::encode_repeat(uint64_t pts_ms, bool force_keyframe,
                                     std::vector<EncodedPacket>& out) {
    if (!encoder_ || !have_input_) {
        return Error(Status::NotCurrentlyAvailable, "no encoded input to repeat");
    }
    Error lost = device_lost();
    if (!lost.ok()) {
        return lost;
    }
    return send(pts_ms, force_keyframe, out);
}

Error OpenH264Encoder::send(uint64_t pts_ms, bool force_keyframe,
                            std::vector<EncodedPacket>& out) {
    force_idr_ = force_idr_ || force_keyframe;
    if (force_idr_) {
        const int status = encoder_->ForceIntraFrame(true);
        if (status != cmResultSuccess) {
            return openh264_error(status, "OpenH264 ForceIntraFrame");
        }
    }

    const size_t luma_size = static_cast<size_t>(width_) * height_;
    SSourcePicture picture{};
    picture.iColorFormat = videoFormatI420;
    picture.iStride[0] = static_cast<int>(width_);
    picture.iStride[1] = static_cast<int>(width_ / 2);
    picture.iStride[2] = static_cast<int>(width_ / 2);
    picture.pData[0] = i420_.data();
    picture.pData[1] = picture.pData[0] + luma_size;
    picture.pData[2] = picture.pData[1] + luma_size / 4;
    picture.iPicWidth = static_cast<int>(width_);
    picture.iPicHeight = static_cast<int>(height_);
    picture.uiTimeStamp = static_cast<long long>(pts_ms);

    SFrameBSInfo frame{};
    const int status = encoder_->EncodeFrame(&picture, &frame);
    if (status != cmResultSuccess) {
        return openh264_error(status, "OpenH264 EncodeFrame");
    }
    if (frame.eFrameType == videoFrameTypeSkip || frame.iFrameSizeInBytes <= 0) {
        return Error::success();
    }

    EncodedPacket packet;
    packet.data.reserve(static_cast<size_t>(frame.iFrameSizeInBytes) + 6);
    packet.pts_ms = pts_ms;
    packet.keyframe = frame.eFrameType == videoFrameTypeIDR ||
                      frame.eFrameType == videoFrameTypeI;
    packet.idr = frame.eFrameType == videoFrameTypeIDR;
    // Mark the access unit explicitly: a parallel frame contains multiple
    // slices, which elementary-stream replay must keep in a single packet.
    packet.data = {0, 0, 0, 1, 9,
                   static_cast<uint8_t>(packet.keyframe ? 0x10 : 0x30)};
    for (int layer_index = 0; layer_index < frame.iLayerNum; ++layer_index) {
        const SLayerBSInfo& layer = frame.sLayerInfo[layer_index];
        if (!layer.pBsBuf || !layer.pNalLengthInByte) {
            continue;
        }
        size_t offset = 0;
        for (int nal_index = 0; nal_index < layer.iNalCount; ++nal_index) {
            const int nal_size = layer.pNalLengthInByte[nal_index];
            if (nal_size <= 0) {
                continue;
            }
            const uint8_t* const begin = layer.pBsBuf + offset;
            packet.data.insert(packet.data.end(), begin, begin + nal_size);
            offset += static_cast<size_t>(nal_size);
        }
    }
    if (packet.data.size() > 6) {
        if (packet.idr) force_idr_ = false;
        out.push_back(std::move(packet));
    } else {
        return Error(Status::CaptureFailed, "OpenH264 returned an empty access unit");
    }
    return Error::success();
}

Error OpenH264Encoder::set_rate(UINT bits_per_second, UINT framerate,
                                bool* keyframe_required) {
    if (keyframe_required) {
        *keyframe_required = false;
    }
    if (!encoder_) {
        return Error(Status::NotCurrentlyAvailable, "the encoder is not open");
    }

    const UINT fps = framerate == 0 ? framerate_ : clamp_framerate(framerate);
    const UINT bits = (std::min)(clamp_bitrate(bits_per_second, (std::max)(fps, framerate_)),
                                 bitrate_limit_);
    if (bits == bitrate_ && fps == framerate_) {
        return Error::success();
    }

    if (bits != bitrate_) {
        // Keep the level's maximum separate from the target. Updating only
        // SPATIAL_LAYER_ALL's maximum leaves the spatial layer's old ceiling
        // in place and causes later bitrate increases to fail validation.
        SBitrateInfo rate{SPATIAL_LAYER_ALL, static_cast<int>(bits)};
        const int status = encoder_->SetOption(ENCODER_OPTION_BITRATE, &rate);
        if (status != cmResultSuccess) {
            SBitrateInfo previous{SPATIAL_LAYER_ALL, static_cast<int>(bitrate_)};
            encoder_->SetOption(ENCODER_OPTION_BITRATE, &previous);
            return openh264_error(status, "OpenH264 bitrate update");
        }
        bitrate_ = bits;
    }

    if (fps != framerate_) {
        float frame_rate = static_cast<float>(fps);
        const int status =
            encoder_->SetOption(ENCODER_OPTION_FRAME_RATE, &frame_rate);
        if (status != cmResultSuccess) {
            return openh264_error(status, "OpenH264 frame-rate update");
        }
        framerate_ = fps;
    }
    return Error::success();
}

void OpenH264Encoder::close() {
    if (encoder_) {
        encoder_->Uninitialize();
        WelsDestroySVCEncoder(encoder_);
        encoder_ = nullptr;
    }
}

}  // namespace

std::unique_ptr<IVideoEncoder> create_openh264_encoder(
    ID3D11Device* device, const EncoderConfig& config, std::string* error) {
    auto encoder = std::make_unique<OpenH264Encoder>(device, config);
    Error initialized = encoder->initialize();
    if (!initialized.ok()) {
        if (error) {
            *error = initialized.to_string();
        }
        return nullptr;
    }
    return encoder;
}

}  // namespace chast
