// Intel oneVPL hardware encoder.
//
// One frame, end to end:
//
//   FrameInfo::nv12_texture (GPU, in the capture device)
//     -> mfxMemoryInterface::ImportFrameSurface (D3D11 texture)
//     -> MFXVideoENCODE_EncodeFrameAsync       (input: video memory)
//     -> MFXVideoCORE_SyncOperation
//     -> mfxBitstream bytes
//
// The import is what keeps the frame off the CPU: the runtime takes the NV12
// texture in VRAM and either maps it (IMPORT_SHARED) or GPU-copies it into its
// internal surface pool (IMPORT_COPY). Never a system-memory upload, never a
// readback -- there is no code path here that stages the image through RAM.
// This machine's driver answers MFX_ERR_MEMORY_ALLOC to IMPORT_SHARED on NV12
// textures (Intel's own hello-sharing-dx11-import fails the same way), so
// IMPORT_COPY is what actually runs; import_surface() records which one the
// runtime negotiated.
//
// Settings follow RustDesk's VRAM encoder (hwcodec's util_encode::set_av_codec_ctx
// and set_rate_control): CBR, no B-frames, no periodic key frames, one slice,
// low-delay, H.264 High / HEVC Main, BT.601 limited video signal info.

#ifndef ONEVPL_EXPERIMENTAL
#define ONEVPL_EXPERIMENTAL  // mfxMemoryInterface / mfxSurfaceD3D11Tex2D
#endif

#include "chast/encoder.h"

#include <d3d10.h>

#include <vpl/mfxdefs.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxmemory.h>
#include <vpl/mfxstructures.h>
#include <vpl/mfxvideo.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "chast/com_ptr.h"

namespace chast {

namespace {

constexpr UINT kAsyncDepth = 1;
constexpr mfxU32 kBitstreamBufferSize = 4u * 1024u * 1024u;
// MFXVideoCORE_SyncOperation polling step; the encode itself is a few ms.
constexpr mfxU32 kSyncWaitMs = 100;
constexpr int kMaxSyncPolls = 100;  // 10 s before giving up on a frame

const char* mfx_status_name(mfxStatus sts) {
    switch (sts) {
        case MFX_ERR_NONE: return "MFX_ERR_NONE";
        case MFX_ERR_UNKNOWN: return "MFX_ERR_UNKNOWN";
        case MFX_ERR_NULL_PTR: return "MFX_ERR_NULL_PTR";
        case MFX_ERR_UNSUPPORTED: return "MFX_ERR_UNSUPPORTED";
        case MFX_ERR_MEMORY_ALLOC: return "MFX_ERR_MEMORY_ALLOC";
        case MFX_ERR_NOT_ENOUGH_BUFFER: return "MFX_ERR_NOT_ENOUGH_BUFFER";
        case MFX_ERR_INVALID_HANDLE: return "MFX_ERR_INVALID_HANDLE";
        case MFX_ERR_DEVICE_LOST: return "MFX_ERR_DEVICE_LOST";
        case MFX_ERR_NOT_INITIALIZED: return "MFX_ERR_NOT_INITIALIZED";
        case MFX_ERR_MORE_DATA: return "MFX_ERR_MORE_DATA";
        case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
        case MFX_ERR_DEVICE_FAILED: return "MFX_ERR_DEVICE_FAILED";
        case MFX_ERR_GPU_HANG: return "MFX_ERR_GPU_HANG";
        case MFX_ERR_UNDEFINED_BEHAVIOR: return "MFX_ERR_UNDEFINED_BEHAVIOR";
        case MFX_WRN_IN_EXECUTION: return "MFX_WRN_IN_EXECUTION";
        case MFX_WRN_DEVICE_BUSY: return "MFX_WRN_DEVICE_BUSY";
        default: return "MFX status";
    }
}

Error mfx_error(mfxStatus sts, const char* what) {
    // Device loss and GPU hangs are the ones capture has to hear about: the
    // caller treats DeviceRemoved as a reason to rebuild the whole pipeline.
    Status status = Status::CaptureFailed;
    if (sts == MFX_ERR_DEVICE_LOST || sts == MFX_ERR_GPU_HANG) {
        status = Status::DeviceRemoved;
    } else if (sts == MFX_ERR_UNSUPPORTED || sts == MFX_ERR_NOT_INITIALIZED) {
        status = Status::Unsupported;
    } else if (sts == MFX_ERR_NULL_PTR || sts == MFX_ERR_INVALID_HANDLE) {
        status = Status::InvalidCall;
    }
    return Error(status, std::string(what) + " failed: " + mfx_status_name(sts) + " (" +
                           std::to_string(static_cast<int>(sts)) + ")");
}

// Rounds a dimension up to oneVPL's 16-pixel alignment.
mfxU16 align16(UINT value) {
    return static_cast<mfxU16>((value + 15u) & ~15u);
}

// True when the LUID identifies this adapter. VPL's mfxImplDescription does
// not carry a LUID, so the adapter is matched by walking DXGI in the same
// order the dispatcher chose the device: the implementation selected with
// MFXCreateSession is tied to the adapter VPL picks, which is adapter 0 of the
// Intel device. If the capture device is not on that adapter,
// D3D11 device sharing fails and the import is refused, which is reported at
// creation.
struct ImplementationInfo {
    std::string name;
};

ImplementationInfo describe_implementation(mfxLoader loader, mfxU32 index) {
    ImplementationInfo info;
    mfxHDL raw = nullptr;
    if (MFXEnumImplementations(loader, index, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &raw) != MFX_ERR_NONE ||
        !raw) {
        return info;
    }
    const auto* desc = reinterpret_cast<const mfxImplDescription*>(raw);
    info.name = desc->ImplName;
    if (desc->Dev.DeviceID[0] != '\0') {
        if (!info.name.empty()) {
            info.name += " on ";
        }
        info.name += desc->Dev.DeviceID;
    }
    MFXDispReleaseImplDescription(loader, raw);
    return info;
}

// Common D3D11 device lifetime for VPL. MFXVideoCORE_SetHandle does not take a
// reference on the device, and the encoder must not release a device the
// capturer still owns, so the encoder holds its own reference for the session's
// lifetime.
class MfxEncoder final : public IVideoEncoder {
public:
    ~MfxEncoder() override { close(); }

