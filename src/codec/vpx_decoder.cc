#include "vpx_internal.h"
#include "chast/com_ptr.h"

#include <libyuv.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <algorithm>
#include <limits>
#include <thread>

namespace chast {
namespace {
class VpxDecoder final : public IVideoDecoder {
public:
    explicit VpxDecoder(const DecoderConfig& config) : config_(config), device_(config.d3d11_device) {
        if (device_) { device_.add_ref(); device_->GetImmediateContext(context_.put()); }
    }
    ~VpxDecoder() override { if (opened_) vpx_codec_destroy(&codec_); }
    Error open() {
        if (config_.chroma != VideoChroma::Yuv420 && config_.chroma != VideoChroma::Yuv444)
            return Error(Status::InvalidCall, "invalid VP9 chroma");
        vpx_codec_dec_cfg_t settings{};
        settings.threads = (std::clamp)(std::thread::hardware_concurrency(), 1u, 8u);
        if (vpx_codec_dec_init(&codec_, vpx_codec_vp9_dx(), &settings, 0) != VPX_CODEC_OK)
            return failure("VP9 decoder initialization");
        opened_ = true;
        vpx_codec_control(&codec_, VP9D_SET_ROW_MT, 1);
        name_ = std::string("libvpx ") + vpx_codec_version_str() +
            (device_ ? " VP9 / BGRA GPU upload" : " VP9");
        return Error::success();
    }
    Error decode(const uint8_t* data, size_t size, uint64_t pts, bool keyframe,
                 std::vector<DecodedFrame>& out) override {
        if (flushed_) return Error(Status::InvalidCall, "VP9 decoder was flushed");
        if (!data || !size || size > (std::numeric_limits<unsigned int>::max)())
            return Error(Status::InvalidCall, "invalid VP9 packet size");
        if (!initialized_ && !keyframe) return Error(Status::NotCurrentlyAvailable, "VP9 needs a keyframe");
        vpx_codec_stream_info_t info{}; info.sz = sizeof(info);
        const auto peeked = vpx_codec_peek_stream_info(vpx_codec_vp9_dx(), data, static_cast<unsigned int>(size), &info);
        if ((keyframe && (peeked != VPX_CODEC_OK || !info.is_kf)) ||
            (peeked == VPX_CODEC_OK && info.is_kf && !valid_size(info.w, info.h)))
            return Error(Status::Unsupported, "invalid or oversized VP9 keyframe");
        if (vpx_codec_decode(&codec_, data, static_cast<unsigned int>(size), nullptr, 0) != VPX_CODEC_OK)
            return failure("VP9 decode");
        initialized_ = true;
        return deliver(pts, out);
    }
    Error flush(std::vector<DecodedFrame>& out) override {
        if (flushed_ || !initialized_) return Error::success();
        flushed_ = true;
        if (vpx_codec_decode(&codec_, nullptr, 0, nullptr, 0) != VPX_CODEC_OK) return failure("VP9 flush");
        return deliver(last_pts_, out);
    }
    bool initialized() const noexcept override { return initialized_; }
    UINT width() const noexcept override { return width_; }
    UINT height() const noexcept override { return height_; }
    const std::string& implementation() const noexcept override { return name_; }
private:
    static bool valid_size(unsigned w, unsigned h) {
        return w && h && w <= 8192 && h <= 8192 && uint64_t(w) * h <= 7680ull * 4320ull;
    }
    Error failure(const char* what) const {
        const char* detail = vpx_codec_error_detail(&codec_);
        return Error(Status::CaptureFailed, std::string(what) + ": " + vpx_codec_error(&codec_) +
            (detail ? std::string("; ") + detail : ""));
    }
    Error deliver(uint64_t pts, std::vector<DecodedFrame>& out) {
        vpx_codec_iter_t iterator = nullptr;
        while (const auto* frame = vpx_codec_get_frame(&codec_, &iterator)) {
            const bool full = frame->fmt == VPX_IMG_FMT_I444;
            if ((!full && frame->fmt != VPX_IMG_FMT_I420) || !valid_size(frame->d_w, frame->d_h) ||
                full != (config_.chroma == VideoChroma::Yuv444))
                return Error(Status::Unsupported, "VP9 picture does not match the negotiated format");
            width_ = frame->d_w; height_ = frame->d_h; last_pts_ = pts;
            const bool gpu = device_ && config_.gpu_handler;
            if (gpu || (config_.copy_frames && full)) {
                bgra_.resize(size_t(width_) * height_ * 4);
                const int converted = full ? libyuv::I444ToARGB(
                    frame->planes[0], frame->stride[0], frame->planes[1], frame->stride[1],
                    frame->planes[2], frame->stride[2], bgra_.data(), width_ * 4, width_, height_) :
                    libyuv::I420ToARGB(frame->planes[0], frame->stride[0], frame->planes[1], frame->stride[1],
                    frame->planes[2], frame->stride[2], bgra_.data(), width_ * 4, width_, height_);
                if (converted != 0) return Error(Status::CaptureFailed, "VP9 output conversion failed");
            }
            if (gpu) {
                auto alive = check(device_->GetDeviceRemovedReason(), "VP9 display device");
                if (!alive.ok()) return alive;
                if (!texture_ || texture_width_ != width_ || texture_height_ != height_) {
                    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width_; desc.Height = height_;
                    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ComPtr<ID3D11Texture2D> next;
                    auto made = check(device_->CreateTexture2D(&desc, nullptr, next.put()), "VP9 upload texture");
                    if (!made.ok()) return made;
                    texture_ = std::move(next); texture_width_ = width_; texture_height_ = height_;
                }
                context_->UpdateSubresource(texture_.get(), 0, nullptr, bgra_.data(), width_ * 4, 0);
                GpuFrame output; output.texture = texture_.get(); output.width = width_;
                output.height = height_; output.pts_ms = pts;
                auto result = config_.gpu_handler(output);
                if (!result.ok()) return result;
            } else {
                DecodedFrame output; output.pts_ms = pts; output.width = width_; output.height = height_;
                output.pitch = frame->stride[0]; output.chroma = config_.chroma;
                uint64_t hash = 14695981039346656037ull, sum = 0, squares = 0;
                for (UINT y = 0; y < height_; ++y) for (UINT x = 0; x < width_; ++x) {
                    const uint8_t value = frame->planes[0][size_t(y) * frame->stride[0] + x];
                    hash = (hash ^ value) * 1099511628211ull; sum += value; squares += unsigned(value) * value;
                }
                const double pixels = double(width_) * height_;
                output.hash = hash; output.luma_mean = sum / pixels;
                output.luma_variance = squares / pixels - output.luma_mean * output.luma_mean;
                if (config_.copy_frames) {
                    if (full) output.bgra = bgra_;
                    else {
                        const UINT uv_pitch = ((width_ + 1) / 2) * 2;
                        output.nv12.resize(size_t(width_) * height_ + size_t(uv_pitch) * ((height_ + 1) / 2));
                        libyuv::I420ToNV12(frame->planes[0], frame->stride[0], frame->planes[1], frame->stride[1],
                            frame->planes[2], frame->stride[2], output.nv12.data(), width_,
                            output.nv12.data() + size_t(width_) * height_, uv_pitch, width_, height_);
                    }
                }
                out.push_back(std::move(output));
            }
        }
        return Error::success();
    }
    DecoderConfig config_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> texture_;
    vpx_codec_ctx_t codec_{};
    bool opened_ = false, initialized_ = false, flushed_ = false;
    UINT width_ = 0, height_ = 0, texture_width_ = 0, texture_height_ = 0;
    uint64_t last_pts_ = 0;
    std::vector<uint8_t> bgra_;
    std::string name_;
};
}
std::unique_ptr<IVideoDecoder> create_vpx_decoder(const DecoderConfig& config, std::string* error) {
    auto decoder = std::make_unique<VpxDecoder>(config);
    const auto opened = decoder->open();
    if (!opened.ok()) { if (error) *error = opened.to_string(); return nullptr; }
    return decoder;
}
}
