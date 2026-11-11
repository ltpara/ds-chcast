#pragma once

// Video encoders for the frames the capturer publishes.
//
// Hardware and 4:2:0 encoders consume FrameInfo::nv12_texture, the NV12 texture
// the capture pipeline already produced. Hardware backends keep this image
// in VRAM; only the compressed bitstream comes back out. That is
// the same deal RustDesk's VRAM encoder strikes with its ffmpeg *_{qsv,nvenc,amf}
// encoders and with oneVPL.
//
// Two hardware backends implement this: oneVPL directly (the primary path on Intel
// iGPUs) and FFmpeg's libavcodec encoders (nvenc on NVIDIA, amf on AMD, qsv on
// Intel). The FFmpeg backend gives NVIDIA and AMD adapters a hardware path
// without oneVPL; it copies the capture texture into an encoder-owned D3D11VA
// frame pool on the GPU, so the frame still never reaches system memory.
//
// Auto uses OpenH264 as a final CPU fallback when no vendor runtime can encode
// on the capture adapter. That path stages NV12 through system memory and
// converts it to I420; pinned Vpl and Ffmpeg backends remain hardware-only.
//
// On oneVPL 2.10+ runtimes the D3D11 texture in capture device memory
// either maps the texture (shared import) or GPU-copies it into its internal
// surface pool (import by copy). Legacy Media SDK ABI 1.x runtimes (mfxhw64,
// shipped for older Intel iGPUs) have no memory interface; there the texture is
// wrapped by the encoder's own frame allocator and read in place. The FFmpeg
// backend always GPU-copies into its own pool. None of these paths stages the
// image through RAM or the CPU.
//
// VP9 uses libvpx on the CPU. Its 4:2:0 path reads NV12; 4:4:4 reads the
// original FrameInfo::texture (BGRA), scales it if necessary, then converts
// directly to I444. bgra_input() tells capture consumers which texture to use.

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "chast/error.h"

namespace chast {

enum class VideoCodec {
    H264,
    Hevc,
    // AV1 is only produced by hardware encoders (oneVPL on AV1-capable Intel
    // GPUs, the FFmpeg vendor encoders on Ada/RDNA3/Arc). Auto may fall back to
    // HEVC/H.264, including OpenH264; it never produces software AV1.
    Av1,
    Vp9,
};

enum class VideoChroma : uint8_t { Yuv420 = 0, Yuv444 = 1 };
inline constexpr bool valid_video_format(VideoCodec codec, VideoChroma chroma) {
    return (codec == VideoCodec::H264 || codec == VideoCodec::Hevc || codec == VideoCodec::Av1 || codec == VideoCodec::Vp9) &&
        (chroma == VideoChroma::Yuv420 || (chroma == VideoChroma::Yuv444 && codec == VideoCodec::Vp9));
}

using VideoCodecMask = uint8_t;
inline constexpr VideoCodecMask kVideoCodecH264 = 0x01;
inline constexpr VideoCodecMask kVideoCodecHevc = 0x02;
inline constexpr VideoCodecMask kVideoCodecAv1 = 0x04;
inline constexpr VideoCodecMask kVideoCodecVp9 = 0x08;
inline constexpr VideoCodecMask kVideoCodecAll = kVideoCodecH264 | kVideoCodecHevc | kVideoCodecAv1 | kVideoCodecVp9;
inline constexpr VideoCodecMask video_codec_bit(VideoCodec codec) {
    return codec == VideoCodec::H264 ? kVideoCodecH264 :
           codec == VideoCodec::Hevc ? kVideoCodecHevc :
           codec == VideoCodec::Av1 ? kVideoCodecAv1 : codec == VideoCodec::Vp9 ? kVideoCodecVp9 : 0;
}

// Which implementation create_encoder() may use. Auto tries oneVPL, then
// FFmpeg's vendor encoder, then OpenH264 software encoding.
// Vpl and Ffmpeg pin hardware backends; Software forces the CPU fallback.
enum class EncoderBackend {
    Auto,
    Vpl,
    Ffmpeg,
    Software,
};

const char* encoder_backend_name(EncoderBackend backend);
bool parse_encoder_backend(std::string_view text, EncoderBackend& backend);

// Encoder tuning, mirroring what RustDesk's VRAM path asks ffmpeg for:
// CBR, no B-frames, an effectively infinite GOP (key frames are requested per
// frame instead of scheduled), and RustDesk's resolution-keyed bitrate table.
struct EncoderConfig {
    VideoCodec codec = VideoCodec::H264;
    UINT width = 0;   // frame size to encode
    UINT height = 0;
    // Bits per second. Zero selects RustDesk's hardware_bitrate() for the frame
    // size and quality, which is what its VRAM encoder starts with.
    UINT bitrate = 0;
    UINT framerate = 30;
    // In frames. 0 disables periodic IDRs. OpenH264 screen mode may still
    // generate an IDR for a scene change; every backend honours explicit requests.
    UINT gop_size = 0;
    // RustDesk's Quality knob, applied on top of base_bitrate() only when
    // `bitrate` is zero: Best 1.5, Balanced 0.67, Low 0.5.
    float quality_ratio = 0.67f;
    // Implementation family to create. See EncoderBackend.
    EncoderBackend backend = EncoderBackend::Auto;
    VideoChroma chroma = VideoChroma::Yuv420;
};

// One compressed access unit.
struct EncodedPacket {
    std::vector<uint8_t> data;
    uint64_t pts_ms = 0;
    bool keyframe = false;
    bool idr = false;
};

// Backend-independent interface, kept separate from the capture library.
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    // Actual implementation, including when an Auto request fell back.
    virtual EncoderBackend backend() const noexcept = 0;
    virtual VideoChroma chroma() const noexcept { return VideoChroma::Yuv420; }
    // Full-chroma software encoders read the original BGRA capture; feeding
    // NV12 here would irreversibly discard the colour detail first.
    virtual bool bgra_input() const noexcept { return false; }