    Error initialize(ID3D11Device* device, const EncoderConfig& config);
    Error encode(ID3D11Texture2D* nv12, uint64_t pts_ms, bool force_keyframe,
                 std::vector<EncodedPacket>& out) override;
    Error set_bitrate(UINT bits_per_second) override;
    UINT bitrate() const noexcept override { return bitrate_; }
    VideoCodec codec() const noexcept override { return codec_; }
    UINT width() const noexcept override { return width_; }
    UINT height() const noexcept override { return height_; }
    const std::string& implementation() const noexcept override { return implementation_; }
    bool shared_import() const noexcept override { return import_shared_; }

private:
    void close();
    Error import_surface(ID3D11Texture2D* texture, mfxFrameSurface1** surface);
    Error submit(mfxFrameSurface1* surface, bool force_keyframe,
                 std::vector<EncodedPacket>& out, uint64_t pts_ms);

    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    bool encode_open_ = false;
    mfxMemoryInterface* memory_interface_ = nullptr;
    ComPtr<ID3D11Device> device_;
    // The NV12 texture imported on the previous call, and the surface the
    // runtime handed back for it. Re-importing is skipped only when the runtime
    // mapped the texture (shared import); a copy import is re-taken per frame,
    // see import_surface.
    ID3D11Texture2D* imported_texture_ = nullptr;
    mfxFrameSurface1* imported_surface_ = nullptr;
    // True when the runtime mapped the texture (IMPORT_SHARED) instead of
    // GPU-copying it (IMPORT_COPY); see import_surface.
    bool import_shared_ = false;
    mfxBitstream bitstream_{};
    std::vector<mfxU8> bitstream_data_;
    // The parameters the encoder was initialised with (as corrected by
    // MFXVideoENCODE_Query). MFXVideoENCODE_Reset takes a complete description,
    // not a patch, so set_bitrate() edits these and resets with them; the
    // signal buffer lives beside them because the extension list points into
    // it.
    mfxExtVideoSignalInfo signal_{};
    mfxVideoParam params_{};

