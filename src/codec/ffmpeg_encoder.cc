// FFmpeg hardware encoder backend: NVENC, AMF and QSV through libavcodec.
//
// This is the fallback for adapters oneVPL does not cover (NVIDIA and AMD),
// and the second attempt on Intel when no VPL implementation can be opened.
// It follows RustDesk's VRAM encoder: the codec owns a D3D11VA frame pool,
// the capture texture is copied into that pool on the GPU, and libavcodec
// reads the picture from video memory. The only CPU-visible product is the
// compressed access unit; no frame is staged through system memory.
//
// A D3D11VA frames context can only pool textures it allocated itself, so the
// capture texture cannot be wrapped per frame. CopySubresourceRegion into the
// pool texture is the zero-copy equivalent of RustDesk's native BgraToNv12()
// into the encoder texture: both operands stay in VRAM and the copy shares the
// device's immediate context with the encoder, so ordering needs no flush.
//
// QSV derives its MFX device from the D3D11VA device, then the pool frame is
// mapped to D3D11 once; every frame is copied into that mapped texture. The
// mapping lives as long as the encoder, so no per-frame map/unmap runs.
//
// Device setup, frame-pool flags, rate control and repeat-frame handling
// mirror RustDesk's hwcodec ffmpeg/vram encoder. There is deliberately no
// software encoder here.

#include "chast/encoder.h"

#include <d3d10.h>
#include <d3d11.h>
#include <dxgi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "chast/com_ptr.h"
#include "encoder_internal.h"

namespace chast {
namespace {

constexpr UINT kVendorAmd = 0x1002;
constexpr UINT kVendorIntel = 0x8086;
constexpr UINT kVendorNvidia = 0x10DE;

enum class Driver {
    Nvenc,
    Amf,
    Qsv,
};

struct DriverInfo {
    Driver driver = Driver::Nvenc;
    const char* h264 = "";
    const char* hevc = "";
    const char* label = "";
};

bool driver_for_vendor(UINT vendor, DriverInfo* info) {
    switch (vendor) {
        case kVendorNvidia:
            *info = {Driver::Nvenc, "h264_nvenc", "hevc_nvenc", "NVIDIA NVENC"};
            return true;
        case kVendorAmd:
            *info = {Driver::Amf, "h264_amf", "hevc_amf", "AMD AMF"};
            return true;
        case kVendorIntel:
            *info = {Driver::Qsv, "h264_qsv", "hevc_qsv", "Intel QSV"};
            return true;
        default:
            return false;
    }
}

const char* encoder_name(const DriverInfo& driver, VideoCodec codec) {
    return codec == VideoCodec::H264 ? driver.h264 : driver.hevc;
}

std::string av_error_text(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return text;
}

Error av_error(int code, std::string_view what) {
    return Error(Status::CaptureFailed,
                 std::string(what) + " failed: " + av_error_text(code) + " (" +
                     std::to_string(code) + ")");
}

bool adapter_vendor(ID3D11Device* device, UINT* vendor) {
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(dxgi.put())))) {
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi->GetAdapter(adapter.put()))) {
        return false;
    }
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) {
        return false;
    }
    *vendor = desc.VendorId;
    return true;
}

// IDR detection for key-frame packets. Encoders without GLOBAL_HEADER emit
// Annex B; a CRA picture (HEVC 21) is a key frame for the viewer but not an
// IDR, and resetting a decoder on it is not required.
bool contains_idr(VideoCodec codec, const uint8_t* data, size_t size) {
    const uint8_t* const end = data + size;
    const uint8_t* cursor = data;
    while (cursor + 3 < end) {
        size_t prefix = 0;
        if (cursor[0] == 0 && cursor[1] == 0 && cursor[2] == 1) {
            prefix = 3;
        } else if (cursor + 4 <= end && cursor[0] == 0 && cursor[1] == 0 &&
                   cursor[2] == 0 && cursor[3] == 1) {
            prefix = 4;
        }
        if (prefix == 0) {
            ++cursor;
            continue;
        }
        const uint8_t* nal = cursor + prefix;
        if (nal >= end) {
            break;
        }
        if (codec == VideoCodec::H264) {
            if ((nal[0] & 0x1f) == 5) {
                return true;
            }
        } else {
            const uint8_t type = static_cast<uint8_t>((nal[0] >> 1) & 0x3f);
            if (type == 19 || type == 20) {
                return true;
            }
        }
        cursor = nal + 1;
    }
    return false;
}

