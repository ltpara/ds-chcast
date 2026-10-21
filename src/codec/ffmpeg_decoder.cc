// FFmpeg D3D11VA decoder: hardware decode straight into NV12 textures.
//
// Replaces the oneVPL decoder on the receiving side. The decode context is
// opened on the caller's D3D11 device (the display GPU), so every picture
// arrives as a slice of a texture array and the renderer can convert it
// without a system-memory round trip. The verification tools run without a
// device: they try D3D11VA on the default adapter and fall back to FFmpeg's
// software decoders, transferring hardware pictures to NV12 only to measure
// them.
//
// Initialisation is deferred to the first key frame: the decode context is
// created lazily so that a session which starts mid-stream asks for an IDR
// instead of feeding delta frames to a decoder that has no SPS yet.

#include "chast/decoder.h"

#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <libavutil/hwcontext_d3d11va.h>

#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chast/com_ptr.h"

namespace chast {

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

AVCodecID av_codec_id(VideoCodec codec) {
    return codec == VideoCodec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
}

const char* codec_name(VideoCodec codec) {
    return codec == VideoCodec::H264 ? "H.264" : "HEVC";
}

AVPixelFormat select_d3d11(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_D3D11) {
            return *format;
        }
    }
    return AV_PIX_FMT_NONE;
}

Error av_error(int code, std::string_view what) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed: " + text + " (" +
                     std::to_string(code) + ")");
}

uint64_t frame_pts(const AVFrame& frame) {
    return frame.pts == AV_NOPTS_VALUE ? 0 : static_cast<uint64_t>(frame.pts);
}

class FFmpegDecoder final : public IVideoDecoder {
public:
    FFmpegDecoder(VideoCodec codec, const DecoderConfig& config)
        : codec_(codec),
          copy_frames_(config.copy_frames),
          gpu_(config.d3d11_device != nullptr && static_cast<bool>(config.gpu_handler)),
          gpu_handler_(config.gpu_handler) {
        if (config.d3d11_device) {
            device_ = ComPtr<ID3D11Device>(config.d3d11_device);
            device_.add_ref();
        }
    }

    ~FFmpegDecoder() override { close(); }

    Error open();
    Error decode(const uint8_t* data, size_t size, uint64_t pts_ms, bool keyframe,
                 std::vector<DecodedFrame>& out) override;
    Error flush(std::vector<DecodedFrame>& out) override;
    bool initialized() const noexcept override { return initialized_; }
    UINT width() const noexcept override { return width_; }
    UINT height() const noexcept override { return height_; }
    const std::string& implementation() const noexcept override { return implementation_; }

private:
    void close();
    Error start();
    Error submit(std::vector<DecodedFrame>& out);
    Error receive(std::vector<DecodedFrame>& out);
    Error deliver(AVFrame& frame, std::vector<DecodedFrame>& out);
    Error collect(const AVFrame& frame, DecodedFrame& out) const;

    VideoCodec codec_;
    bool copy_frames_ = false;
    bool gpu_ = false;
    bool hardware_ = false;
    bool initialized_ = false;
    bool flushed_ = false;
    GpuFrameHandler gpu_handler_;
    ComPtr<ID3D11Device> device_;
    AVBufferRef* hw_device_ = nullptr;
    const AVCodec* codec_api_ = nullptr;
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* software_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    std::string implementation_;
};

Error FFmpegDecoder::open() {
    if (gpu_ && device_.is_null()) {
        return Error(Status::InvalidCall, "GPU decoding needs a D3D11 device");
    }

    codec_api_ = avcodec_find_decoder(av_codec_id(codec_));
    if (!codec_api_) {
        return Error(Status::Unsupported,
                     std::string("this FFmpeg build has no ") + codec_name(codec_) +
                         " decoder");
    }
    context_ = avcodec_alloc_context3(codec_api_);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    software_ = av_frame_alloc();
    if (!context_ || !packet_ || !frame_ || !software_) {
        return Error(Status::CaptureFailed, "FFmpeg decoder allocation failed");
    }

    int status = 0;
    if (gpu_) {
        hw_device_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hw_device_) {
            return Error(Status::Unsupported, "av_hwdevice_ctx_alloc(D3D11VA) failed");
        }
        auto* device_context = reinterpret_cast<AVHWDeviceContext*>(hw_device_->data);
        auto* d3d11 = static_cast<AVD3D11VADeviceContext*>(device_context->hwctx);
        // The hw device context releases this reference when it is freed.
        device_.add_ref();
        d3d11->device = device_.get();
        status = av_hwdevice_ctx_init(hw_device_);
        if (status < 0) {
            return av_error(status, "av_hwdevice_ctx_init(D3D11VA)");
        }
        context_->hw_device_ctx = av_buffer_ref(hw_device_);
        context_->get_format = select_d3d11;
        hardware_ = true;
    } else {
        // Tools run without a display device. Use the default adapter when it
        // can decode, otherwise stay on the CPU.
        status = av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_D3D11VA, nullptr,
                                        nullptr, 0);
        if (status >= 0) {
            context_->hw_device_ctx = av_buffer_ref(hw_device_);
            context_->get_format = select_d3d11;
            hardware_ = true;
        } else {
            av_buffer_unref(&hw_device_);
        }
    }

    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    implementation_ = std::string("FFmpeg ") + (hardware_ ? "D3D11VA" : "software") +
                      " (" + codec_name(codec_) + ")";
    return Error::success();
}