    VideoCodec codec_ = VideoCodec::H264;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT bitrate_ = 0;
    UINT framerate_ = 30;
    UINT gop_size_ = 0;
    std::string implementation_;
};

Error MfxEncoder::initialize(ID3D11Device* device, const EncoderConfig& config) {
    if (!device) {
        return Error(Status::InvalidCall, "encoder needs a D3D11 device");
    }
    if (config.width == 0 || config.height == 0) {
        return Error(Status::InvalidCall, "encoder needs a frame size");
    }
    device_ = ComPtr<ID3D11Device>(device);
    device_.add_ref();

    codec_ = config.codec;
    width_ = config.width;
    height_ = config.height;
    framerate_ = config.framerate ? config.framerate : 30;
    gop_size_ = config.gop_size;
    bitrate_ = config.bitrate;
    if (bitrate_ == 0) {
        bitrate_ = hardware_bitrate(width_, height_, config.quality_ratio,
                                    codec_ == VideoCodec::H264);
    }

    const mfxU32 codec_id = codec_ == VideoCodec::H264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;

    // Dispatcher: pick hardware implementations that expose exactly the
    // surface-sharing configuration the import needs. Intel's hello-sharing
    // example does the same, and a runtime without the D3D11 sharing extension
    // is filtered out here instead of failing mid-frame.
    loader_ = MFXLoad();
    if (!loader_) {
        return Error(Status::Unsupported, "MFXLoad failed: no oneVPL runtime is installed");
    }

    mfxConfig cfg[4];
    mfxVariant value[4];

    cfg[0] = MFXCreateConfig(loader_);
    value[0].Type = MFX_VARIANT_TYPE_U32;
    value[0].Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    mfxStatus sts = MFXSetConfigFilterProperty(
        cfg[0], reinterpret_cast<const mfxU8*>("mfxImplDescription.Impl"), value[0]);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXSetConfigFilterProperty(Impl)");
    }

    cfg[1] = MFXCreateConfig(loader_);
    value[1].Type = MFX_VARIANT_TYPE_U32;
    value[1].Data.U32 = codec_id;
    sts = MFXSetConfigFilterProperty(
        cfg[1],
        reinterpret_cast<const mfxU8*>(
            "mfxImplDescription.mfxEncoderDescription.encoder.CodecID"),
        value[1]);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXSetConfigFilterProperty(CodecID)");
    }