class FFmpegEncoder final : public IVideoEncoder {
public:
    FFmpegEncoder(ID3D11Device* device, const EncoderConfig& config)
        : device_(device), config_(config) {
        if (device_) {
            device_.add_ref();
        }
    }

    ~FFmpegEncoder() override { close(); }

    Error initialize();
    Error encode(ID3D11Texture2D* nv12, uint64_t pts_ms, bool force_keyframe,
                 std::vector<EncodedPacket>& out) override;
    Error encode_repeat(uint64_t pts_ms, bool force_keyframe,
                        std::vector<EncodedPacket>& out) override;
    Error set_rate(UINT bits_per_second, UINT framerate,
                   bool* keyframe_required) override;

    UINT bitrate() const noexcept override { return bitrate_; }
    UINT framerate() const noexcept override { return framerate_; }
    VideoCodec codec() const noexcept override { return codec_; }
    UINT width() const noexcept override { return width_; }
    UINT height() const noexcept override { return height_; }
    const std::string& implementation() const noexcept override {
        return implementation_;
    }
    bool shared_import() const noexcept override { return false; }

private:
    Error open_device();
    Error open_codec();
    Error open_codec_impl();
    void close_codec();
    void close();
    Error probe();
    Error copy_texture(ID3D11Texture2D* texture);
    Error send(uint64_t pts_ms, bool force_keyframe,
               std::vector<EncodedPacket>& out);
    Error drain(std::vector<EncodedPacket>& out);
    Error flush(std::vector<EncodedPacket>& out);
    Error reopen(std::vector<EncodedPacket>& out);
    Error device_lost() const;

    int gop_size() const;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> d3d_context_;
    EncoderConfig config_;
    DriverInfo driver_;
    VideoCodec codec_ = VideoCodec::H264;
    std::string implementation_;

    AVBufferRef* hw_device_ = nullptr;
    AVCodecContext* codec_context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* mapped_ = nullptr;  // QSV only: the D3D11 view of frame_
    AVPacket* packet_ = nullptr;
    const AVCodec* codec_api_ = nullptr;
    // Non-owning; the texture belongs to frame_ or mapped_.
    ID3D11Texture2D* encode_texture_ = nullptr;
    UINT encode_subresource_ = 0;

    UINT width_ = 0;
    UINT height_ = 0;
    UINT bitrate_ = 0;
    UINT framerate_ = 30;
    // What the open codec context was configured with.
    UINT context_bitrate_ = 0;
    UINT context_framerate_ = 0;
    bool have_input_ = false;
    bool reopen_pending_ = false;
    uint64_t last_pts_ms_ = 0;
};

int FFmpegEncoder::gop_size() const {
    if (config_.gop_size != 0) {
        return static_cast<int>((std::min)(config_.gop_size, UINT{65535}));
    }
    // Key frames are requested per frame; both QSV and the FFmpeg encoders
    // treat this as "no periodic IDR". QSV takes the Media SDK field width.
    return driver_.driver == Driver::Qsv ? 65535 : (std::numeric_limits<int>::max)();
}

