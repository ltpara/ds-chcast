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
// memory, so the runtime never sees system memory. On oneVPL 2.10+ runtimes it
// either maps the texture (shared import) or GPU-copies it into its internal
// surface pool (import by copy). Legacy Media SDK ABI 1.x runtimes (mfxhw64,
// shipped for older Intel iGPUs) have no memory interface; there the texture is
// wrapped by the encoder's own frame allocator and read in place. None of these
// paths stages the image through RAM or the CPU.

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
    // its reference frames, so the viewer decodes on), while a frame-rate
    // change, a sequence restart or an encoder reopen cannot. The caller must
    // honour it: without the IDR a peer that lost the sequence never recovers,
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
    // allocator; false when it GPU-copied it into its own pool. All of these
    // keep the frame in VRAM; the flag exists so a driver that starts honouring
    // shared import is visible instead of being assumed. Meaningful after the
    // first encode().
    virtual bool shared_import() const noexcept = 0;
};

// Which encoders can be used on this machine, for a UI or an auto-selection
// step. True when a hardware implementation of `codec` (H.264 or HEVC) with
// D3D11 acceleration is enumerated, on either a oneVPL 2.10+ runtime or the
// legacy Media SDK runtime that older Intel iGPUs use.
//
// When `device` is given, implementations are restricted to the adapter that
// device lives on, which is the adapter the frames come from. HEVC counts as
// available when an HEVC implementation exists, or when an H.264 one does:
// create_encoder() may fall back to the AVC tiers for an HEVC request. The
// device is not opened.
bool hardware_encoder_available(VideoCodec codec, ID3D11Device* device = nullptr);

// RustDesk's base_bitrate(): the bitrate table from libs/scrap codec.rs, in
// kbit/s, for a screen of the given size. Unknown sizes fall back to 1080p.
UINT base_bitrate(UINT width, UINT height);

// RustDesk's HwRamEncoder::calc_bitrate(): base_bitrate() scaled by the quality
// ratio and by the codec's headroom factor, in bit/s. This is what a VRAM
// encoder is created with and what set_quality() switches to.
UINT hardware_bitrate(UINT width, UINT height, float quality_ratio, bool h264);

// Creates a hardware encoder for `config.width x config.height` on `device`.
// The device is the adapter selection: VPL implementations reporting a
// different DXGI LUID are rejected, so encoding always stays on the card that
// captured and converted the frame. Encoding tiers are tried in order --
// VDENC+HEVC, VDENC+AVC, VME+AVC -- and each tier must return a test frame
// before it is accepted, so a runtime that opens a session but cannot encode
// does not become the stream's encoder. codec() reports the tier that won; an
// H.264 request starts at VDENC+AVC and never upgrades to HEVC, while an HEVC
// request may fall back to AVC. The frame geometry must be unrotated; the
// encoder has no rotation support, so a caller with a rotated desktop has to
// convert before encoding.
std::unique_ptr<IVideoEncoder> create_encoder(ID3D11Device* device, const EncoderConfig& config,
                                              std::string* error);

}  // namespace chast