    cfg[2] = MFXCreateConfig(loader_);
    value[2].Type = MFX_VARIANT_TYPE_U32;
    value[2].Data.U32 = MFX_ACCEL_MODE_VIA_D3D11;
    sts = MFXSetConfigFilterProperty(
        cfg[2], reinterpret_cast<const mfxU8*>("mfxImplDescription.AccelerationMode"), value[2]);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXSetConfigFilterProperty(AccelerationMode)");
    }

    // The import path is experimental API that must be advertised by the
    // runtime: D3D11 text2D surfaces used as encode input. The sharing flag is
    // deliberately not part of the filter -- this machine's runtime only
    // accepts a copy import on a render-target NV12 texture (see
    // import_surface), and the filter must not hide it.
    cfg[3] = MFXCreateConfig(loader_);
    value[3].Type = MFX_VARIANT_TYPE_U32;
    value[3].Data.U32 = MFX_SURFACE_TYPE_D3D11_TEX2D;
    sts = MFXSetConfigFilterProperty(
        cfg[3], reinterpret_cast<const mfxU8*>("mfxSurfaceTypesSupported.surftype.SurfaceType"),
        value[3]);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXSetConfigFilterProperty(SurfaceType)");
    }
    value[3].Data.U32 = MFX_SURFACE_COMPONENT_ENCODE;
    sts = MFXSetConfigFilterProperty(
        cfg[3],
        reinterpret_cast<const mfxU8*>("mfxSurfaceTypesSupported.surftype.surfcomp.SurfaceComponent"),
        value[3]);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXSetConfigFilterProperty(SurfaceComponent)");
    }

    // The dispatcher has already filtered the implementations down to the one
    // that encodes this codec on this adapter with D3D11 acceleration and
    // admits a shared D3D11 texture on the encode input, so index 0 is the
    // session we want. Building the session any other way (MFXInitEx, or
    // MFXInitialize directly) bypasses that selection.
    sts = MFXCreateSession(loader_, 0, &session_);
    if (sts != MFX_ERR_NONE) {
        // No driver on this adapter. OneVPL's software (or another vendor's)
        // runtime cannot take a D3D11 texture, so there is nothing to fall
        // back to -- and copy-based import is exactly what this project rules
        // out.
        return Error(Status::Unsupported,
                     std::string("oneVPL has no D3D11 hardware session for ") +
                         (codec_ == VideoCodec::H264 ? "H.264" : "HEVC") + ": " +
                         mfx_status_name(sts));
    }
    implementation_ = describe_implementation(loader_, 0).name;

    // The runtime encodes on its own internal thread and drives the D3D11
    // immediate context from there. Without multithread protection on the
    // device the runtime refuses to take it (SetHandle answers
    // MFX_ERR_UNDEFINED_BEHAVIOR). hwcodec and Intel's examples both set it
    // before handing the device over.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.get()->QueryInterface(IID_PPV_ARGS(multithread.put())))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    sts = MFXVideoCORE_SetHandle(session_, MFX_HANDLE_D3D11_DEVICE,
                                 reinterpret_cast<mfxHDL>(device_.get()));
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoCORE_SetHandle(D3D11)");
    }

    // Query and Init with the same description so the runtime can allocate its
    // internal surface pool around the imported frames.
    mfxVideoParam& params = params_;
    params = {};
    params.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    params.AsyncDepth = kAsyncDepth;
    mfxInfoMFX& mfx = params.mfx;
    mfx.CodecId = codec_id;
    mfx.LowPower = MFX_CODINGOPTION_OFF;
    mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    mfx.TargetKbps = static_cast<mfxU16>((std::max)(1u, bitrate_ / 1000u));
    // A stream with no periodic key frames; RustDesk's gop is MAX_GOP unless
    // the caller configured an interval, and this encoder asks per frame.
    mfx.GopPicSize =
        static_cast<mfxU16>((std::clamp)(gop_size_ == 0 ? 0xFFFFu : gop_size_, 1u, 0xFFFFu));
    mfx.GopRefDist = 1;  // no B-frames
    mfx.IdrInterval = 0;  // every I-frame an IDR (H.264)
    mfx.CodecProfile = static_cast<mfxU16>(
        codec_ == VideoCodec::H264 ? MFX_PROFILE_AVC_HIGH : MFX_PROFILE_HEVC_MAIN);

    mfxFrameInfo& info = mfx.FrameInfo;
    info.FourCC = MFX_FOURCC_NV12;
    info.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    info.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    // Video memory wants 16-pixel alignment; the NV12 pass publishes exactly
    // that, with the real frame size carried as the crop.
    info.Width = align16(width_);
    info.Height = align16(height_);
    info.CropX = 0;
    info.CropY = 0;
    info.CropW = static_cast<mfxU16>(width_);
    info.CropH = static_cast<mfxU16>(height_);
    info.FrameRateExtN = framerate_;
    info.FrameRateExtD = 1;
    info.AspectRatioW = 0;
    info.AspectRatioH = 0;

    // Colour signalling, matching RustDesk's AVCOL_SPC_SMPTE170M / MPEG range
    // and Intel's BT.601 limited conversion done by the NV12 pass.
    mfxExtVideoSignalInfo& signal = signal_;
    signal = {};
    signal.Header.BufferId = MFX_EXTBUFF_VIDEO_SIGNAL_INFO;
    signal.Header.BufferSz = sizeof(signal);
    signal.VideoFormat = 5;  // unspecified video format
    signal.VideoFullRange = 0;
    signal.ColourDescriptionPresent = 1;
    signal.ColourPrimaries = 6;               // SMPTE 170M
    signal.TransferCharacteristics = 6;       // SMPTE 170M
    signal.MatrixCoefficients = 6;            // SMPTE 170M (BT.601)
    mfxExtBuffer* extensions[] = {&signal.Header};
    params.ExtParam = extensions;
    params.NumExtParam = 1;

    sts = MFXVideoENCODE_Query(session_, &params, &params);
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoENCODE_Query");
    }

    sts = MFXVideoENCODE_Init(session_, &params);
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoENCODE_Init");
    }
    encode_open_ = true;

    bitstream_data_.resize(kBitstreamBufferSize);
    bitstream_ = {};
    bitstream_.MaxLength = kBitstreamBufferSize;
    bitstream_.Data = bitstream_data_.data();

    sts = MFXGetMemoryInterface(session_, &memory_interface_);
    if (sts != MFX_ERR_NONE || !memory_interface_) {
        return mfx_error(sts, "MFXGetMemoryInterface");
    }
    return Error::success();
}