void FFmpegDecoder::close() {
    if (software_) {
        av_frame_free(&software_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (context_) {
        avcodec_free_context(&context_);
    }
    if (hw_device_) {
        av_buffer_unref(&hw_device_);
    }
}

Error FFmpegDecoder::start() {
    const int status = avcodec_open2(context_, codec_api_, nullptr);
    if (status < 0) {
        return av_error(status, "avcodec_open2");
    }
    initialized_ = true;
    return Error::success();
}

Error FFmpegDecoder::decode(const uint8_t* data, size_t size, uint64_t pts_ms,
                            bool keyframe, std::vector<DecodedFrame>& out) {
    if (!data || size == 0) {
        return Error(Status::InvalidCall, "decoder needs a non-empty access unit");
    }
    if (flushed_) {
        return Error(Status::InvalidCall, "decode after flush");
    }
    if (!initialized_) {
        if (!keyframe) {
            return Error(Status::NotCurrentlyAvailable,
                         "decoder waits for a key frame to initialise");
        }
        Error opened = start();
        if (!opened.ok()) {
            return opened;
        }
    }

    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    packet_->pts = static_cast<int64_t>(pts_ms);
    packet_->dts = packet_->pts;
    return submit(out);
}

Error FFmpegDecoder::submit(std::vector<DecodedFrame>& out) {
    for (;;) {
        const int status = avcodec_send_packet(context_, packet_);
        if (status != AVERROR(EAGAIN)) {
            if (status < 0) {
                return av_error(status, "avcodec_send_packet");
            }
            return receive(out);
        }
        // The decoder's input queue is full; pulling the pending pictures out
        // is the documented way to make room.
        Error drained = receive(out);
        if (!drained.ok()) {
            return drained;
        }
    }
}

Error FFmpegDecoder::receive(std::vector<DecodedFrame>& out) {
    for (;;) {
        av_frame_unref(frame_);
        const int status = avcodec_receive_frame(context_, frame_);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            return Error::success();
        }
        if (status < 0) {
            return av_error(status, "avcodec_receive_frame");
        }
        Error delivered = deliver(*frame_, out);
        if (!delivered.ok()) {
            return delivered;
        }
    }
}

Error FFmpegDecoder::deliver(AVFrame& frame, std::vector<DecodedFrame>& out) {
    if (gpu_) {
        if (frame.format != AV_PIX_FMT_D3D11) {
            return Error(Status::Unsupported,
                         "FFmpeg did not decode into a D3D11 texture");
        }
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame.data[0]);
        if (!texture) {
            return Error(Status::CaptureFailed, "the decoded frame has no texture");
        }
        width_ = static_cast<UINT>(frame.width);
        height_ = static_cast<UINT>(frame.height);

        GpuFrame picture;
        picture.texture = texture;
        picture.subresource =
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frame.data[1]));
        picture.width = width_;
        picture.height = height_;
        picture.pts_ms = frame_pts(frame);
        return gpu_handler_(picture);
    }

    const AVFrame* visible = &frame;
    if (frame.format == AV_PIX_FMT_D3D11) {
        av_frame_unref(software_);
        const int status = av_hwframe_transfer_data(software_, &frame, 0);
        if (status < 0) {
            return av_error(status, "av_hwframe_transfer_data");
        }
        visible = software_;
    }

    DecodedFrame picture;
    Error measured = collect(*visible, picture);
    if (!measured.ok()) {
        return measured;
    }
    out.push_back(std::move(picture));
    return Error::success();
}

