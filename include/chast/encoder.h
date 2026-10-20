#pragma once

// Hardware video encoder for the frames the capturer publishes.
//
// The encoder consumes FrameInfo::nv12_texture, the GPU-resident NV12 texture
// the capture pipeline already produced, so no frame crosses into system
// memory on the way in; only the compressed bitstream comes back out. That is
// the same deal RustDesk's VRAM encoder strikes with ffmpeg's *_{qsv,nvenc,amf}
// encoders, except the API here is Intel oneVPL directly instead of libavcodec.
//
// Like capture, this has no CPU fallback: a GPU the vendor runtime refuses to
// encode on makes create_encoder() fail, it does not silently switch to a
// software encoder.
//
// The frame is handed over as a D3D11 texture in the capture device's video
// memory, so the runtime never sees system memory: it either maps the texture
// (shared import) or GPU-copies it into its internal surface pool (import by
// copy). Neither path stages the image through RAM or the CPU.

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "chast/error.h"

namespace chast {

enum class VideoCodec {
    H264,
    Hevc,
};

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
    // In frames. 0 means "key frames only when asked": RustDesk passes
    // i32::MAX for the same reason, a screen stream with no periodic IDR.
    UINT gop_size = 0;
    // RustDesk's Quality knob, applied on top of base_bitrate() only when
    // `bitrate` is zero: Best 1.5, Balanced 0.67, Low 0.5.
    float quality_ratio = 0.67f;
};

// One compressed access unit.
struct EncodedPacket {
    std::vector<uint8_t> data;
    uint64_t pts_ms = 0;
    bool keyframe = false;
    bool idr = false;
};

// Abstract so the VPL implementation stays out of the capture library and so a
// second backend (NVENC, AMF) can slot in without touching callers.
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // Encodes one NV12 texture, which must live on the device the encoder was
    // created with and be at least EncoderConfig::width x height. Blocks until
    // the packets for this frame are ready and appends them to `out` (the
    // encoder has no reorder buffer, so that is normally exactly one packet).
    // `force_keyframe` asks for an IDR on this frame, which is how the
    // application reacts to a new viewer or a lost reference frame.
    virtual Error encode(ID3D11Texture2D* nv12, uint64_t pts_ms, bool force_keyframe,
                         std::vector<EncodedPacket>& out) = 0;

    // Changes the target bitrate without recreating the session, like RustDesk's
    // set_bitrate when the quality setting moves. The new rate takes effect
    // from the next frame.
    virtual Error set_bitrate(UINT bits_per_second) = 0;

    virtual UINT bitrate() const noexcept = 0;
    virtual VideoCodec codec() const noexcept = 0;
    virtual UINT width() const noexcept = 0;
    virtual UINT height() const noexcept = 0;

    // Name of the encoder implementation that was selected ("oneVPL ..."),
    // for logs and the harness output.
    virtual const std::string& implementation() const noexcept = 0;

    // How the most recent frame reached the encoder's internal surfaces: true
    // when the runtime mapped the application's texture directly (shared
    // import), false when it GPU-copied it into its own pool. Both keep the
    // frame in VRAM; the flag exists so a driver that starts honouring shared
    // import is visible instead of being assumed. Meaningful after the first
    // encode().
    virtual bool shared_import() const noexcept = 0;
};

// Which encoders exist on this machine, for a UI or an auto-selection step.
// True when a hardware implementation of `codec` (H.264 or HEVC) can be
// enumerated; the device is not opened.
bool hardware_encoder_available(VideoCodec codec);

// RustDesk's base_bitrate(): the bitrate table from libs/scrap codec.rs, in
// kbit/s, for a screen of the given size. Unknown sizes fall back to 1080p.
UINT base_bitrate(UINT width, UINT height);

// RustDesk's HwRamEncoder::calc_bitrate(): base_bitrate() scaled by the quality
// ratio and by the codec's headroom factor, in bit/s. This is what a VRAM
// encoder is created with and what set_quality() switches to.
UINT hardware_bitrate(UINT width, UINT height, float quality_ratio, bool h264);

// Creates a hardware encoder for `config.width x config.height` on `device`.
// `adapter_luid` selects the VPL implementation that runs on the same adapter
// as the capture device; pass AdapterDevice::luid from the capturer.
std::unique_ptr<IVideoEncoder> create_encoder(ID3D11Device* device, const LUID& adapter_luid,
                                              const EncoderConfig& config, std::string* error);

}  // namespace chast