Error MfxEncoder::import_surface(ID3D11Texture2D* texture, mfxFrameSurface1** surface) {
    // Reusing the previous import is only valid when the runtime mapped the
    // texture: then the surface reads whatever the capture pipeline last wrote
    // into that texture. An import by copy is a snapshot taken at import time,
    // so it has to be redone for every frame or the encoder would keep seeing
    // the first image.
    if (import_shared_ && texture == imported_texture_ && imported_surface_) {
        *surface = imported_surface_;
        return Error::success();
    }
    if (imported_surface_ && imported_surface_->FrameInterface) {
        imported_surface_->FrameInterface->Release(imported_surface_);
        imported_surface_ = nullptr;
    }
    imported_texture_ = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) {
        return Error(Status::InvalidCall, "encoder input is not NV12");
    }
    if (desc.Width < align16(width_) || desc.Height < align16(height_)) {
        return Error(Status::InvalidCall, "encoder input texture is smaller than the frame");
    }

    // MFX_SURFACE_FLAG_IMPORT_SHARED asks the runtime to map the texture, and
    // this machine's driver refuses it (MFX_ERR_MEMORY_ALLOC) for NV12
    // textures bound as decoder, render target, or both, even though it
    // advertises the flag in its capabilities; Intel's own
    // hello-sharing-dx11-import fails the same way here. The copy import is
    // the one that works: the texture never leaves the GPU, and the runtime
    // GPU-copies it into its internal surface pool (device-to-device, not the
    // CPU upload a system-memory import would do). The flag the runtime
    // actually negotiated is recorded, so a driver that starts honouring
    // shared import shows up in the log instead of being used silently.
    mfxSurfaceD3D11Tex2D external{};
    external.SurfaceInterface.Header.SurfaceType = MFX_SURFACE_TYPE_D3D11_TEX2D;
    external.SurfaceInterface.Header.SurfaceFlags = MFX_SURFACE_FLAG_IMPORT_COPY;
    external.SurfaceInterface.Header.StructSize = sizeof(external);
    external.texture2D = reinterpret_cast<mfxHDL>(texture);

    mfxStatus sts = memory_interface_->ImportFrameSurface(
        memory_interface_, MFX_SURFACE_COMPONENT_ENCODE, &external.SurfaceInterface.Header,
        surface);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "ImportFrameSurface(NV12)");
    }
    import_shared_ =
        (external.SurfaceInterface.Header.SurfaceFlags & MFX_SURFACE_FLAG_IMPORT_SHARED) != 0;

    imported_texture_ = texture;
    imported_surface_ = *surface;
    return Error::success();
}

Error MfxEncoder::encode(ID3D11Texture2D* nv12, uint64_t pts_ms, bool force_keyframe,
                         std::vector<EncodedPacket>& out) {
    if (!session_ || !encode_open_) {
        return Error(Status::NotCurrentlyAvailable, "encoder is not initialized");
    }
    if (!nv12) {
        return Error(Status::InvalidCall, "encoder got a null NV12 texture");
    }
    mfxFrameSurface1* surface = nullptr;
    Error err = import_surface(nv12, &surface);
    if (!err.ok()) {
        return err;
    }
    return submit(surface, force_keyframe, out, pts_ms);
}

