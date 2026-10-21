#pragma once

// Hardware video decoder for the access units the encoder produces.
//
// Input is an Annex B access unit as it arrives from the encoder (start-code
// prefixed NAL units, SPS/PPS repeated on key frames). Decoding runs through
// FFmpeg's D3D11VA backend on the display GPU. There are two output modes:
//
//   * system memory (default): the hardware picture is transferred to the CPU
//     for measurement data, used by the verification tools. If D3D11VA is not
//     available the decoder falls back to FFmpeg's software decoders;
//   * D3D11 video memory: when DecoderConfig::d3d11_device and gpu_handler are
//     set, every picture is handed to the handler as an NV12 texture while the
//     decoder still owns the surface. The image never touches the CPU, which
//     is what the display path needs. There is no software fallback in this
//     mode: the handler's device is the only decoder the caller wants.
//
// The decoder is deliberately independent of the capture device: it runs on a
// different machine than the encoder and must not try to pin itself to the
// sender's adapter.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chast/encoder.h"
#include "chast/error.h"

namespace chast {

// One decoded picture, with the measurements the verification harness reports.
struct DecodedFrame {
    uint64_t pts_ms = 0;
    UINT width = 0;   // visible (cropped) size
    UINT height = 0;
    UINT pitch = 0;   // luma row stride of the decoded surface
    UINT crop_x = 0;
    UINT crop_y = 0;
    // FNV-1a over the visible luma samples, row by row without padding. Two
    // frames with the same content hash the same even when their surfaces
    // differ in stride.
    uint64_t hash = 0;
    double luma_mean = 0.0;
    double luma_variance = 0.0;
    // Tightly packed NV12 of the visible rectangle, only filled when
    // DecoderConfig::copy_frames is set.
    std::vector<uint8_t> nv12;
};

// One decoded picture in D3D11 video memory. The texture borrows the decoder's
// surface and is only valid for the duration of the handler call.
struct GpuFrame {
    ID3D11Texture2D* texture = nullptr;
    // Array slice holding this picture: FFmpeg's D3D11VA pool is one texture
    // array, so the renderer must bind this subresource rather than slice 0.
    uint32_t subresource = 0;
    uint32_t width = 0;   // visible (cropped) size
    uint32_t height = 0;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint64_t pts_ms = 0;
};

// Called synchronously from decode()/flush() before the surface is recycled.
// Returning an error aborts decoding.
using GpuFrameHandler = std::function<Error(const GpuFrame&)>;

struct DecoderConfig {
    VideoCodec codec = VideoCodec::H264;
    // Keep the decoded pixels, not just the statistics. Off by default: a
    // 1080p frame is 3 MB, and the transport milestone only checks content.
    bool copy_frames = false;
    // When both are set, decode to video memory on this device and deliver
    // every picture through gpu_handler. `out` is left empty in that mode.
    ID3D11Device* d3d11_device = nullptr;
    GpuFrameHandler gpu_handler;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // Decodes one access unit. `keyframe` tells the decoder that `data` starts
    // with a sequence header, which is what lets it initialise; a stream that
    // begins with a delta frame fails with NotCurrentlyAvailable until a key
    // frame arrives. Appends every picture that completed to `out` (normally
    // one, none while the decoder's reorder delay fills, possibly more on
    // flush). `pts_ms` is attached to pictures produced from this access unit.
    // In GPU mode the handler is called instead and `out` stays empty.
    virtual Error decode(const uint8_t* data, size_t size, uint64_t pts_ms, bool keyframe,
                         std::vector<DecodedFrame>& out) = 0;

    // Drains pictures still held by the decoder's delay. Call once after the
    // last access unit; decode() after flush() is not supported.
    virtual Error flush(std::vector<DecodedFrame>& out) = 0;

    virtual bool initialized() const noexcept = 0;
    virtual UINT width() const noexcept = 0;
    virtual UINT height() const noexcept = 0;
    virtual const std::string& implementation() const noexcept = 0;
};

// Creates a decoder for `config.codec`. Fails with Unsupported when the
// FFmpeg build has no decoder for that codec, or when GPU mode was requested
// and D3D11VA cannot be initialised on the supplied device. The decode context
// itself is opened on the first key frame, because that is the first access
// unit that carries a sequence header.
std::unique_ptr<IVideoDecoder> create_decoder(const DecoderConfig& config, std::string* error);

}  // namespace chast