    // Encodes NV12, or original BGRA when bgra_input() is true. It must live on the device it was
    // created with and be at least EncoderConfig::width x height. Blocks until
    // the packets for this frame are ready and appends them to `out` (the
    // encoder has no reorder buffer, so that is normally exactly one packet).
    // `force_keyframe` asks for an IDR on this frame, which is how the
    // application reacts to a new viewer or a lost reference frame.
    virtual Error encode(ID3D11Texture2D* nv12, uint64_t pts_ms, bool force_keyframe,
                         std::vector<EncodedPacket>& out) = 0;
    virtual Error encode_cpu_bgra(const uint8_t*, UINT, UINT, uint64_t, bool,
                                  std::vector<EncodedPacket>&) {
        return Error(Status::Unsupported, "encoder has no CPU pixel input");
    }

    // Encodes the last submitted image again, for frames whose desktop content
    // did not change (FrameInfo::nv12_updated is false). Same result as
    // encode() with that texture, but the encoder reuses the surface it already
    // holds instead of importing/GPU-copying the unchanged texture again.
    // Fails with NotCurrentlyAvailable before the first successful encode() and
    // on backends without repeat support; the caller then falls back to
    // encode(), which still has the texture. DeviceRemoved/DeviceReset means the
    // device itself is gone: recreate the encoder instead of falling back.
    // `pts_ms` is the new timestamp.
    virtual Error encode_repeat(uint64_t pts_ms, bool force_keyframe,
                                std::vector<EncodedPacket>& out) = 0;