Error MfxEncoder::submit(mfxFrameSurface1* surface, bool force_keyframe,
                         std::vector<EncodedPacket>& out, uint64_t pts_ms) {
    mfxEncodeCtrl control{};
    mfxEncodeCtrl* control_ptr = nullptr;
    if (force_keyframe) {
        control.FrameType = static_cast<mfxU16>(MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR);
        control_ptr = &control;
    }

    surface->Data.TimeStamp = pts_ms;
    bitstream_.DataOffset = 0;
    bitstream_.DataLength = 0;

    mfxSyncPoint sync = nullptr;
    mfxStatus sts = MFXVideoENCODE_EncodeFrameAsync(session_, control_ptr, surface, &bitstream_,
                                                   &sync);
    // The surface stays referenced by the runtime until the operation
    // completes; nothing to release here (the import keeps it alive).
    if (sts == MFX_WRN_DEVICE_BUSY) {
        // Transient back-pressure from the GPU; wait for outstanding work, as
        // Intel's examples do, then retry once.
        MFXVideoCORE_SyncOperation(session_, nullptr, kSyncWaitMs);
        sts = MFXVideoENCODE_EncodeFrameAsync(session_, control_ptr, surface, &bitstream_, &sync);
    }
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoENCODE_EncodeFrameAsync");
    }
    // MFX_ERR_MORE_DATA with a valid sync point is the normal low-delay case:
    // the encoder accepted the frame but has no output yet.
    if (sync) {
        int polls = 0;
        do {
            sts = MFXVideoCORE_SyncOperation(session_, sync, kSyncWaitMs);
            ++polls;
        } while (sts == MFX_WRN_IN_EXECUTION && polls < kMaxSyncPolls);
        if (sts == MFX_WRN_IN_EXECUTION) {
            return Error(Status::CaptureFailed, "encode did not complete within 10 s");
        }
        if (sts != MFX_ERR_NONE) {
            return mfx_error(sts, "MFXVideoCORE_SyncOperation");
        }
    }

    if (bitstream_.DataLength > 0 && bitstream_.Data) {
        EncodedPacket packet;
        packet.data.assign(bitstream_.Data + bitstream_.DataOffset,
                           bitstream_.Data + bitstream_.DataOffset + bitstream_.DataLength);
        packet.pts_ms = bitstream_.TimeStamp;
        // The runtime reports the frame type it actually encoded; an IDR can
        // only be the I-frame carrying the IDR NAL.
        packet.idr = (bitstream_.FrameType & MFX_FRAMETYPE_IDR) != 0;
        packet.keyframe = packet.idr || (bitstream_.FrameType & MFX_FRAMETYPE_I) != 0;
        out.push_back(std::move(packet));
        bitstream_.DataLength = 0;
        bitstream_.DataOffset = 0;
    }
    return Error::success();
}

Error MfxEncoder::set_bitrate(UINT bits_per_second) {
    if (!session_ || !encode_open_) {
        return Error(Status::NotCurrentlyAvailable, "encoder is not initialized");
    }
    const UINT kbps = (std::max)(1u, bits_per_second / 1000u);
    // The reset needs a full description, not just the fields that change: the
    // runtime compares it against what is running. Everything else is carried
    // over from initialisation.
    mfxVideoParam reset = params_;
    reset.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    reset.mfx.TargetKbps = static_cast<mfxU16>((std::min)(kbps, 0xFFFFu));
    mfxExtBuffer* extensions[] = {&signal_.Header};
    reset.ExtParam = extensions;
    reset.NumExtParam = 1;
    const mfxStatus sts = MFXVideoENCODE_Reset(session_, &reset);
    // A positive status is a warning the runtime already resolved (mfx-gen
    // answers MFX_WRN_INCOMPATIBLE_VIDEO_PARAM while applying the new rate) and
    // the reset has taken effect.
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoENCODE_Reset(bitrate)");
    }
    params_.mfx.TargetKbps = reset.mfx.TargetKbps;
    bitrate_ = bits_per_second;
    return Error::success();
}

void MfxEncoder::close() {
    if (imported_surface_) {
        if (imported_surface_->FrameInterface) {
            imported_surface_->FrameInterface->Release(imported_surface_);
        }
        imported_surface_ = nullptr;
    }
    imported_texture_ = nullptr;
    if (session_) {
        if (encode_open_) {
            MFXVideoENCODE_Close(session_);
            encode_open_ = false;
        }
        MFXClose(session_);
        session_ = nullptr;
    }
    if (loader_) {
        MFXUnload(loader_);
        loader_ = nullptr;
    }
    memory_interface_ = nullptr;
    device_.reset();
}

}  // namespace

