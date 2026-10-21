// Intel hardware decoder: oneVPL, system memory or D3D11 video memory.
//
// The decoder is the mirror image of vpl_encoder.cc but is not pinned to the
// capture adapter: it runs on the receiving machine and may sit on a different
// GPU entirely. It requests a hardware implementation of the stream's codec.
//
// On legacy runtimes (mfxhw64, API 1.x) video-memory decode needs an
// application-owned surface pool, so the decoder creates NV12 textures through
// its own frame allocator. On oneVPL 2.x runtimes the surface is allocated
// internally and exported with GetNativeHandle. Either way the picture is
// delivered as an ID3D11Texture2D* while the decoder still owns the surface,
// which is what lets the renderer convert and display it without a copy.
//
// Initialisation is deferred to the first key frame: DecodeHeader needs a
// sequence header (SPS/PPS), and the server always starts its stream with an
// IDR. Until then decode() answers NotCurrentlyAvailable and the caller asks
// for a key frame.

#include "chast/decoder.h"

#include <d3d10.h>
#include <d3d11.h>

#include <vpl/mfxdefs.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxstructures.h>
#include <vpl/mfxvideo.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chast/com_ptr.h"

namespace chast {

namespace {

constexpr mfxU32 kSyncTimeoutMs = 10000;
// Extra surfaces beyond what QueryIOSurf asks for: the runtime returns
// MFX_ERR_MORE_SURFACE while its reference pool fills, and having a couple of
// spares keeps that off the steady-state path.
constexpr UINT kSurfaceSlack = 2;
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

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
        case MFX_ERR_MORE_SURFACE: return "MFX_ERR_MORE_SURFACE";
        case MFX_ERR_ABORTED: return "MFX_ERR_ABORTED";
        case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
        case MFX_ERR_INVALID_VIDEO_PARAM: return "MFX_ERR_INVALID_VIDEO_PARAM";
        case MFX_ERR_NOT_FOUND: return "MFX_ERR_NOT_FOUND";
        case MFX_ERR_DEVICE_FAILED: return "MFX_ERR_DEVICE_FAILED";
        case MFX_ERR_MORE_BITSTREAM: return "MFX_ERR_MORE_BITSTREAM";
        case MFX_ERR_GPU_HANG: return "MFX_ERR_GPU_HANG";
        case MFX_ERR_UNDEFINED_BEHAVIOR: return "MFX_ERR_UNDEFINED_BEHAVIOR";
        case MFX_WRN_IN_EXECUTION: return "MFX_WRN_IN_EXECUTION";
        case MFX_WRN_DEVICE_BUSY: return "MFX_WRN_DEVICE_BUSY";
        case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: return "MFX_WRN_INCOMPATIBLE_VIDEO_PARAM";
        default: return "MFX status";
    }
}

