// Intel hardware encoder: oneVPL on newer runtimes, Media SDK ABI 1.x on the
// legacy mfxhw64 runtime that ships for older iGPUs (6th-10th gen).
//
// Implementation selection is pinned to the capture adapter: the capturer's
// D3D11 device is the one that produced the NV12 texture, so only a VPL
// implementation reporting the same DXGI LUID is eligible. Inside that, tiers
// are tried in order until one initialises:
//
//   VDENC + HEVC  ->  VDENC + AVC  ->  VME + AVC
//
// A tier is only accepted after a test frame completes, so a runtime that can
// open a session but not encode does not become the stream's encoder. An HEVC
// request may end on AVC when HEVC encoding is unavailable; an AVC
// request never upgrades. VDENC is mfx.LowPower=ON on runtimes that have both
// encoders; oneVPL 2.10+ implementations are VDENC-only, so VME fails Init and
// the ladder stops there.
//
// One frame, end to end:
//
//   FrameInfo::nv12_texture (GPU, in the capture device)
//     -> either ImportFrameSurface (oneVPL 2.10+)
//        or a surface wrapping the texture (mfxhw64)
//     -> MFXVideoENCODE_EncodeFrameAsync       (input: video memory)
//     -> MFXVideoCORE_SyncOperation
//     -> mfxBitstream bytes
//
// Neither path stages the frame through CPU memory. The 2.10 path either maps
// the texture (IMPORT_SHARED) or GPU-copies it into the runtime's surface pool
// (IMPORT_COPY); shared import is requested first, and import_surface()
// records which one actually ran. The legacy path has no memory interface:
// EncodeFrameAsync is handed the NV12 texture itself, and a pass-through
// mfxFrameAllocator::GetHDL resolves it, so the encoder reads the texture in
// place with no copy at all.
//
// Settings follow RustDesk's VRAM encoder (hwcodec's util_encode::set_av_codec_ctx
// and set_rate_control): CBR, no B-frames, no periodic key frames, one slice,
// low-delay, H.264 High / HEVC Main, BT.601 limited video signal info.

#ifndef ONEVPL_EXPERIMENTAL
#define ONEVPL_EXPERIMENTAL  // mfxMemoryInterface / mfxSurfaceD3D11Tex2D
#endif

#include "chast/encoder.h"

#include <d3d10.h>
#include <dxgi1_6.h>

#include <vpl/mfxdefs.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxmemory.h>
#include <vpl/mfxstructures.h>
#include <vpl/mfxvideo.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "chast/com_ptr.h"