UINT base_bitrate(UINT width, UINT height) {
    // Verbatim from RustDesk's libs/scrap/src/common/codec.rs base_bitrate():
    // the preset whose pixel count is closest to this frame's, scaled by the
    // pixel-count ratio.
    struct Preset {
        UINT w, h, bitrate;
    };
    static constexpr Preset kPresets[] = {
        {640, 480, 400},     {800, 600, 500},     {1024, 768, 800},
        {1280, 720, 1000},   {1366, 768, 1100},   {1440, 900, 1300},
        {1600, 900, 1500},   {1920, 1080, 2073},  {2048, 1080, 2200},
        {2560, 1440, 3000},  {3440, 1440, 4000},  {3840, 2160, 5000},
        {7680, 4320, 12000},
    };
    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    const Preset* best = &kPresets[7];  // 1080p fallback, like RustDesk's unwrap_or
    uint64_t best_distance = ~uint64_t{0};
    for (const Preset& preset : kPresets) {
        const uint64_t preset_pixels = static_cast<uint64_t>(preset.w) * preset.h;
        const uint64_t distance = preset_pixels >= pixels ? preset_pixels - pixels
                                                          : pixels - preset_pixels;
        if (distance < best_distance) {
            best_distance = distance;
            best = &preset;
        }
    }
    if (pixels == 0) {
        return best->bitrate;
    }
    const uint64_t preset_pixels = static_cast<uint64_t>(best->w) * best->h;
    const double scaled = static_cast<double>(best->bitrate) *
                          (static_cast<double>(pixels) / static_cast<double>(preset_pixels));
    return static_cast<UINT>(scaled + 0.5);
}

UINT hardware_bitrate(UINT width, UINT height, float quality_ratio, bool h264) {
    // HwRamEncoder::calc_bitrate(): base_bitrate() is in kbit/s, and the codec
    // factor widens it for small screens where a flat ratio would starve the
    // encoder, tapering off above 2000 kbit/s.
    const float base = static_cast<float>(base_bitrate(width, height)) * quality_ratio;
    const float threshold = 2000.0f;
    const float decay_rate = 0.001f;
    const float factor = h264 ? (base > threshold ? 1.0f + 1.0f / (1.0f + (base - threshold) * decay_rate)
                                                 : 2.0f)
                              : (base > threshold ? 1.0f + 0.5f / (1.0f + (base - threshold) * decay_rate)
                                                  : 1.5f);
    const UINT kbps = static_cast<UINT>(base * factor);
    return (std::max)(1u, kbps) * 1000u;
}

bool hardware_encoder_available(VideoCodec codec) {
    mfxLoader loader = MFXLoad();
    if (!loader) {
        return false;
    }
    const mfxU32 codec_id = codec == VideoCodec::H264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;
    mfxConfig cfg = MFXCreateConfig(loader);
    mfxVariant value{};
    value.Type = MFX_VARIANT_TYPE_U32;
    value.Data.U32 = codec_id;
    MFXSetConfigFilterProperty(
        cfg,
        reinterpret_cast<const mfxU8*>(
            "mfxImplDescription.mfxEncoderDescription.encoder.CodecID"),
        value);
    bool found = false;
    for (mfxU32 i = 0;; ++i) {
        mfxHDL raw = nullptr;
        if (MFXEnumImplementations(loader, i, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &raw) != MFX_ERR_NONE ||
            !raw) {
            break;
        }
        const auto* desc = reinterpret_cast<const mfxImplDescription*>(raw);
        if (desc->Impl == MFX_IMPL_TYPE_HARDWARE) {
            for (mfxU16 e = 0; e < desc->Enc.NumCodecs; ++e) {
                if (desc->Enc.Codecs[e].CodecID == codec_id) {
                    found = true;
                }
            }
        }
        MFXDispReleaseImplDescription(loader, raw);
        if (found) {
            break;
        }
    }
    MFXUnload(loader);
    return found;
}

std::unique_ptr<IVideoEncoder> create_encoder(ID3D11Device* device, const LUID& adapter_luid,
                                              const EncoderConfig& config, std::string* error) {
    (void)adapter_luid;  // see MfxEncoder::initialize: the device decides the adapter
    auto encoder = std::make_unique<MfxEncoder>();
    const Error err = encoder->initialize(device, config);
    if (!err.ok()) {
        if (error) {
            *error = err.to_string();
        }
        return nullptr;
    }
    return encoder;
}

}  // namespace chast