Error mfx_error(mfxStatus sts, std::string_view what) {
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

mfxU32 codec_id(VideoCodec codec) {
    return codec == VideoCodec::H264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;
}

const char* codec_name(VideoCodec codec) {
    return codec == VideoCodec::H264 ? "H.264" : "HEVC";
}

struct ImplCandidate {
    mfxU32 index = 0;
    std::string name;
    mfxU16 api_major = 0;
    mfxU16 api_minor = 0;
};

// Restricts the dispatcher to hardware implementations of the stream's codec.
// No acceleration-mode filter: decoding to system memory works on whichever
// device the runtime prefers, and on the receiving machine that is often not
// the capture adapter.
Error filter_decoder(mfxLoader loader, mfxU32 codec) {
    struct Filter {
        const char* name;
        mfxU32 value;
    };
    const Filter filters[] = {
        {"mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE},
        {"mfxImplDescription.mfxDecoderDescription.decoder.CodecID", codec},
    };
    for (const Filter& filter : filters) {
        mfxConfig cfg = MFXCreateConfig(loader);
        if (!cfg) {
            return Error(Status::Unsupported, "MFXCreateConfig failed");
        }
        mfxVariant value{};
        value.Type = MFX_VARIANT_TYPE_U32;
        value.Data.U32 = filter.value;
        const mfxStatus sts = MFXSetConfigFilterProperty(
            cfg, reinterpret_cast<const mfxU8*>(filter.name), value);
        if (sts != MFX_ERR_NONE) {
            return mfx_error(sts,
                             std::string("MFXSetConfigFilterProperty(") + filter.name + ")");
        }
    }
    return Error::success();
}

// One application-owned surface. Data.Locked is the runtime's busy marker:
// non-zero while a decoded picture still lives there.
struct DecoderSurface {
    mfxFrameSurface1 frame{};
    std::vector<mfxU8> storage;
};

// Legacy video-memory surface: the frame's MemId is the texture itself, which
// is the convention mfxhw64 expects from an application allocator.
struct VideoSurface {
    mfxFrameSurface1 frame{};
    ID3D11Texture2D* texture = nullptr;
};

class MfxDecoder final : public IVideoDecoder {
public:
    MfxDecoder(VideoCodec codec, const DecoderConfig& config)
        : codec_(codec),
          copy_frames_(config.copy_frames),
          gpu_(config.d3d11_device != nullptr && static_cast<bool>(config.gpu_handler)),
          gpu_handler_(config.gpu_handler) {
        if (config.d3d11_device) {
            device_ = ComPtr<ID3D11Device>(config.d3d11_device);
            device_.add_ref();
        }
    }
    ~MfxDecoder() override { close(); }

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
    void discard_session();
    Error start(const uint8_t* data, size_t size);
    Error start_on(const ImplCandidate& candidate, const uint8_t* data, size_t size);
    Error pump(mfxBitstream* bitstream, std::vector<DecodedFrame>& out);
    DecoderSurface* free_surface();
    VideoSurface* free_video_surface();
    Error collect(const mfxFrameSurface1& surface, DecodedFrame& out) const;
    Error emit_gpu(const mfxFrameSurface1& surface);
    ID3D11Texture2D* create_texture(UINT width, UINT height) const;

    static mfxStatus MFX_CDECL alloc_frames(mfxHDL pthis, mfxFrameAllocRequest* request,
                                           mfxFrameAllocResponse* response);
    static mfxStatus MFX_CDECL free_frames(mfxHDL pthis, mfxFrameAllocResponse* response);
    static mfxStatus MFX_CDECL lock_frames(mfxHDL pthis, mfxMemId mid, mfxFrameData* data);
    static mfxStatus MFX_CDECL unlock_frames(mfxHDL pthis, mfxMemId mid, mfxFrameData* data);
    static mfxStatus MFX_CDECL get_handle(mfxHDL pthis, mfxMemId mid, mfxHDL* handle);

    VideoCodec codec_;
    bool copy_frames_ = false;
    bool gpu_ = false;
    // API 1.x has no internal surface allocation; the decoder owns the D3D11
    // surface pool and hands it to DecodeFrameAsync instead.
    bool legacy_ = false;
    GpuFrameHandler gpu_handler_;
    ComPtr<ID3D11Device> device_;
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    bool decoder_open_ = false;
    bool initialized_ = false;
    bool flushed_ = false;
    mfxVideoParam params_{};
    std::vector<ImplCandidate> candidates_;
    std::vector<DecoderSurface> surfaces_;
    std::vector<VideoSurface> video_surfaces_;
    mfxFrameAllocator frame_allocator_{};
    UINT width_ = 0;
    UINT height_ = 0;
    std::string implementation_;
};

Error MfxDecoder::open() {
    const bool wants_device = !device_.is_null();
    const bool wants_gpu = static_cast<bool>(gpu_handler_);
    if (wants_device != wants_gpu) {
        return Error(Status::InvalidCall,
                     "GPU decoding needs both a D3D11 device and a frame handler");
    }
    if (gpu_) {
        // The runtime decodes on its own thread and drives the immediate
        // context there while the renderer uses it from the session thread.
        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(multithread.put())))) {
            multithread->SetMultithreadProtected(TRUE);
        }
    }

    loader_ = MFXLoad();
    if (!loader_) {
        return Error(Status::Unsupported,
                     "MFXLoad failed: no oneVPL runtime is installed");
    }
    Error filtered = filter_decoder(loader_, codec_id(codec_));
    if (!filtered.ok()) {
        return filtered;
    }

    std::string names;
    for (mfxU32 index = 0;; ++index) {
        mfxHDL raw = nullptr;
        if (MFXEnumImplementations(loader_, index, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &raw) !=
                MFX_ERR_NONE ||
            !raw) {
            break;
        }
        const auto* desc = reinterpret_cast<const mfxImplDescription*>(raw);
        ImplCandidate candidate;
        candidate.index = index;
        candidate.name = desc->ImplName;
        candidate.api_major = desc->ApiVersion.Major;
        candidate.api_minor = desc->ApiVersion.Minor;
        candidates_.push_back(std::move(candidate));
        if (!names.empty()) {
            names += ", ";
        }
        names += desc->ImplName;
        MFXDispReleaseImplDescription(loader_, raw);
    }
    if (candidates_.empty()) {
        return Error(Status::Unsupported,
                     std::string("no hardware VPL decoder for ") + codec_name(codec_));
    }
    implementation_ = names;
    return Error::success();
}