Error FFmpegEncoder::initialize() {
    if (device_.is_null()) {
        return Error(Status::InvalidCall, "FFmpeg encoding needs a D3D11 device");
    }
    if (config_.width == 0 || config_.height == 0 || (config_.width & 1) != 0 ||
        (config_.height & 1) != 0) {
        return Error(Status::InvalidCall,
                     "FFmpeg NV12 encoding needs an even, non-zero frame size");
    }
    width_ = config_.width;
    height_ = config_.height;
    framerate_ = config_.framerate != 0 ? config_.framerate : 30;

    UINT vendor = 0;
    if (!adapter_vendor(device_.get(), &vendor)) {
        return Error(Status::Unsupported,
                     "cannot read the DXGI adapter of the capture device");
    }
    if (!driver_for_vendor(vendor, &driver_)) {
        char text[32] = {};
        std::snprintf(text, sizeof(text), "0x%04X", vendor);
        return Error(Status::Unsupported,
                     std::string("the FFmpeg backend has no hardware encoder for "
                                 "adapter vendor ") +
                         text);
    }

    codec_ = config_.codec;
    const char* name = encoder_name(driver_, codec_);
    if (!avcodec_find_encoder_by_name(name)) {
        if (codec_ == VideoCodec::H264) {
            return Error(Status::Unsupported,
                         std::string("this FFmpeg build has no ") + name);
        }
        codec_ = VideoCodec::H264;
        name = encoder_name(driver_, codec_);
        if (!avcodec_find_encoder_by_name(name)) {
            return Error(Status::Unsupported,
                         std::string("this FFmpeg build has no ") + name);
        }
    }

    bitrate_ = config_.bitrate != 0
                   ? config_.bitrate
                   : hardware_bitrate(width_, height_, config_.quality_ratio,
                                      codec_ == VideoCodec::H264);
    bitrate_ = (std::max)(bitrate_, UINT{2});

    Error ready = open_device();
    if (ready.ok()) {
        ready = open_codec();
    }
    if (!ready.ok()) {
        return ready;
    }
    packet_ = av_packet_alloc();
    if (!packet_) {
        return Error(Status::CaptureFailed, "av_packet_alloc failed");
    }

    implementation_ = "FFmpeg " + std::string(av_version_info()) + " " +
                      encoder_name(driver_, codec_) + " (" + driver_.label +
                      ", D3D11)";
    Error tested = probe();
    if (!tested.ok()) {
        return tested;
    }
    return Error::success();
}

Error FFmpegEncoder::open_device() {
    hw_device_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_) {
        return Error(Status::Unsupported, "av_hwdevice_ctx_alloc(D3D11VA) failed");
    }
    auto* device_context = reinterpret_cast<AVHWDeviceContext*>(hw_device_->data);
    auto* d3d11 = static_cast<AVD3D11VADeviceContext*>(device_context->hwctx);
    // The hw device context releases the device and the context it is given.
    device_.add_ref();
    d3d11->device = device_.get();
    ID3D11DeviceContext* immediate = nullptr;
    device_->GetImmediateContext(&immediate);
    if (!immediate) {
        return Error(Status::CaptureFailed,
                     "the capture device has no immediate context");
    }
    d3d11->device_context = immediate;
    immediate->AddRef();
    d3d_context_.attach(immediate);

    int status = av_hwdevice_ctx_init(hw_device_);
    if (status < 0) {
        return av_error(status, "av_hwdevice_ctx_init(D3D11VA)");
    }
    if (driver_.driver == Driver::Qsv) {
        // mfx-gen drives the device's immediate context from its own threads,
        // and refuses the handle on a device without multithread protection.
        // The capture device is shared with the capture pipeline, so the
        // protection also keeps MFX and the capturer from racing the context.
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(multithread.put())))) {
            multithread->SetMultithreadProtected(TRUE);
        }
        // QSV has to encode on the capture adapter, not on whatever "auto"
        // would pick, so derive the MFX device from the D3D11 one.
        AVBufferRef* derived = nullptr;
        status = av_hwdevice_ctx_create_derived(&derived, AV_HWDEVICE_TYPE_QSV,
                                                hw_device_, 0);
        if (status < 0) {
            return av_error(status, "av_hwdevice_ctx_create_derived(QSV)");
        }
        av_buffer_unref(&hw_device_);
        hw_device_ = derived;
    }
    return Error::success();
}

Error FFmpegEncoder::open_codec() {
    const Error err = open_codec_impl();
    if (!err.ok()) {
        // Leave no half-open context behind: reopen() retries from a clean
        // state when a new frame rate is refused.
        close_codec();
    }
    return err;
}

