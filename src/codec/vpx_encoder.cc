// VP9 software encoding. I444 reads the original BGRA capture, never NV12.
#include "encoder_internal.h"
#include "chast/com_ptr.h"

#include <libyuv.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <thread>

namespace chast {
namespace {
class VpxEncoder final : public IVideoEncoder {
public:
    VpxEncoder(ID3D11Device* device, const EncoderConfig& config) : device_(device), config_(config) {
        if (device_) device_.add_ref();
    }
    ~VpxEncoder() override {
        if (opened_) vpx_codec_destroy(&codec_);
        if (image_) vpx_img_free(image_);
    }
    Error initialize() {
        if (!device_ || config_.codec != VideoCodec::Vp9 || !config_.width || !config_.height ||
            config_.width > 8192 || config_.height > 8192 ||
            uint64_t(config_.width) * config_.height > 7680ull * 4320ull ||
            (config_.chroma != VideoChroma::Yuv420 && config_.chroma != VideoChroma::Yuv444))
            return Error(Status::InvalidCall, "invalid VP9 source or format");
        device_->GetImmediateContext(context_.put());
        if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &settings_, 0) != VPX_CODEC_OK)
            return Error(Status::Unsupported, "VP9 encoder configuration is unavailable");
        fps_ = (std::clamp)(config_.framerate, 1u, 60u);
        bitrate_ = config_.bitrate ? config_.bitrate :
            hardware_bitrate(config_.width, config_.height, config_.quality_ratio, false);
        settings_.g_w = config_.width; settings_.g_h = config_.height;
        settings_.g_timebase = {1, 1000};
        settings_.g_threads = (std::clamp)(std::thread::hardware_concurrency(), 1u, 8u);
        settings_.g_profile = bgra_input() ? 1 : 0;
        settings_.g_lag_in_frames = 0;
        settings_.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
        settings_.rc_end_usage = VPX_CBR;
        settings_.rc_target_bitrate = (std::max)(1u, bitrate_ / 1000);
        settings_.rc_min_quantizer = 0; settings_.rc_max_quantizer = 56;
        settings_.rc_buf_initial_sz = 500; settings_.rc_buf_optimal_sz = 600;
        settings_.rc_buf_sz = 1000; settings_.rc_dropframe_thresh = 0;
        settings_.kf_mode = config_.gop_size ? VPX_KF_AUTO : VPX_KF_DISABLED;
        settings_.kf_min_dist = 0; settings_.kf_max_dist = config_.gop_size;
        if (vpx_codec_enc_init(&codec_, vpx_codec_vp9_cx(), &settings_, 0) != VPX_CODEC_OK)
            return failure("VP9 encoder initialization");
        opened_ = true;
        if (vpx_codec_control(&codec_, VP8E_SET_CPUUSED, 7) != VPX_CODEC_OK ||
            vpx_codec_control(&codec_, VP9E_SET_ROW_MT, 1u) != VPX_CODEC_OK ||
            vpx_codec_control(&codec_, VP9E_SET_TUNE_CONTENT, VP9E_CONTENT_SCREEN) != VPX_CODEC_OK ||
            vpx_codec_control(&codec_, VP8E_SET_ENABLEAUTOALTREF, 0u) != VPX_CODEC_OK)
            return failure("VP9 realtime controls");
        image_ = vpx_img_alloc(nullptr, bgra_input() ? VPX_IMG_FMT_I444 : VPX_IMG_FMT_I420,
                              config_.width, config_.height, 32);
        if (!image_) return Error(Status::CaptureFailed, "could not allocate VP9 planes");
        image_->cs = VPX_CS_BT_601;
        image_->range = VPX_CR_STUDIO_RANGE;
        name_ = std::string("libvpx ") + vpx_codec_version_str() +
            (bgra_input() ? " VP9 I444" : " VP9 I420");
        return Error::success();
    }
    Error encode(ID3D11Texture2D* texture, uint64_t pts, bool force,
                 std::vector<EncodedPacket>& out) override {
        const auto copied = copy(texture);
        if (!copied.ok()) return copied;
        have_input_ = true;
        return send(pts, force, out);
    }
    Error encode_repeat(uint64_t pts, bool force, std::vector<EncodedPacket>& out) override {
        if (!have_input_) return Error(Status::NotCurrentlyAvailable, "VP9 has no previous image");
        const auto alive = check(device_->GetDeviceRemovedReason(), "VP9 capture device");
        return alive.ok() ? send(pts, force, out) : alive;
    }
    Error set_rate(UINT bits, UINT fps, bool* keyframe) override {
        if (keyframe) *keyframe = false;
        auto next = settings_;
        next.rc_target_bitrate = (std::max)(1u, bits / 1000);
        if (vpx_codec_enc_config_set(&codec_, &next) != VPX_CODEC_OK) return failure("VP9 rate change");
        settings_ = next; bitrate_ = bits; fps_ = (std::clamp)(fps, 1u, 60u);
        return Error::success();
    }
    UINT bitrate() const noexcept override { return bitrate_; }
    UINT framerate() const noexcept override { return fps_; }
    UINT width() const noexcept override { return config_.width; }
    UINT height() const noexcept override { return config_.height; }
    VideoCodec codec() const noexcept override { return VideoCodec::Vp9; }
    VideoChroma chroma() const noexcept override { return config_.chroma; }
    bool bgra_input() const noexcept override { return config_.chroma == VideoChroma::Yuv444; }
    EncoderBackend backend() const noexcept override { return EncoderBackend::Software; }
    bool shared_import() const noexcept override { return false; }
    const std::string& implementation() const noexcept override { return name_; }
private:
    Error failure(const char* action) const {
        const char* detail = vpx_codec_error_detail(&codec_);
        return Error(Status::CaptureFailed, std::string(action) + ": " +
            vpx_codec_error(&codec_) + (detail ? std::string("; ") + detail : ""));
    }
    Error copy(ID3D11Texture2D* texture) {
        if (!texture) return Error(Status::InvalidCall, "VP9 capture texture is missing");
        const auto alive = check(device_->GetDeviceRemovedReason(), "VP9 capture device");
        if (!alive.ok()) return alive;
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        ComPtr<ID3D11Device> owner; texture->GetDevice(owner.put());
        const DXGI_FORMAT wanted = bgra_input() ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_NV12;
        if (owner.get() != device_.get() || desc.Format != wanted || desc.ArraySize != 1 ||
            desc.SampleDesc.Count != 1 || desc.Width < config_.width || desc.Height < config_.height ||
            desc.Width > 8192 || desc.Height > 8192)
            return Error(Status::InvalidCall, "VP9 capture texture does not match its input format");
        if (!staging_ || desc.Width != staging_width_ || desc.Height != staging_height_) {
            auto staging = desc; staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = staging.MiscFlags = 0; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MipLevels = 1;
            ComPtr<ID3D11Texture2D> next;
            auto made = check(device_->CreateTexture2D(&staging, nullptr, next.put()), "VP9 readback texture");
            if (!made.ok()) return made;
            staging_ = std::move(next); staging_width_ = desc.Width; staging_height_ = desc.Height;
        }
        if (bgra_input() && (desc.Width != config_.width || desc.Height != config_.height))
            scaled_.resize(size_t(config_.width) * config_.height * 4);
        context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, texture, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        auto read = check(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped), "VP9 readback");
        if (!read.ok()) return read;
        int converted = -1;
        const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
        if (bgra_input()) {
            const uint8_t* source = bytes; int pitch = static_cast<int>(mapped.RowPitch);
            if (desc.Width != config_.width || desc.Height != config_.height) {
                converted = libyuv::ARGBScale(bytes, pitch, desc.Width, desc.Height,
                    scaled_.data(), config_.width * 4, config_.width, config_.height, libyuv::kFilterBox);
                source = scaled_.data(); pitch = config_.width * 4;
            } else converted = 0;
            if (converted == 0) converted = libyuv::ARGBToI444(source, pitch,
                image_->planes[0], image_->stride[0], image_->planes[1], image_->stride[1],
                image_->planes[2], image_->stride[2], config_.width, config_.height);
        } else {
            converted = libyuv::NV12ToI420(bytes, mapped.RowPitch,
                bytes + size_t(mapped.RowPitch) * desc.Height, mapped.RowPitch,
                image_->planes[0], image_->stride[0], image_->planes[1], image_->stride[1],
                image_->planes[2], image_->stride[2], config_.width, config_.height);
        }
        context_->Unmap(staging_.get(), 0);
        return converted == 0 ? Error::success() : Error(Status::CaptureFailed, "VP9 pixel conversion failed");
    }
    Error send(uint64_t pts, bool force, std::vector<EncodedPacket>& out) {
        if (pts > uint64_t((std::numeric_limits<int64_t>::max)()))
            return Error(Status::InvalidCall, "VP9 timestamp overflow");
        if (vpx_codec_encode(&codec_, image_, static_cast<int64_t>(pts), (std::max)(1u, 1000u / fps_),
            force ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_REALTIME) != VPX_CODEC_OK) return failure("VP9 encode");
        vpx_codec_iter_t iterator = nullptr;
        while (const auto* packet = vpx_codec_get_cx_data(&codec_, &iterator)) {
            if (packet->kind != VPX_CODEC_CX_FRAME_PKT) continue;
            const auto& frame = packet->data.frame;
            EncodedPacket output;
            const auto* begin = static_cast<const uint8_t*>(frame.buf);
            output.data.assign(begin, begin + frame.sz);
            output.pts_ms = static_cast<uint64_t>(frame.pts);
            output.keyframe = output.idr = (frame.flags & VPX_FRAME_IS_KEY) != 0;
            out.push_back(std::move(output));
        }
        return Error::success();
    }
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> staging_;
    EncoderConfig config_;
    vpx_codec_ctx_t codec_{};
    vpx_codec_enc_cfg_t settings_{};
    vpx_image_t* image_ = nullptr;
    bool opened_ = false, have_input_ = false;
    UINT bitrate_ = 0, fps_ = 30, staging_width_ = 0, staging_height_ = 0;
    std::vector<uint8_t> scaled_;
    std::string name_;
};
}

std::unique_ptr<IVideoEncoder> create_vpx_encoder(ID3D11Device* device,
    const EncoderConfig& config, std::string* error) {
    auto encoder = std::make_unique<VpxEncoder>(device, config);
    const auto initialized = encoder->initialize();
    if (!initialized.ok()) { if (error) *error = initialized.to_string(); return nullptr; }
    return encoder;
}
}