Error FFmpegDecoder::collect(const AVFrame& frame, DecodedFrame& out) const {
    const UINT width = static_cast<UINT>(frame.width);
    const UINT height = static_cast<UINT>(frame.height);
    if (width == 0 || height == 0 || !frame.data[0] || frame.linesize[0] <= 0) {
        return Error(Status::CaptureFailed, "the decoded frame has no luma plane");
    }
    const UINT pitch = static_cast<UINT>(frame.linesize[0]);
    const uint8_t* luma = frame.data[0];

    uint64_t hash = kFnvOffset;
    uint64_t sum = 0;
    uint64_t sum_sq = 0;
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* row = luma + static_cast<size_t>(y) * pitch;
        for (UINT x = 0; x < width; ++x) {
            const uint8_t value = row[x];
            hash = (hash ^ value) * kFnvPrime;
            sum += value;
            sum_sq += static_cast<uint64_t>(value) * value;
        }
    }
    const double samples = static_cast<double>(width) * height;
    out.pts_ms = frame_pts(frame);
    out.width = width;
    out.height = height;
    out.pitch = pitch;
    out.hash = hash;
    out.luma_mean = static_cast<double>(sum) / samples;
    const double mean_sq = static_cast<double>(sum_sq) / samples;
    const double variance = mean_sq - out.luma_mean * out.luma_mean;
    out.luma_variance = variance > 0.0 ? variance : 0.0;

    if (copy_frames_) {
        const UINT chroma_width = (width + 1) / 2;
        const UINT chroma_height = (height + 1) / 2;
        out.nv12.resize(static_cast<size_t>(width) * height +
                        static_cast<size_t>(chroma_width) * chroma_height * 2);
        uint8_t* dst = out.nv12.data();
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * width,
                        luma + static_cast<size_t>(y) * pitch, width);
        }
        dst += static_cast<size_t>(width) * height;
        if (frame.format == AV_PIX_FMT_NV12) {
            const uint8_t* uv = frame.data[1];
            if (!uv || frame.linesize[1] <= 0) {
                return Error(Status::CaptureFailed, "the decoded frame has no chroma plane");
            }
            const UINT uv_pitch = static_cast<UINT>(frame.linesize[1]);
            for (UINT y = 0; y < chroma_height; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * chroma_width * 2,
                            uv + static_cast<size_t>(y) * uv_pitch, chroma_width * 2);
            }
        } else if (frame.format == AV_PIX_FMT_YUV420P) {
            const uint8_t* u = frame.data[1];
            const uint8_t* v = frame.data[2];
            if (!u || !v || frame.linesize[1] <= 0 || frame.linesize[2] <= 0) {
                return Error(Status::CaptureFailed, "the decoded frame has no chroma planes");
            }
            for (UINT y = 0; y < chroma_height; ++y) {
                const uint8_t* u_row = u + static_cast<size_t>(y) * frame.linesize[1];
                const uint8_t* v_row = v + static_cast<size_t>(y) * frame.linesize[2];
                uint8_t* uv_row = dst + static_cast<size_t>(y) * chroma_width * 2;
                for (UINT x = 0; x < chroma_width; ++x) {
                    uv_row[x * 2] = u_row[x];
                    uv_row[x * 2 + 1] = v_row[x];
                }
            }
        } else {
            return Error(Status::Unsupported, "the decoded frame is not NV12 or YUV420P");
        }
    }
    return Error::success();
}

Error FFmpegDecoder::flush(std::vector<DecodedFrame>& out) {
    if (!initialized_ || flushed_) {
        return Error::success();
    }
    flushed_ = true;
    const int status = avcodec_send_packet(context_, nullptr);
    if (status < 0 && status != AVERROR_EOF) {
        return av_error(status, "avcodec_send_packet (flush)");
    }
    return receive(out);
}

}  // namespace

std::unique_ptr<IVideoDecoder> create_decoder(const DecoderConfig& config,
                                              std::string* error) {
    auto decoder = std::make_unique<FFmpegDecoder>(config.codec, config);
    Error opened = decoder->open();
    if (!opened.ok()) {
        if (error) {
            *error = opened.to_string();
        }
        return nullptr;
    }
    return decoder;
}

}  // namespace chast

// FFmpeg's codec registry pulls in the QSV decoder object, which references
// the QSV user-plugin loader even though this project never selects QSV. The
// oneVPL dispatcher we link has no user-plugin support, so resolve the dead
// reference with an unsupported stub.
extern "C" int __cdecl MFXVideoUSER_Load(void*, const void*, unsigned int) {
    return -3;  // MFX_ERR_UNSUPPORTED
}