Error FFmpegEncoder::open_codec_impl() {
    codec_api_ = avcodec_find_encoder_by_name(encoder_name(driver_, codec_));
    if (!codec_api_) {
        return Error(Status::Unsupported,
                     std::string("this FFmpeg build has no ") +
                         encoder_name(driver_, codec_));
    }
    codec_context_ = avcodec_alloc_context3(codec_api_);
    if (!codec_context_) {
        return Error(Status::CaptureFailed, "avcodec_alloc_context3 failed");
    }

    AVCodecContext* c = codec_context_;
    c->width = static_cast<int>(width_);
    c->height = static_cast<int>(height_);
    c->time_base = AVRational{1, 1000};
    c->framerate = AVRational{static_cast<int>(framerate_), 1};
    c->pix_fmt = driver_.driver == Driver::Qsv ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11;
    c->sw_pix_fmt = AV_PIX_FMT_NV12;
    c->has_b_frames = 0;
    c->max_b_frames = 0;
    c->gop_size = gop_size();
    c->keyint_min = (std::numeric_limits<int>::max)();
    c->slices = 1;
    c->thread_count = 1;
    c->thread_type = FF_THREAD_SLICE;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // Our protocol carries no out-of-band config: SPS/PPS must stay in the
    // key-frame packets, as RustDesk's LOCAL_HEADER does.
    c->flags2 |= AV_CODEC_FLAG2_LOCAL_HEADER;
    c->color_range = AVCOL_RANGE_MPEG;
    c->colorspace = AVCOL_SPC_SMPTE170M;
    c->color_primaries = AVCOL_PRI_SMPTE170M;
    c->color_trc = AVCOL_TRC_SMPTE170M;
    c->profile = codec_ == VideoCodec::H264 ? AV_PROFILE_H264_HIGH
                                            : AV_PROFILE_HEVC_MAIN;
    if (driver_.driver == Driver::Qsv) {
        // RustDesk's CBR arrangement: QSV selects CBR only when bit_rate and
        // rc_max_rate are equal, so keep max at the target and the target one
        // bit lower, which picks VBR capped at the configured rate.
        c->bit_rate = static_cast<int64_t>(bitrate_) - 1;
        c->rc_max_rate = bitrate_;
        c->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    } else {
        c->bit_rate = bitrate_;
        c->rc_max_rate = bitrate_;
    }

    auto set_option = [&](const char* key, const char* value) -> Error {
        const int status = av_opt_set(c->priv_data, key, value, 0);
        if (status < 0) {
            return av_error(status, std::string("av_opt_set(") + key + ")");
        }
        return Error::success();
    };
    Error tuned = Error::success();
    switch (driver_.driver) {
        case Driver::Nvenc:
            tuned = set_option("rc", "cbr");
            if (tuned.ok()) {
                tuned = set_option("delay", "0");
            }
            if (tuned.ok()) {
                tuned = set_option("forced-idr", "1");
            }
            break;
        case Driver::Amf:
            tuned = set_option("rc", "cbr");
            if (tuned.ok()) {
                tuned = set_option("forced_idr", "1");
            }
            break;
        case Driver::Qsv:
            tuned = set_option("async_depth", "1");
            if (tuned.ok()) {
                tuned = set_option("forced_idr", "1");
            }
            break;
    }
    if (!tuned.ok()) {
        return tuned;
    }

    AVBufferRef* frames_ref = av_hwframe_ctx_alloc(hw_device_);
    if (!frames_ref) {
        return Error(Status::CaptureFailed, "av_hwframe_ctx_alloc failed");
    }
    auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    frames->format = driver_.driver == Driver::Qsv ? AV_PIX_FMT_QSV
                                                   : AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = static_cast<int>(width_);
    frames->height = static_cast<int>(height_);
    frames->initial_pool_size = driver_.driver == Driver::Qsv ? 0 : 1;
    if (driver_.driver != Driver::Qsv) {
        auto* frames_d3d11 = static_cast<AVD3D11VAFramesContext*>(frames->hwctx);
        frames_d3d11->BindFlags = D3D11_BIND_RENDER_TARGET;
        frames_d3d11->MiscFlags = 0;
    }
    int status = av_hwframe_ctx_init(frames_ref);
    if (status < 0) {
        av_buffer_unref(&frames_ref);
        return av_error(status, "av_hwframe_ctx_init");
    }
    c->hw_device_ctx = av_buffer_ref(hw_device_);
    c->hw_frames_ctx = av_buffer_ref(frames_ref);
    av_buffer_unref(&frames_ref);
    if (!c->hw_device_ctx || !c->hw_frames_ctx) {
        return Error(Status::CaptureFailed, "av_buffer_ref failed");
    }

    status = avcodec_open2(c, codec_api_, nullptr);
    if (status < 0) {
        return av_error(status, "avcodec_open2");
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        return Error(Status::CaptureFailed, "av_frame_alloc failed");
    }
    frame_->format = c->pix_fmt;
    frame_->width = c->width;
    frame_->height = c->height;
    frame_->color_range = c->color_range;
    frame_->color_primaries = c->color_primaries;
    frame_->color_trc = c->color_trc;
    frame_->colorspace = c->colorspace;
    status = av_hwframe_get_buffer(c->hw_frames_ctx, frame_, 0);
    if (status < 0) {
        return av_error(status, "av_hwframe_get_buffer");
    }
    if (frame_->format == AV_PIX_FMT_QSV) {
        mapped_ = av_frame_alloc();
        if (!mapped_) {
            return Error(Status::CaptureFailed, "av_frame_alloc failed");
        }
        mapped_->format = AV_PIX_FMT_D3D11;
        status = av_hwframe_map(mapped_, frame_,
                                AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE);
        if (status < 0) {
            return av_error(status, "av_hwframe_map(QSV)");
        }
        encode_texture_ = reinterpret_cast<ID3D11Texture2D*>(mapped_->data[0]);
        encode_subresource_ = static_cast<UINT>(
            reinterpret_cast<uintptr_t>(mapped_->data[1]));
    } else {
        encode_texture_ = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
        encode_subresource_ =
            static_cast<UINT>(reinterpret_cast<uintptr_t>(frame_->data[1]));
    }
    if (!encode_texture_) {
        return Error(Status::CaptureFailed, "the encoder frame has no D3D11 texture");
    }

    context_bitrate_ = bitrate_;
    context_framerate_ = framerate_;
    have_input_ = false;
    return Error::success();
}