void MfxDecoder::close() {
    discard_session();
    if (loader_) {
        MFXUnload(loader_);
        loader_ = nullptr;
    }
}

void MfxDecoder::discard_session() {
    if (decoder_open_) {
        MFXVideoDECODE_Close(session_);
        decoder_open_ = false;
    }
    if (session_) {
        MFXClose(session_);
        session_ = nullptr;
    }
    surfaces_.clear();
    for (VideoSurface& surface : video_surfaces_) {
        if (surface.texture) {
            surface.texture->Release();
        }
    }
    video_surfaces_.clear();
    frame_allocator_ = {};
    params_ = {};
    initialized_ = false;
    flushed_ = false;
}

Error MfxDecoder::start_on(const ImplCandidate& candidate, const uint8_t* data, size_t size) {
    mfxStatus sts = MFXCreateSession(loader_, candidate.index, &session_);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXCreateSession");
    }
    legacy_ = candidate.api_major < 2;
    std::fprintf(stderr, "decoder: %s api %u.%u legacy=%d gpu=%d\n", candidate.name.c_str(),
                 candidate.api_major, candidate.api_minor, legacy_ ? 1 : 0, gpu_ ? 1 : 0);

    if (gpu_) {
        sts = MFXVideoCORE_SetHandle(session_, MFX_HANDLE_D3D11_DEVICE,
                                     reinterpret_cast<mfxHDL>(device_.get()));
        if (sts != MFX_ERR_NONE) {
            return mfx_error(sts, "MFXVideoCORE_SetHandle(D3D11)");
        }
        if (legacy_) {
            frame_allocator_ = {};
            frame_allocator_.pthis = this;
            frame_allocator_.Alloc = &MfxDecoder::alloc_frames;
            frame_allocator_.Free = &MfxDecoder::free_frames;
            frame_allocator_.Lock = &MfxDecoder::lock_frames;
            frame_allocator_.Unlock = &MfxDecoder::unlock_frames;
            frame_allocator_.GetHDL = &MfxDecoder::get_handle;
            sts = MFXVideoCORE_SetFrameAllocator(session_, &frame_allocator_);
            if (sts != MFX_ERR_NONE) {
                return mfx_error(sts, "MFXVideoCORE_SetFrameAllocator");
            }
        }
    }

    mfxVideoParam& params = params_;
    params = {};
    params.mfx.CodecId = codec_id(codec_);
    params.IOPattern = static_cast<mfxU16>(
        gpu_ ? MFX_IOPATTERN_OUT_VIDEO_MEMORY : MFX_IOPATTERN_OUT_SYSTEM_MEMORY);
    params.AsyncDepth = 1;

    // DecodeHeader only reads the sequence header; the access unit itself is
    // fed again by decode(), from its first start code, so no bytes are lost.
    mfxBitstream bs{};
    bs.Data = const_cast<mfxU8*>(data);
    bs.DataLength = static_cast<mfxU32>(size);
    bs.MaxLength = static_cast<mfxU32>(size);
    sts = MFXVideoDECODE_DecodeHeader(session_, &bs, &params);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoDECODE_DecodeHeader");
    }

    params.IOPattern = static_cast<mfxU16>(
        gpu_ ? MFX_IOPATTERN_OUT_VIDEO_MEMORY : MFX_IOPATTERN_OUT_SYSTEM_MEMORY);
    sts = MFXVideoDECODE_Query(session_, &params, &params);
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoDECODE_Query");
    }
    sts = MFXVideoDECODE_Init(session_, &params);
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoDECODE_Init");
    }
    decoder_open_ = true;

    // Init may correct the geometry; the surface pool must match what the
    // runtime will actually write.
    sts = MFXVideoDECODE_GetVideoParam(session_, &params);
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoDECODE_GetVideoParam");
    }
    const mfxFrameInfo& info = params.mfx.FrameInfo;
    if (info.Width == 0 || info.Height == 0) {
        return Error(Status::Unsupported, "decoder reported an empty frame size");
    }

    if (!gpu_ || legacy_) {
        mfxFrameAllocRequest request{};
        sts = MFXVideoDECODE_QueryIOSurf(session_, &params, &request);
        if (sts < MFX_ERR_NONE) {
            return mfx_error(sts, "MFXVideoDECODE_QueryIOSurf");
        }
        const UINT count =
            (std::max)({4u, static_cast<UINT>(request.NumFrameMin),
                        static_cast<UINT>(request.NumFrameSuggested)}) +
            kSurfaceSlack;
        const UINT pitch = info.Width;
        const UINT height = info.Height;

        if (gpu_) {
            // mfxhw64 has no internal allocation: the decoder's surface pool is
            // a set of NV12 textures handed to DecodeFrameAsync directly.
            video_surfaces_.resize(count);
            for (VideoSurface& surface : video_surfaces_) {
                surface.texture = create_texture(info.Width, info.Height);
                if (!surface.texture) {
                    return Error(Status::CaptureFailed, "CreateTexture2D(NV12 decode)");
                }
                surface.frame.Info = info;
                surface.frame.Data.MemId = surface.texture;
                surface.frame.Data.Locked = 0;
            }
        } else {
            surfaces_.resize(count);
            for (DecoderSurface& surface : surfaces_) {
                // NV12: a full luma plane followed by an interleaved half-height
                // chroma plane, both with the same stride.
                surface.storage.assign(static_cast<size_t>(pitch) * height * 3 / 2, 0);
                surface.frame.Info = info;
                surface.frame.Data.Pitch = static_cast<mfxU16>(pitch);
                surface.frame.Data.Y = surface.storage.data();
                surface.frame.Data.UV =
                    surface.storage.data() + static_cast<size_t>(pitch) * height;
                surface.frame.Data.Locked = 0;
            }
        }
    }

    width_ = info.CropW ? info.CropW : info.Width;
    height_ = info.CropH ? info.CropH : info.Height;
    implementation_ = candidate.name + (gpu_ ? " (D3D11)" : "");
    initialized_ = true;
    return Error::success();
}