namespace chast {

namespace {

constexpr UINT kAsyncDepth = 1;
constexpr size_t kBitstreamBufferSize = 4u * 1024u * 1024u;
// Upper bound for growing a bitstream that cannot hold one access unit.
constexpr size_t kMaxBitstreamBufferSize = 64u * 1024u * 1024u;
// MFXVideoCORE_SyncOperation polling step; the encode itself is a few ms.
constexpr mfxU32 kSyncWaitMs = 100;
constexpr int kMaxSyncPolls = 100;  // 10 s before giving up on a frame
// A tier's test frame must come back quickly; a broken encoder should cost the
// ladder a second, not the full stream timeout.
constexpr int kProbeSyncPolls = 10;  // 1 s

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

// Identity of the adapter the capture device lives on. VPL may only use an
// implementation on this adapter: the NV12 texture belongs to that device, so
// an encoder on another GPU would have to cross devices, which neither import
// nor surface wrapping can do.
struct AdapterIdentity {
    bool valid = false;
    LUID luid{};
    UINT vendor_id = 0;
    UINT device_id = 0;
};

AdapterIdentity identify_adapter(ID3D11Device* device) {
    AdapterIdentity identity;
    ComPtr<IDXGIDevice> dxgi_device = com_query<IDXGIDevice>(device);
    ComPtr<IDXGIAdapter> adapter;
    if (!dxgi_device || FAILED(dxgi_device->GetAdapter(adapter.put()))) {
        return identity;
    }
    ComPtr<IDXGIAdapter1> adapter1 = com_query<IDXGIAdapter1>(adapter);
    DXGI_ADAPTER_DESC1 desc{};
    if (!adapter1 || FAILED(adapter1->GetDesc1(&desc))) {
        return identity;
    }
    identity.valid = true;
    identity.luid = desc.AdapterLuid;
    identity.vendor_id = desc.VendorId;
    identity.device_id = desc.DeviceId;
    return identity;
}

std::string luid_string(const LUID& luid) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%08lX:%08lX",
                  static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    return text;
}

// One enumerated implementation that runs on the capture adapter.
struct ImplCandidate {
    mfxU32 index = 0;
    std::string name;
    mfxU16 api_major = 0;
    mfxU16 api_minor = 0;
};

// True when the implementation's device is the capture adapter. The extended
// device ID carries the DXGI LUID, verified against the actual device; runtimes
// that do not populate it fall back to PCI vendor/device IDs, and anything that
// cannot prove it is on this adapter is rejected rather than risk crossing.
bool implementation_on_adapter(mfxLoader loader, mfxU32 index, const mfxImplDescription& desc,
                               const AdapterIdentity& adapter) {
    mfxHDL raw = nullptr;
    if (MFXEnumImplementations(loader, index, MFX_IMPLCAPS_DEVICE_ID_EXTENDED, &raw) ==
            MFX_ERR_NONE &&
        raw != nullptr) {
        const auto* extended = reinterpret_cast<const mfxExtendedDeviceId*>(raw);
        bool match = false;
        if (extended->LUIDValid) {
            match = std::memcmp(extended->DeviceLUID, &adapter.luid, sizeof(adapter.luid)) == 0;
        } else {
            match = extended->VendorID == adapter.vendor_id &&
                    extended->DeviceID == adapter.device_id;
        }
        MFXDispReleaseImplDescription(loader, raw);
        return match;
    }

    if (desc.VendorID != adapter.vendor_id) {
        return false;
    }
    unsigned device_id = 0;
    const auto parsed = std::from_chars(desc.Dev.DeviceID,
                                        desc.Dev.DeviceID + sizeof(desc.Dev.DeviceID), device_id);
    return parsed.ec == std::errc() && device_id == adapter.device_id;
}

// Restricts the dispatcher to a hardware implementation with D3D11
// acceleration. A non-zero `codec_id` additionally requires that encoder;
// session creation passes 0 so one loader can serve every fallback tier.
// Surface-sharing capabilities are deliberately not a filter: several runtimes
// (including this machine's mfxhw64) do not populate mfxSurfaceTypesSupported,
// and filtering on it removes an implementation that can still encode. The API
// version decides how frames get in: 2.10+ imports through the memory
// interface, older runtimes wrap the texture directly.
Error filter_d3d11_encoder(mfxLoader loader, mfxU32 codec_id) {
    struct Filter {
        const char* name;
        mfxU32 value;
    };
    const Filter filters[] = {
        {"mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE},
        {"mfxImplDescription.mfxEncoderDescription.encoder.CodecID", codec_id},
        {"mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_D3D11},
    };
    for (const Filter& filter : filters) {
        if (filter.value == 0) {
            continue;
        }
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

// One encoder configuration to try. `low_power` selects VDENC on runtimes that
// have both fixed-function and VME encoders (Media SDK 1.x); oneVPL 2.10+
// implementations only exist in the low-power form, where VME attempts simply
// fail Init and the ladder stops there.
struct EncodeTier {
    VideoCodec codec;
    bool low_power;
    const char* label;
};

// Requested codec first. An HEVC request may fall back to H.264 when HEVC
// hardware encoding is unavailable; an H.264 request never upgrades, so a
// client that cannot decode HEVC is never sent it.
std::vector<EncodeTier> encode_tiers(VideoCodec requested) {
    std::vector<EncodeTier> tiers;
    if (requested == VideoCodec::Hevc) {
        tiers.push_back({VideoCodec::Hevc, true, "VDENC HEVC"});
    }
    tiers.push_back({VideoCodec::H264, true, "VDENC AVC"});
    tiers.push_back({VideoCodec::H264, false, "VME AVC"});
    return tiers;
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
    Error encode_repeat(uint64_t pts_ms, bool force_keyframe,
                        std::vector<EncodedPacket>& out) override;
    Error set_bitrate(UINT bits_per_second) override;
    UINT bitrate() const noexcept override { return bitrate_; }
    VideoCodec codec() const noexcept override { return codec_; }
    UINT width() const noexcept override { return width_; }
    UINT height() const noexcept override { return height_; }
    const std::string& implementation() const noexcept override { return implementation_; }
    bool shared_import() const noexcept override { return legacy_memory_ || import_shared_; }

private:
    void close();
    void discard_session();
    Error start_tier(const ImplCandidate& implementation, const EncodeTier& tier);
    Error probe_encode();
    Error import_surface(ID3D11Texture2D* texture, mfxFrameSurface1** surface);
    Error submit(mfxFrameSurface1* surface, bool force_keyframe,
                 std::vector<EncodedPacket>& out, uint64_t pts_ms,
                 mfxU32 wait_ms = kSyncWaitMs, int max_polls = kMaxSyncPolls);

    // mfxFrameAllocator used only on legacy runtimes, whose video-memory Init
    // requires one. Frames are D3D11 NV12 textures; the MemId is the texture
    // pointer itself.
    static mfxStatus MFX_CDECL alloc_frames(mfxHDL pthis, mfxFrameAllocRequest* request,
                                           mfxFrameAllocResponse* response);
    static mfxStatus MFX_CDECL free_frames(mfxHDL pthis, mfxFrameAllocResponse* response);
    static mfxStatus MFX_CDECL lock_frames(mfxHDL pthis, mfxMemId mid, mfxFrameData* data);
    static mfxStatus MFX_CDECL unlock_frames(mfxHDL pthis, mfxMemId mid, mfxFrameData* data);
    static mfxStatus MFX_CDECL get_handle(mfxHDL pthis, mfxMemId mid, mfxHDL* handle);

    AdapterIdentity adapter_;
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    bool encode_open_ = false;
    mfxMemoryInterface* memory_interface_ = nullptr;
    // The legacy runtime exposes no memory interface, so the texture is wrapped
    // in a surface directly; frame_allocator_ must outlive the session. The
    // surface is reused across frames and kept alive even past a sync timeout,
    // because the runtime may still reference it.
    bool legacy_memory_ = false;
    mfxFrameAllocator frame_allocator_{};
    mfxFrameSurface1 legacy_surface_{};
    ComPtr<ID3D11Device> device_;
    // The NV12 texture imported on the previous call, and the surface the
    // runtime handed back for it. Re-importing is skipped only when the runtime
    // mapped the texture (shared import); a copy import is re-taken per frame,
    // see import_surface.
    ID3D11Texture2D* imported_texture_ = nullptr;
    mfxFrameSurface1* imported_surface_ = nullptr;
    // True once a frame has been submitted successfully, so encode_repeat()
    // has a surface to hand back to the runtime.
    bool has_input_ = false;
    // True when the runtime mapped the texture (IMPORT_SHARED) instead of
    // GPU-copying it (IMPORT_COPY); see import_surface. On the legacy path the
    // texture is wrapped in place, which is also a no-copy handover.
    bool import_shared_ = false;
    // Set after the runtime refuses a shared import once, so the fallback is
    // used directly on later frames instead of retrying a known failure.
    bool shared_import_refused_ = false;
    mfxBitstream bitstream_{};
    std::vector<mfxU8> bitstream_data_;
    // The parameters the encoder was initialised with (as corrected by
    // MFXVideoENCODE_Query). MFXVideoENCODE_Reset takes a complete description,
    // not a patch, so set_bitrate() edits these and resets with them; the
    // signal buffer lives beside them because the extension list points into
    // it.
    mfxExtVideoSignalInfo signal_{};
    // Owns the extension pointer list carried by params_ so it stays valid for
    // the whole session (a local array would dangle after initialize()).
    mfxExtBuffer* extensions_[1] = {nullptr};
    mfxVideoParam params_{};

    VideoCodec codec_ = VideoCodec::H264;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT bitrate_ = 0;
    UINT requested_bitrate_ = 0;
    UINT framerate_ = 30;
    UINT gop_size_ = 0;
    float quality_ratio_ = 0.67f;
    std::string implementation_;
};

mfxStatus MFX_CDECL MfxEncoder::alloc_frames(mfxHDL pthis, mfxFrameAllocRequest* request,
                                             mfxFrameAllocResponse* response) {
    if (!pthis || !request || !response) {
        return MFX_ERR_NULL_PTR;
    }
    response->mids = nullptr;
    response->NumFrameActual = 0;
    if ((request->Type & MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET) == 0 ||
        request->Info.FourCC != MFX_FOURCC_NV12 || request->Info.Width == 0 ||
        request->Info.Height == 0) {
        return MFX_ERR_UNSUPPORTED;
    }
    auto* self = static_cast<MfxEncoder*>(pthis);
    const mfxU16 count =
        (std::max)(static_cast<mfxU16>(1), request->NumFrameSuggested);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = request->Info.Width;
    desc.Height = request->Info.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    auto** mids = new mfxMemId[count]{};
    for (mfxU32 i = 0; i < count; ++i) {
        ID3D11Texture2D* texture = nullptr;
        if (FAILED(self->device_->CreateTexture2D(&desc, nullptr, &texture)) || !texture) {
            for (mfxU32 j = 0; j < i; ++j) {
                static_cast<ID3D11Texture2D*>(mids[j])->Release();
            }
            delete[] mids;
            return MFX_ERR_MEMORY_ALLOC;
        }
        mids[i] = texture;
    }
    response->mids = mids;
    response->NumFrameActual = count;
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL MfxEncoder::free_frames(mfxHDL, mfxFrameAllocResponse* response) {
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

mfxStatus MFX_CDECL MfxEncoder::lock_frames(mfxHDL, mfxMemId, mfxFrameData*) {
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus MFX_CDECL MfxEncoder::unlock_frames(mfxHDL, mfxMemId, mfxFrameData*) {
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus MFX_CDECL MfxEncoder::get_handle(mfxHDL, mfxMemId mid, mfxHDL* handle) {
    if (!mid || !handle) {
        return MFX_ERR_INVALID_HANDLE;
    }
    // mfxhw64 expects the texture pointer itself in *handle; mfxHDLPair is only
    // needed for texture arrays, which this encoder never allocates.
    *handle = mid;
    return MFX_ERR_NONE;
}

Error MfxEncoder::initialize(ID3D11Device* device, const EncoderConfig& config) {
    if (!device) {
        return Error(Status::InvalidCall, "encoder needs a D3D11 device");
    }
    if (config.width == 0 || config.height == 0) {
        return Error(Status::InvalidCall, "encoder needs a frame size");
    }
    device_ = ComPtr<ID3D11Device>(device);
    device_.add_ref();

    width_ = config.width;
    height_ = config.height;
    framerate_ = config.framerate ? config.framerate : 30;
    gop_size_ = config.gop_size;
    requested_bitrate_ = config.bitrate;
    quality_ratio_ = config.quality_ratio;

    // The texture comes from this device, so the encoder must be on the same
    // adapter. Never let the dispatcher pick an implementation that lives on
    // another GPU: the frame cannot cross devices.
    adapter_ = identify_adapter(device_.get());
    if (!adapter_.valid) {
        return Error(Status::InvalidCall,
                     "cannot identify the adapter the capture device lives on");
    }

    loader_ = MFXLoad();
    if (!loader_) {
        return Error(Status::Unsupported, "MFXLoad failed: no oneVPL runtime is installed");
    }
    // No codec filter here: the fallback ladder may switch codec, so the loader
    // lists every hardware D3D11 implementation and each tier is tried against
    // those that sit on the capture adapter.
    Error filtered = filter_d3d11_encoder(loader_, 0);
    if (!filtered.ok()) {
        return filtered;
    }

    std::vector<ImplCandidate> candidates;
    std::vector<std::string> elsewhere;
    for (mfxU32 index = 0;; ++index) {
        mfxHDL raw = nullptr;
        if (MFXEnumImplementations(loader_, index, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &raw) !=
                MFX_ERR_NONE ||
            !raw) {
            break;
        }
        const auto* desc = reinterpret_cast<const mfxImplDescription*>(raw);
        if (implementation_on_adapter(loader_, index, *desc, adapter_)) {
            ImplCandidate candidate;
            candidate.index = index;
            candidate.name = desc->ImplName;
            candidate.api_major = desc->ApiVersion.Major;
            candidate.api_minor = desc->ApiVersion.Minor;
            candidates.push_back(std::move(candidate));
        } else if (desc->ImplName[0] != '\0') {
            elsewhere.emplace_back(desc->ImplName);
        }
        MFXDispReleaseImplDescription(loader_, raw);
    }
    if (candidates.empty()) {
        std::string message =
            "no oneVPL D3D11 implementation on the capture adapter (LUID " +
            luid_string(adapter_.luid) + ")";
        if (!elsewhere.empty()) {
            message += "; implementations on other adapters: ";
            for (size_t i = 0; i < elsewhere.size(); ++i) {
                if (i != 0) {
                    message += ", ";
                }
                message += elsewhere[i];
            }
        }
        return Error(Status::Unsupported, message);
    }

    // The runtime encodes on its own internal thread and drives the D3D11
    // immediate context from there. Without multithread protection on the
    // device the runtime refuses to take it (SetHandle answers
    // MFX_ERR_UNDEFINED_BEHAVIOR). hwcodec and Intel's examples both set it
    // before handing the device over.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(multithread.put())))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    std::string attempts;
    for (const EncodeTier& tier : encode_tiers(config.codec)) {
        for (const ImplCandidate& candidate : candidates) {
            Error err = start_tier(candidate, tier);
            if (err.ok()) {
                return Error::success();
            }
            if (!attempts.empty()) {
                attempts += "; ";
            }
            attempts += candidate.name + " " + tier.label + ": " + err.to_string();
            discard_session();
        }
    }
    return Error(Status::Unsupported,
                 "no VPL encoder tier could be initialised: " + attempts);
}

Error MfxEncoder::start_tier(const ImplCandidate& candidate, const EncodeTier& tier) {
    discard_session();

    codec_ = tier.codec;
    bitrate_ = requested_bitrate_ ? requested_bitrate_
                                  : hardware_bitrate(width_, height_, quality_ratio_,
                                                     codec_ == VideoCodec::H264);

    mfxStatus sts = MFXCreateSession(loader_, candidate.index, &session_);
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXCreateSession");
    }
    // ImportFrameSurface and the memory interface arrived in oneVPL 2.10. The
    // legacy mfxhw64 runtime (API 1.x) can still encode D3D11 textures, but
    // only through an application frame allocator, so it wraps them instead.
    legacy_memory_ = candidate.api_major < 2 ||
                     (candidate.api_major == 2 && candidate.api_minor < 10);

    sts = MFXVideoCORE_SetHandle(session_, MFX_HANDLE_D3D11_DEVICE,
                                 reinterpret_cast<mfxHDL>(device_.get()));
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoCORE_SetHandle(D3D11)");
    }

    if (legacy_memory_) {
        // mfxhw64 refuses MFX_IOPATTERN_IN_VIDEO_MEMORY without an allocator
        // (Init answers MFX_ERR_INVALID_VIDEO_PARAM). The allocator produces
        // D3D11 NV12 textures for the runtime's internal requests; external
        // frames are the capture textures themselves.
        frame_allocator_ = {};
        frame_allocator_.pthis = this;
        frame_allocator_.Alloc = &MfxEncoder::alloc_frames;
        frame_allocator_.Free = &MfxEncoder::free_frames;
        frame_allocator_.Lock = &MfxEncoder::lock_frames;
        frame_allocator_.Unlock = &MfxEncoder::unlock_frames;
        frame_allocator_.GetHDL = &MfxEncoder::get_handle;
        sts = MFXVideoCORE_SetFrameAllocator(session_, &frame_allocator_);
        if (sts != MFX_ERR_NONE) {
            return mfx_error(sts, "MFXVideoCORE_SetFrameAllocator");
        }
    }

    // Query and Init with the same description so the runtime can allocate its
    // internal surface pool around the imported frames.
    mfxVideoParam& params = params_;
    params = {};
    params.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    params.AsyncDepth = kAsyncDepth;
    mfxInfoMFX& mfx = params.mfx;
    mfx.CodecId = tier.codec == VideoCodec::H264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;
    // ON selects the fixed-function VDENC encoder where the runtime has both;
    // OFF asks for the VME encoder. oneVPL 2.10+ implementations are VDENC-only,
    // so their VME tier fails Init and the ladder ends there.
    mfx.LowPower =
        static_cast<mfxU16>(tier.low_power ? MFX_CODINGOPTION_ON : MFX_CODINGOPTION_OFF);
    mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
    mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    mfx.TargetKbps =
        static_cast<mfxU16>((std::clamp)(bitrate_ / 1000u, 1u, 0xFFFFu));
    // A stream with no periodic key frames; RustDesk's gop is MAX_GOP unless
    // the caller configured an interval, and this encoder asks per frame.
    mfx.GopPicSize =
        static_cast<mfxU16>((std::clamp)(gop_size_ == 0 ? 0xFFFFu : gop_size_, 1u, 0xFFFFu));
    mfx.GopRefDist = 1;  // no B-frames
    mfx.IdrInterval = 0;  // every I-frame an IDR (H.264)
    mfx.CodecProfile = static_cast<mfxU16>(
        tier.codec == VideoCodec::H264 ? MFX_PROFILE_AVC_HIGH : MFX_PROFILE_HEVC_MAIN);

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
    extensions_[0] = &signal.Header;
    params.ExtParam = extensions_;
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
    // Query may have corrected the rate; report the value actually in use.
    bitrate_ = static_cast<UINT>(params.mfx.TargetKbps) * 1000u;

    bitstream_data_.resize(kBitstreamBufferSize);
    bitstream_ = {};
    bitstream_.MaxLength = static_cast<mfxU32>(kBitstreamBufferSize);
    bitstream_.Data = bitstream_data_.data();

    if (!legacy_memory_) {
        sts = MFXGetMemoryInterface(session_, &memory_interface_);
        if (sts != MFX_ERR_NONE) {
            return mfx_error(sts, "MFXGetMemoryInterface");
        }
        if (!memory_interface_) {
            return Error(Status::Unsupported,
                         "oneVPL runtime " + candidate.name + " exposed no memory interface");
        }
    }

    // Init accepting the parameters is not proof the runtime can encode: a
    // driver can open a VDENC session and then fail every EncodeFrameAsync.
    // The tier is only accepted after a frame goes through the same path real
    // frames take.
    Error probe = probe_encode();
    if (!probe.ok()) {
        return probe;
    }

    implementation_ = candidate.name;
    if (!implementation_.empty()) {
        implementation_ += " ";
    }
    implementation_ += "(";
    implementation_ += tier.label;
    implementation_ += ")";
    return Error::success();
}

Error MfxEncoder::probe_encode() {
    // Same NV12 description the capture pass publishes, so the test exercises
    // the texture flags real frames carry.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = align16(width_);
    desc.Height = align16(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_VIDEO_ENCODER | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, texture.put());
    if (FAILED(hr) || !texture) {
        return Error(Status::CaptureFailed,
                     "CreateTexture2D(NV12 probe) failed: " +
                         std::to_string(static_cast<unsigned long>(hr)));
    }

    mfxFrameSurface1* surface = nullptr;
    if (legacy_memory_) {
        legacy_surface_ = {};
        legacy_surface_.Info = params_.mfx.FrameInfo;
        legacy_surface_.Data.MemId = texture.get();
        surface = &legacy_surface_;
    } else {
        Error err = import_surface(texture.get(), &surface);
        if (!err.ok()) {
            return err;
        }
    }

    std::vector<EncodedPacket> packets;
    Error err = submit(surface, /*force_keyframe=*/true, packets, /*pts_ms=*/0, kSyncWaitMs,
                       kProbeSyncPolls);
    // A frame that comes back must be the IDR the probe asked for; a runtime
    // that silently ignores the request is not usable for a live stream.
    if (err.ok() && !packets.empty() && !packets.front().keyframe) {
        err = Error(Status::Unsupported, "probe encode returned no key frame");
    }

    // The probe image is garbage, so it must not become the repeat input. The
    // imported surface holds its own reference; drop it before the texture dies.
    if (imported_surface_) {
        if (imported_surface_->FrameInterface) {
            imported_surface_->FrameInterface->Release(imported_surface_);
        }
        imported_surface_ = nullptr;
    }
    imported_texture_ = nullptr;
    import_shared_ = false;
    legacy_surface_ = {};
    bitstream_.DataOffset = 0;
    bitstream_.DataLength = 0;
    return err;
}

void MfxEncoder::discard_session() {
    if (imported_surface_) {
        if (imported_surface_->FrameInterface) {
            imported_surface_->FrameInterface->Release(imported_surface_);
        }
        imported_surface_ = nullptr;
    }
    imported_texture_ = nullptr;
    import_shared_ = false;
    shared_import_refused_ = false;
    if (session_) {
        if (encode_open_) {
            MFXVideoENCODE_Close(session_);
            encode_open_ = false;
        }
        MFXClose(session_);
        session_ = nullptr;
    }
    memory_interface_ = nullptr;
    legacy_memory_ = false;
    frame_allocator_ = {};
    legacy_surface_ = {};
    has_input_ = false;
    bitstream_ = {};
    bitstream_data_.clear();
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

    auto import_with = [&](mfxU32 flags, mfxU32* negotiated, mfxFrameSurface1** imported) {
        mfxSurfaceD3D11Tex2D external{};
        external.SurfaceInterface.Header.SurfaceType = MFX_SURFACE_TYPE_D3D11_TEX2D;
        external.SurfaceInterface.Header.SurfaceFlags = flags;
        external.SurfaceInterface.Header.StructSize = sizeof(external);
        external.texture2D = reinterpret_cast<mfxHDL>(texture);
        *imported = nullptr;
        const mfxStatus result = memory_interface_->ImportFrameSurface(
            memory_interface_, MFX_SURFACE_COMPONENT_ENCODE, &external.SurfaceInterface.Header,
            imported);
        *negotiated = external.SurfaceInterface.Header.SurfaceFlags;
        return result;
    };

    // Mapping the texture is the cheapest path. Some runtimes advertise the
    // flag but refuse it at import time (this machine's driver answers
    // MFX_ERR_MEMORY_ALLOC for an NV12 render target; Intel's own
    // hello-sharing-dx11-import fails the same way). Once that happens, copy
    // import is used directly for the rest of the session: the texture still
    // never leaves the GPU, it is device-copied into the runtime's pool.
    mfxU32 negotiated = MFX_SURFACE_FLAG_IMPORT_COPY;
    mfxStatus sts = MFX_ERR_UNSUPPORTED;
    if (!shared_import_refused_) {
        sts = import_with(MFX_SURFACE_FLAG_IMPORT_SHARED, &negotiated, surface);
        if (sts != MFX_ERR_NONE) {
            shared_import_refused_ = true;
            // A refused import should not hand back a surface, but releasing a
            // partial result here is cheaper than leaking it if one does.
            if (*surface && (*surface)->FrameInterface) {
                (*surface)->FrameInterface->Release(*surface);
            }
            *surface = nullptr;
        }
    }
    if (sts != MFX_ERR_NONE) {
        sts = import_with(MFX_SURFACE_FLAG_IMPORT_COPY, &negotiated, surface);
    }
    if (sts != MFX_ERR_NONE) {
        return mfx_error(sts, "ImportFrameSurface(NV12)");
    }
    import_shared_ = (negotiated & MFX_SURFACE_FLAG_IMPORT_SHARED) != 0;

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
    if (legacy_memory_) {
        D3D11_TEXTURE2D_DESC desc{};
        nv12->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12) {
            return Error(Status::InvalidCall, "encoder input is not NV12");
        }
        if (desc.Width < align16(width_) || desc.Height < align16(height_)) {
            return Error(Status::InvalidCall, "encoder input texture is smaller than the frame");
        }
        // The legacy runtime resolves MemId through get_handle() and reads the
        // capture texture in place. The persistent surface is refilled here;
        // submit() waits for the sync point before returning.
        legacy_surface_ = {};
        legacy_surface_.Info = params_.mfx.FrameInfo;
        legacy_surface_.Data.MemId = nv12;
        legacy_surface_.Data.Pitch = 0;
        Error err = submit(&legacy_surface_, force_keyframe, out, pts_ms);
        if (err.ok()) {
            has_input_ = true;
        }
        return err;
    }
    mfxFrameSurface1* surface = nullptr;
    Error err = import_surface(nv12, &surface);
    if (!err.ok()) {
        return err;
    }
    err = submit(surface, force_keyframe, out, pts_ms);
    if (err.ok()) {
        has_input_ = true;
    }
    return err;
}

Error MfxEncoder::encode_repeat(uint64_t pts_ms, bool force_keyframe,
                                std::vector<EncodedPacket>& out) {
    if (!session_ || !encode_open_) {
        return Error(Status::NotCurrentlyAvailable, "encoder is not initialized");
    }
    if (!has_input_) {
        return Error(Status::NotCurrentlyAvailable, "no encoded input to repeat");
    }
    // A repeat frame never touches the texture, so no import failure reports a
    // removed device first; without this check the runtime would sit in the
    // sync wait for the full timeout before the loss surfaces.
    const HRESULT device_status = device_->GetDeviceRemovedReason();
    if (FAILED(device_status)) {
        return check(device_status, "D3D11 device check before repeat");
    }
    mfxFrameSurface1* surface = legacy_memory_ ? &legacy_surface_ : imported_surface_;
    if (!surface) {
        return Error(Status::NotCurrentlyAvailable, "no encoded input to repeat");
    }
    return submit(surface, force_keyframe, out, pts_ms);
}

Error MfxEncoder::submit(mfxFrameSurface1* surface, bool force_keyframe,
                         std::vector<EncodedPacket>& out, uint64_t pts_ms,
                         mfxU32 wait_ms, int max_polls) {
    mfxEncodeCtrl control{};
    mfxEncodeCtrl* control_ptr = nullptr;
    if (force_keyframe) {
        control.FrameType = static_cast<mfxU16>(MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR);
        control_ptr = &control;
    }

    surface->Data.TimeStamp = pts_ms;
    mfxSyncPoint sync = nullptr;
    // The surface stays referenced by the runtime until the operation
    // completes; nothing to release here (the import keeps it alive).
    auto encode_once = [&]() {
        bitstream_.DataOffset = 0;
        bitstream_.DataLength = 0;
        sync = nullptr;
        return MFXVideoENCODE_EncodeFrameAsync(session_, control_ptr, surface, &bitstream_,
                                               &sync);
    };
    mfxStatus sts = encode_once();
    if (sts == MFX_WRN_DEVICE_BUSY) {
        // Transient back-pressure from the GPU; wait for outstanding work, as
        // Intel's examples do, then retry once.
        MFXVideoCORE_SyncOperation(session_, nullptr, wait_ms);
        sts = encode_once();
    }
    // One access unit can exceed the initial buffer (large IDR at a high
    // target rate). Grow it as Intel's samples do instead of failing the
    // stream permanently on the same undersized buffer.
    for (unsigned growth = 0; sts == MFX_ERR_NOT_ENOUGH_BUFFER; ++growth) {
        if (growth >= 4 || bitstream_data_.size() >= kMaxBitstreamBufferSize) {
            break;
        }
        const size_t capacity =
            (std::min)(bitstream_data_.size() * 2, kMaxBitstreamBufferSize);
        bitstream_data_.resize(capacity);
        bitstream_.MaxLength = static_cast<mfxU32>(capacity);
        bitstream_.Data = bitstream_data_.data();
        sts = encode_once();
    }
    // The encoder accepted the frame but produced nothing yet. This is normal
    // for the low-delay path and is not an error; the sync point (when one is
    // returned) is still waited on below.
    if (sts == MFX_ERR_MORE_DATA && sync == nullptr) {
        return Error::success();
    }
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoENCODE_EncodeFrameAsync");
    }
    if (sync) {
        int polls = 0;
        do {
            sts = MFXVideoCORE_SyncOperation(session_, sync, wait_ms);
            ++polls;
        } while (sts == MFX_WRN_IN_EXECUTION && polls < max_polls);
        if (sts == MFX_WRN_IN_EXECUTION) {
            return Error(Status::CaptureFailed,
                         "encode did not complete within " +
                             std::to_string(static_cast<long long>(wait_ms) * max_polls) + " ms");
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
    const UINT kbps = (std::clamp)(bits_per_second / 1000u, 1u, 0xFFFFu);
    // The reset needs a full description, not just the fields that change: the
    // runtime compares it against what is running. Everything else is carried
    // over from initialisation.
    mfxVideoParam reset = params_;
    reset.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
    reset.mfx.TargetKbps = static_cast<mfxU16>(kbps);
    reset.ExtParam = extensions_;
    reset.NumExtParam = 1;
    const mfxStatus sts = MFXVideoENCODE_Reset(session_, &reset);
    // A positive status is a warning the runtime already resolved (mfx-gen
    // answers MFX_WRN_INCOMPATIBLE_VIDEO_PARAM while applying the new rate) and
    // the reset has taken effect.
    if (sts < MFX_ERR_NONE) {
        return mfx_error(sts, "MFXVideoENCODE_Reset(bitrate)");
    }
    params_.mfx.TargetKbps = reset.mfx.TargetKbps;
    bitrate_ = static_cast<UINT>(reset.mfx.TargetKbps) * 1000u;
    return Error::success();
}

void MfxEncoder::close() {
    discard_session();
    if (loader_) {
        MFXUnload(loader_);
        loader_ = nullptr;
    }
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

bool hardware_encoder_available(VideoCodec codec, ID3D11Device* device) {
    AdapterIdentity adapter;
    if (device) {
        adapter = identify_adapter(device);
        if (!adapter.valid) {
            return false;
        }
    }

    auto has_implementation = [&](VideoCodec wanted) {
        mfxLoader loader = MFXLoad();
        if (!loader) {
            return false;
        }
        const mfxU32 codec_id = wanted == VideoCodec::H264 ? MFX_CODEC_AVC : MFX_CODEC_HEVC;
        bool found = false;
        // Any enumerated implementation already satisfies the filters. The
        // description's encoder table is not consulted: mfxhw64 leaves
        // Enc.NumCodecs at zero even though session creation and encoding work,
        // and its API version (1.x) predates ImportFrameSurface, which is fine
        // -- create_encoder() uses an application frame allocator there.
        if (filter_d3d11_encoder(loader, codec_id).ok()) {
            for (mfxU32 index = 0; !found; ++index) {
                mfxHDL raw = nullptr;
                if (MFXEnumImplementations(loader, index, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, &raw) !=
                        MFX_ERR_NONE ||
                    !raw) {
                    break;
                }
                const auto* desc = reinterpret_cast<const mfxImplDescription*>(raw);
                found = !device || implementation_on_adapter(loader, index, *desc, adapter);
                MFXDispReleaseImplDescription(loader, raw);
            }
        }
        MFXUnload(loader);
        return found;
    };

    if (has_implementation(codec)) {
        return true;
    }
    // An HEVC request falls back to the AVC tiers when HEVC hardware encoding
    // is absent, so an AVC encoder on the capture adapter still qualifies.
    return codec == VideoCodec::Hevc && has_implementation(VideoCodec::H264);
}

std::unique_ptr<IVideoEncoder> create_encoder(ID3D11Device* device, const EncoderConfig& config,
                                              std::string* error) {
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