void FFmpegEncoder::close_codec() {
    encode_texture_ = nullptr;
    encode_subresource_ = 0;
    if (mapped_) {
        av_frame_free(&mapped_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (codec_context_) {
        avcodec_free_context(&codec_context_);
    }
    codec_api_ = nullptr;
    have_input_ = false;
}

void FFmpegEncoder::close() {
    close_codec();
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (hw_device_) {
        av_buffer_unref(&hw_device_);
    }
    d3d_context_.reset();
    device_.reset();
}

Error FFmpegEncoder::probe() {
    const size_t luma = static_cast<size_t>(width_) * height_;
    std::vector<uint8_t> black(luma + luma / 2, 0x80);
    std::memset(black.data(), 0x10, luma);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = black.data();
    initial.SysMemPitch = width_;
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr =
        device_->CreateTexture2D(&desc, &initial, texture.put());
    if (FAILED(hr)) {
        return check(hr, "CreateTexture2D(NV12 probe)");
    }

    // One encode proves the runtime can actually produce a packet; a second
    // one covers encoders that hold the first frame. Both are discarded.
    std::vector<EncodedPacket> packets;
    Error encoded = encode(texture.get(), 0, true, packets);
    if (encoded.ok() && packets.empty()) {
        encoded = encode_repeat(1, true, packets);
    }
    if (!encoded.ok()) {
        return encoded;
    }
    if (packets.empty()) {
        return Error(Status::CaptureFailed,
                     "the encoder produced no packet for its test frame");
    }
    have_input_ = false;
    return Error::success();
}

Error FFmpegEncoder::device_lost() const {
    if (device_.is_null()) {
        return Error(Status::DeviceRemoved, "the capture device is gone");
    }
    const HRESULT hr = device_->GetDeviceRemovedReason();
    if (FAILED(hr)) {
        return check(hr, "D3D11 device check");
    }
    return Error::success();
}

Error FFmpegEncoder::copy_texture(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC source{};
    texture->GetDesc(&source);
    if (source.Format != DXGI_FORMAT_NV12) {
        return Error(Status::InvalidCall,
                     "the encoder needs an NV12 texture, got format " +
                         std::to_string(static_cast<unsigned>(source.Format)));
    }
    if (source.Width < width_ || source.Height < height_) {
        return Error(Status::InvalidCall,
                     "the capture texture is smaller than the encoded picture");
    }
    const D3D11_BOX picture{0, 0, 0, width_, height_, 1};
    d3d_context_->CopySubresourceRegion(encode_texture_, encode_subresource_, 0, 0,
                                        0, texture, 0, &picture);
    return Error::success();
}

Error FFmpegEncoder::encode(ID3D11Texture2D* nv12, uint64_t pts_ms,
                            bool force_keyframe,
                            std::vector<EncodedPacket>& out) {
    if (!codec_context_) {
        return Error(Status::NotCurrentlyAvailable, "the encoder is not open");
    }
    if (!nv12) {
        return Error(Status::InvalidCall, "the encoder needs a texture");
    }
    Error lost = device_lost();
    if (!lost.ok()) {
        return lost;
    }
    if (reopen_pending_) {
        Error reopened = reopen(out);
        if (!reopened.ok()) {
            return reopened;
        }
        force_keyframe = true;
    }
    Error copied = copy_texture(nv12);
    if (!copied.ok()) {
        return copied;
    }
    have_input_ = true;
    return send(pts_ms, force_keyframe, out);
}

Error FFmpegEncoder::encode_repeat(uint64_t pts_ms, bool force_keyframe,
                                   std::vector<EncodedPacket>& out) {
    if (!codec_context_ || !have_input_) {
        return Error(Status::NotCurrentlyAvailable, "no encoded input to repeat");
    }
    if (reopen_pending_) {
        // The frame pool is about to be replaced; the caller still has the
        // capture texture and will fall back to encode().
        return Error(Status::NotCurrentlyAvailable, "the encoder is reopening");
    }
    Error lost = device_lost();
    if (!lost.ok()) {
        return lost;
    }
    return send(pts_ms, force_keyframe, out);
}

Error FFmpegEncoder::send(uint64_t pts_ms, bool force_keyframe,
                          std::vector<EncodedPacket>& out) {
    frame_->pts = static_cast<int64_t>(pts_ms);
    frame_->pict_type =
        force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (force_keyframe) {
        frame_->flags |= AV_FRAME_FLAG_KEY;
    } else {
        frame_->flags &= ~AV_FRAME_FLAG_KEY;
    }
    last_pts_ms_ = pts_ms;

    int status = avcodec_send_frame(codec_context_, frame_);
    if (status == AVERROR(EAGAIN)) {
        Error drained = drain(out);
        if (!drained.ok()) {
            return drained;
        }
        status = avcodec_send_frame(codec_context_, frame_);
    }
    if (status < 0) {
        Error lost = device_lost();
        return lost.ok() ? av_error(status, "avcodec_send_frame") : lost;
    }
    Error drained = drain(out);
    if (!drained.ok()) {
        Error lost = device_lost();
        return lost.ok() ? drained : lost;
    }
    return Error::success();
}

Error FFmpegEncoder::drain(std::vector<EncodedPacket>& out) {
    for (;;) {
        const int status = avcodec_receive_packet(codec_context_, packet_);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            return Error::success();
        }
        if (status < 0) {
            return av_error(status, "avcodec_receive_packet");
        }
        if (packet_->size > 0 && packet_->data) {
            EncodedPacket encoded;
            encoded.data.assign(packet_->data, packet_->data + packet_->size);
            encoded.pts_ms = packet_->pts == AV_NOPTS_VALUE
                                 ? last_pts_ms_
                                 : static_cast<uint64_t>(packet_->pts);
            encoded.keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
            encoded.idr =
                encoded.keyframe &&
                contains_idr(codec_, encoded.data.data(), encoded.data.size());
            out.push_back(std::move(encoded));
        }
        av_packet_unref(packet_);
    }
}

Error FFmpegEncoder::flush(std::vector<EncodedPacket>& out) {
    if (!codec_context_) {
        return Error::success();
    }
    int status = avcodec_send_frame(codec_context_, nullptr);
    if (status < 0 && status != AVERROR_EOF) {
        return av_error(status, "avcodec_send_frame(flush)");
    }
    return drain(out);
}

Error FFmpegEncoder::reopen(std::vector<EncodedPacket>& out) {
    Error flushed = flush(out);
    if (!flushed.ok()) {
        return flushed;
    }
    const UINT previous_framerate = context_framerate_;
    const UINT previous_bitrate = context_bitrate_;
    const UINT wanted_framerate = framerate_;
    const UINT wanted_bitrate = bitrate_;

    close_codec();
    Error opened = open_codec();
    if (!opened.ok()) {
        const Error failure = opened;
        framerate_ = previous_framerate;
        bitrate_ = previous_bitrate;
        Error fallback = open_codec();
        if (!fallback.ok()) {
            return failure;
        }
        reopen_pending_ = false;
        return Error::success();
    }
    framerate_ = wanted_framerate;
    bitrate_ = wanted_bitrate;
    reopen_pending_ = false;
    return Error::success();
}

Error FFmpegEncoder::set_rate(UINT bits_per_second, UINT framerate,
                              bool* keyframe_required) {
    if (keyframe_required) {
        *keyframe_required = false;
    }
    if (!codec_context_) {
        return Error(Status::NotCurrentlyAvailable, "the encoder is not open");
    }
    const UINT bits = (std::max)(bits_per_second, UINT{2});
    const UINT fps = framerate != 0 ? framerate : framerate_;
    if (bits == bitrate_ && fps == framerate_) {
        return Error::success();
    }

    if (driver_.driver == Driver::Qsv) {
        // FFmpeg's QSV encoder picks bit_rate/framerate changes up from the
        // context, then flushes and resets the MFX session before the next
        // frame. A reset starts a new sequence, so the caller must force an
        // IDR; there is no continuous-resequence path as in oneVPL.
        bitrate_ = bits;
        framerate_ = fps;
        codec_context_->bit_rate = static_cast<int64_t>(bits) - 1;
        codec_context_->rc_max_rate = bits;
        codec_context_->framerate = AVRational{static_cast<int>(fps), 1};
        if (keyframe_required) {
            *keyframe_required = true;
        }
        return Error::success();
    }

    // NVENC and AMF re-read the rate at the next frame without restarting the
    // sequence; a frame-rate change does not reach their rate control that
    // way, so it reopens the codec before the next picture.
    const bool fps_change = fps != context_framerate_;
    bitrate_ = bits;
    framerate_ = fps;
    codec_context_->bit_rate = bits;
    codec_context_->rc_max_rate = bits;
    if (fps_change) {
        reopen_pending_ = true;
        if (keyframe_required) {
            *keyframe_required = true;
        }
    }
    return Error::success();
}

}  // namespace