    // Changes the target bitrate and capture frame rate without recreating the
    // session, like RustDesk's set_bitrate when the quality setting moves. The
    // rate control divides the bitrate over the frame rate, so a caller that
    // captures slower must move both or every frame gets a budget for a frame
    // it is not sending. The new rates take effect from the next frame.
    //
    // `keyframe_required` reports whether the next frame has to be an IDR. A
    // bitrate-only change can continue the coded sequence (the encoder keeps
    // its reference frames, so the viewer decodes on). Some backends restart
    // the sequence on a frame-rate change; OpenH264 updates it in place. The
    // caller must honour this flag when a restart or reopen needs an IDR,
    // and forcing one anyway wastes a whole frame on every quality step.
    virtual Error set_rate(UINT bits_per_second, UINT framerate,
                           bool* keyframe_required = nullptr) = 0;

    virtual UINT bitrate() const noexcept = 0;
    virtual UINT framerate() const noexcept = 0;
    // The codec the encoder is actually producing. Anything that tells a peer
    // what format the stream uses (session negotiation, file naming) must use
    // this, not EncoderConfig::codec: an HEVC request can be fulfilled by an
    // AVC tier, and a peer told HEVC would decode the stream as the wrong
    // format.
    virtual VideoCodec codec() const noexcept = 0;
    virtual UINT width() const noexcept = 0;
    virtual UINT height() const noexcept = 0;

    // Name of the encoder implementation that was selected ("oneVPL ..."),
    // for logs and the harness output.
    virtual const std::string& implementation() const noexcept = 0;

    // How the most recent frame reached the encoder's internal surfaces: true
    // when the runtime mapped the application's texture directly (shared
    // import) or, on legacy runtimes, used it in place through the frame
    // allocator; false for GPU copies and OpenH264's CPU readback. This flag
    // alone does not distinguish GPU copies from software encoding.
    // Meaningful after the first encode().
    virtual bool shared_import() const noexcept = 0;
};

// Which encoders can be used on this machine, for a UI or an auto-selection
// step. True when a hardware implementation of `codec` (H.264, HEVC or AV1)
// with D3D11 acceleration is available through `backend`: a oneVPL 2.10+
// runtime or the legacy Media SDK runtime for Vpl, NVENC/AMF/QSV for Ffmpeg,
// either for Auto. Software always returns false; this query excludes CPU
// availability and does not gate create_encoder().
//
// When `device` is given, implementations are restricted to the adapter that
// device lives on, which is the adapter the frames come from. A non-H.264
// codec counts as available when its own implementation exists or when one a
// lower tier falls back to does: create_encoder() may answer an HEVC request
// with AVC, and an AV1 request with HEVC or AVC. The device is not opened.
bool hardware_encoder_available(VideoCodec codec, ID3D11Device* device = nullptr,
                                EncoderBackend backend = EncoderBackend::Auto);

// RustDesk's base_bitrate(): the bitrate table from libs/scrap codec.rs, in
// kbit/s, for a screen of the given size. Unknown sizes fall back to 1080p.
UINT base_bitrate(UINT width, UINT height);

// RustDesk's HwRamEncoder::calc_bitrate(): base_bitrate() scaled by the quality
// ratio and by the codec's headroom factor, in bit/s. This is what a VRAM
// encoder is created with and what set_quality() switches to.
UINT hardware_bitrate(UINT width, UINT height, float quality_ratio, bool h264);

// Creates an encoder for `config.width x config.height` on `device`.
// The device is the adapter selection: implementations on another DXGI LUID
// are rejected for hardware backends. With Auto, oneVPL is tried first and FFmpeg's vendor
// encoder is the hardware fallback and OpenH264 is the final software tier; a
// pinned backend does not fall back at all.
//
// VPL encoding tiers are tried in order -- VDENC+HEVC, VDENC+AVC, VME+AVC --
// and each tier must return a test frame before it is accepted, so a runtime
// that opens a session but cannot encode does not become the stream's encoder.
// codec() reports what won; an H.264 request never upgrades to HEVC, while an
// HEVC request may fall back to AVC on either backend. The frame geometry must
// be unrotated; the encoder has no rotation support, so a caller with a
// rotated desktop has to convert before encoding.
std::unique_ptr<IVideoEncoder> create_encoder(ID3D11Device* device, const EncoderConfig& config,
                                              std::string* error);

}  // namespace chast