ID3D11Texture2D* MfxDecoder::create_texture(UINT width, UINT height) const {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // Intel rejects NV12 textures that combine DECODER with RENDER_TARGET
    // (CreateTexture2D returns E_INVALIDARG), so the decoder surfaces carry
    // DECODER|SHADER_RESOURCE. The renderer reads them through a video
    // processor input view, which accepts both. MISC_SHARED matches the
    // Media SDK allocator's D3D11 surfaces.
    desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        std::fprintf(stderr, "decoder: CreateTexture2D(NV12 %ux%u) failed: 0x%08lx\n", width,
                     height, static_cast<unsigned long>(hr));
        return nullptr;
    }
    return texture;
}

mfxStatus MFX_CDECL MfxDecoder::alloc_frames(mfxHDL pthis, mfxFrameAllocRequest* request,
                                             mfxFrameAllocResponse* response) {
    if (!pthis || !request || !response) {
        return MFX_ERR_NULL_PTR;
    }
    std::fprintf(stderr,
                 "decoder: alloc type=0x%x fourcc=0x%x %ux%u min=%u suggested=%u\n",
                 request->Type, request->Info.FourCC, request->Info.Width,
                 request->Info.Height, request->NumFrameMin, request->NumFrameSuggested);
    response->mids = nullptr;
    response->NumFrameActual = 0;
    if ((request->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET) == 0 ||
        request->Info.FourCC != MFX_FOURCC_NV12 || request->Info.Width == 0 ||
        request->Info.Height == 0) {
        return MFX_ERR_UNSUPPORTED;
    }
    auto* self = static_cast<MfxDecoder*>(pthis);
    const mfxU16 count =
        (std::max)(static_cast<mfxU16>(1), request->NumFrameSuggested);
    auto** mids = new mfxMemId[count]{};
    for (mfxU32 i = 0; i < count; ++i) {
        ID3D11Texture2D* texture =
            self->create_texture(request->Info.Width, request->Info.Height);
        if (!texture) {
            std::fprintf(stderr, "decoder: alloc: CreateTexture2D failed\n");
            for (mfxU32 j = 0; j < i; ++j) {
                static_cast<ID3D11Texture2D*>(mids[j])->Release();
            }
            delete[] mids;
            return MFX_ERR_MEMORY_ALLOC;
        }
        mids[i] = texture;
    }
    std::fprintf(stderr, "decoder: alloc: %u textures\n", count);
    response->mids = mids;
    response->NumFrameActual = count;
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL MfxDecoder::free_frames(mfxHDL, mfxFrameAllocResponse* response) {
    if (!response) {
        return MFX_ERR_NULL_PTR;
    }
    if (response->mids) {
        for (mfxU32 i = 0; i < response->NumFrameActual; ++i) {
            auto* texture = static_cast<ID3D11Texture2D*>(response->mids[i]);
            if (texture) {
                texture->Release();
            }
        }
        delete[] response->mids;
    }
    response->mids = nullptr;
    response->NumFrameActual = 0;
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL MfxDecoder::lock_frames(mfxHDL, mfxMemId, mfxFrameData*) {
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus MFX_CDECL MfxDecoder::unlock_frames(mfxHDL, mfxMemId, mfxFrameData*) {
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus MFX_CDECL MfxDecoder::get_handle(mfxHDL, mfxMemId mid, mfxHDL* handle) {
    if (!mid || !handle) {
        return MFX_ERR_INVALID_HANDLE;
    }
    // For D3D11 the runtime expects a handle pair: the texture plus the
    // subresource index (0, every surface is a single texture). Writing the
    // bare texture pointer here makes DecodeFrameAsync return
    // MFX_ERR_UNDEFINED_BEHAVIOR.
    auto* pair = reinterpret_cast<mfxHDLPair*>(handle);
    pair->first = mid;
    pair->second = nullptr;
    return MFX_ERR_NONE;
}

Error MfxDecoder::start(const uint8_t* data, size_t size) {
    std::string attempts;
    for (const ImplCandidate& candidate : candidates_) {
        Error err = start_on(candidate, data, size);
        if (err.ok()) {
            return Error::success();
        }
        if (!attempts.empty()) {
            attempts += "; ";
        }
        attempts += candidate.name + ": " + err.to_string();
        discard_session();
    }
    return Error(Status::Unsupported,
                 std::string("no VPL decoder could initialise: ") + attempts);
}

Error MfxDecoder::decode(const uint8_t* data, size_t size, uint64_t pts_ms, bool keyframe,
                         std::vector<DecodedFrame>& out) {
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
        Error opened = start(data, size);
        if (!opened.ok()) {
            return opened;
        }
    }

    mfxBitstream bitstream{};
    bitstream.Data = const_cast<mfxU8*>(data);
    bitstream.DataLength = static_cast<mfxU32>(size);
    bitstream.MaxLength = static_cast<mfxU32>(size);
    bitstream.TimeStamp = pts_ms;
    // Each call carries exactly one access unit, so the runtime may finish it
    // without waiting for the next start code.
    bitstream.DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;
    return pump(&bitstream, out);
}

Error MfxDecoder::flush(std::vector<DecodedFrame>& out) {
    if (!initialized_ || flushed_) {
        return Error::success();
    }
    flushed_ = true;
    return pump(nullptr, out);
}

DecoderSurface* MfxDecoder::free_surface() {
    for (DecoderSurface& surface : surfaces_) {
        if (surface.frame.Data.Locked == 0) {
            return &surface;
        }
    }
    return nullptr;
}

VideoSurface* MfxDecoder::free_video_surface() {
    for (VideoSurface& surface : video_surfaces_) {
        if (surface.frame.Data.Locked == 0) {
            return &surface;
        }
    }
    return nullptr;
}

Error MfxDecoder::pump(mfxBitstream* bitstream, std::vector<DecodedFrame>& out) {
    for (;;) {
        if (gpu_ && !legacy_) {
            // oneVPL 2.x owns the surface pool. GetNativeHandle hands out the
            // D3D11 texture without a copy; the handler must be done with it
            // before the surface is released.
            mfxFrameSurface1* decoded = nullptr;
            mfxSyncPoint sync = nullptr;
            const mfxStatus sts = MFXVideoDECODE_DecodeFrameAsync(
                session_, bitstream, nullptr, &decoded, &sync);
            if (sts == MFX_ERR_MORE_DATA) {
                return Error::success();
            }
            if (sts == MFX_ERR_MORE_SURFACE) {
                continue;
            }
            if (sts == MFX_WRN_DEVICE_BUSY) {
                Sleep(1);
                continue;
            }
            if (sts < MFX_ERR_NONE) {
                return mfx_error(sts, bitstream ? "MFXVideoDECODE_DecodeFrameAsync"
                                                : "MFXVideoDECODE_DecodeFrameAsync (flush)");
            }
            if (!decoded) {
                continue;
            }
            mfxStatus synced = MFX_ERR_NONE;
            if (decoded->FrameInterface && decoded->FrameInterface->Synchronize) {
                do {
                    synced = decoded->FrameInterface->Synchronize(decoded, 1000);
                } while (synced == MFX_WRN_IN_EXECUTION);
            } else if (sync) {
                synced = MFXVideoCORE_SyncOperation(session_, sync, kSyncTimeoutMs);
            }
            if (synced < MFX_ERR_NONE) {
                if (decoded->FrameInterface && decoded->FrameInterface->Release) {
                    decoded->FrameInterface->Release(decoded);
                }
                return mfx_error(synced, "surface synchronize");
            }
            const Error emitted = emit_gpu(*decoded);
            if (decoded->FrameInterface && decoded->FrameInterface->Release) {
                decoded->FrameInterface->Release(decoded);
            }
            if (!emitted.ok()) {
                return emitted;
            }
            continue;
        }

        mfxFrameSurface1* work = nullptr;
        if (gpu_) {
            VideoSurface* surface = free_video_surface();
            if (!surface) {
                return Error(Status::NotCurrentlyAvailable, "no free decoder surface");
            }
            work = &surface->frame;
        } else {
            DecoderSurface* surface = free_surface();
            if (!surface) {
                return Error(Status::NotCurrentlyAvailable, "no free decoder surface");
            }
            work = &surface->frame;
        }

        mfxFrameSurface1* decoded = nullptr;
        mfxSyncPoint sync = nullptr;
        const mfxStatus sts = MFXVideoDECODE_DecodeFrameAsync(
            session_, bitstream, work, &decoded, &sync);
        if (sts == MFX_ERR_MORE_DATA) {
            return Error::success();
        }
        if (sts == MFX_ERR_MORE_SURFACE) {
            continue;
        }
        if (sts == MFX_WRN_DEVICE_BUSY) {
            Sleep(1);
            continue;
        }
        if (sts < MFX_ERR_NONE) {
            return mfx_error(sts, bitstream ? "MFXVideoDECODE_DecodeFrameAsync"
                                            : "MFXVideoDECODE_DecodeFrameAsync (flush)");
        }
        if (!decoded || !sync) {
            continue;
        }
        const mfxStatus synced = MFXVideoCORE_SyncOperation(session_, sync, kSyncTimeoutMs);
        if (synced < MFX_ERR_NONE) {
            return mfx_error(synced, "MFXVideoCORE_SyncOperation");
        }
        if (gpu_) {
            const Error emitted = emit_gpu(*decoded);
            decoded->Data.Locked = 0;
            if (!emitted.ok()) {
                return emitted;
            }
            continue;
        }
        DecodedFrame frame;
        Error measured = collect(*decoded, frame);
        decoded->Data.Locked = 0;
        if (!measured.ok()) {
            return measured;
        }
        out.push_back(std::move(frame));
    }
}

Error MfxDecoder::emit_gpu(const mfxFrameSurface1& surface) {
    ID3D11Texture2D* texture = nullptr;
    if (legacy_) {
        texture = static_cast<ID3D11Texture2D*>(surface.Data.MemId);
    } else if (surface.FrameInterface && surface.FrameInterface->GetNativeHandle) {
        mfxHDL resource = nullptr;
        mfxResourceType type{};
        const mfxStatus sts = surface.FrameInterface->GetNativeHandle(
            const_cast<mfxFrameSurface1*>(&surface), &resource, &type);
        if (sts != MFX_ERR_NONE) {
            return mfx_error(sts, "mfxFrameSurfaceInterface::GetNativeHandle");
        }
        if (type != MFX_RESOURCE_DX11_TEXTURE || !resource) {
            return Error(Status::Unsupported,
                         "decoded surface is not a D3D11 texture");
        }
        texture = static_cast<ID3D11Texture2D*>(resource);
    }
    if (!texture) {
        return Error(Status::CaptureFailed, "decoded surface has no D3D11 texture");
    }

    const mfxFrameInfo& info = surface.Info;
    const UINT width = info.CropW ? info.CropW : info.Width;
    const UINT height = info.CropH ? info.CropH : info.Height;
    if (width == 0 || height == 0) {
        return Error(Status::CaptureFailed, "decoded surface has an empty crop");
    }
    GpuFrame frame;
    frame.texture = texture;
    frame.width = width;
    frame.height = height;
    frame.crop_x = info.CropX;
    frame.crop_y = info.CropY;
    frame.pts_ms = surface.Data.TimeStamp;
    return gpu_handler_(frame);
}

Error MfxDecoder::collect(const mfxFrameSurface1& surface, DecodedFrame& out) const {
    if (!surface.Data.Y || !surface.Data.UV || surface.Data.Pitch == 0) {
        return Error(Status::CaptureFailed, "decoded surface has no NV12 planes");
    }
    const mfxFrameInfo& info = surface.Info;
    const UINT width = info.CropW ? info.CropW : info.Width;
    const UINT height = info.CropH ? info.CropH : info.Height;
    if (width == 0 || height == 0) {
        return Error(Status::CaptureFailed, "decoded surface has an empty crop");
    }
    if (info.CropX + width > surface.Data.Pitch || info.CropY + height > info.Height) {
        return Error(Status::CaptureFailed, "decoded surface crop exceeds its plane");
    }
    const UINT pitch = surface.Data.Pitch;
    const uint8_t* luma = surface.Data.Y + static_cast<size_t>(info.CropY) * pitch + info.CropX;

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
    out.pts_ms = surface.Data.TimeStamp;
    out.width = width;
    out.height = height;
    out.pitch = pitch;
    out.crop_x = info.CropX;
    out.crop_y = info.CropY;
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
        const uint8_t* chroma = surface.Data.UV +
                                static_cast<size_t>(info.CropY / 2) * pitch +
                                (info.CropX & ~1u);
        for (UINT y = 0; y < chroma_height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * chroma_width * 2,
                        chroma + static_cast<size_t>(y) * pitch, chroma_width * 2);
        }
    }
    return Error::success();
}

}  // namespace

std::unique_ptr<IVideoDecoder> create_decoder(const DecoderConfig& config, std::string* error) {
    auto decoder = std::make_unique<MfxDecoder>(config.codec, config);
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