std::unique_ptr<IVideoEncoder> create_ffmpeg_encoder(ID3D11Device* device,
                                                     const EncoderConfig& config,
                                                     std::string* error) {
    auto encoder = std::make_unique<FFmpegEncoder>(device, config);
    const Error err = encoder->initialize();
    if (!err.ok()) {
        if (error) {
            *error = err.to_string();
        }
        return nullptr;
    }
    return encoder;
}

bool ffmpeg_encoder_available(VideoCodec codec, ID3D11Device* device) {
    DriverInfo driver;
    if (device) {
        UINT vendor = 0;
        if (!adapter_vendor(device, &vendor) || !driver_for_vendor(vendor, &driver)) {
            return false;
        }
    } else {
        // Without an adapter only the build can be checked. Any vendored
        // hardware encoder means this build carries the backend at all; a
        // probe inside the requested family needs the device.
        static constexpr const char* kAll[] = {"h264_nvenc", "hevc_nvenc",
                                               "h264_amf",   "hevc_amf",
                                               "h264_qsv",   "hevc_qsv"};
        for (const char* name : kAll) {
            if (avcodec_find_encoder_by_name(name)) {
                return true;
            }
        }
        return false;
    }

    if (avcodec_find_encoder_by_name(encoder_name(driver, codec))) {
        return true;
    }
    return codec == VideoCodec::Hevc &&
           avcodec_find_encoder_by_name(encoder_name(driver, VideoCodec::H264));
}

}  // namespace chast
