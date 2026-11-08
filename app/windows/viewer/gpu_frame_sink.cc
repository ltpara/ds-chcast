#include "gpu_frame_sink.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace chast_viewer {

namespace {

constexpr UINT kBlitTimeoutMs = 2000;
// Poll tightly for a short while before falling back to Sleep(1). The sleep
// quantum is 15.6 ms unless something in the process asked for a 1 ms timer,
// and a per-frame wait must not inherit that.
constexpr UINT kWaitSpinMs = 2;
constexpr UINT kNominalRate = 60;

constexpr wchar_t kPluginDll[] = L"flutter_gpu_texture_renderer_plugin.dll";

// A remote desktop picture never legitimately needs more than 8K. The cap
// keeps a hostile or broken host from sizing three shared slots and the
// readback into multiple GiB, and keeps every RECT below LONG_MAX.
constexpr uint32_t kMaxFrameSide = 8192;
constexpr uint64_t kMaxFramePixels = 7680ull * 4320ull;

bool device_lost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

std::string fail_hr(const char* what, HRESULT hr) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lx)", what,
                  static_cast<unsigned long>(hr));
    return buffer;
}

}  // namespace

// The plugin is a normal dependency of the runner, so its DLL is already in
// the process. GetModuleHandle keeps this class from owning a reference the
// plugin registrar also manages.
GpuFrameSink::GpuFrameSink() {
    ready_ = resolve_plugin() && init_device();
}

GpuFrameSink::~GpuFrameSink() = default;

bool GpuFrameSink::resolve_plugin() {
    HMODULE module = GetModuleHandleW(kPluginDll);
    if (module == nullptr) {
        module = LoadLibraryW(kPluginDll);
    }
    if (module == nullptr) {
        set_error("flutter_gpu_texture_renderer_plugin.dll is not loaded");
        return false;
    }
    const auto set_texture = reinterpret_cast<SetTextureFn>(
        GetProcAddress(module, "FlutterGpuTextureRendererPluginCApiSetTexture"));
    if (set_texture == nullptr) {
        set_error("the GPU texture plugin has no SetTexture entry point");
        return false;
    }
    plugin_ = module;
    set_texture_ = set_texture;

    // Flutter renders through the adapter its view was created on. The shared
    // handle only opens on that adapter, so create our textures there too.
    const auto get_luid = reinterpret_cast<GetAdapterLuidFn>(
        GetProcAddress(module, "FlutterGpuTextureRendererPluginCApiGetAdapterLuid"));
    if (get_luid != nullptr) {
        const int64_t luid = get_luid();
        adapter_luid_.HighPart = static_cast<LONG>(luid >> 32);
        adapter_luid_.LowPart = static_cast<DWORD>(luid & 0xffffffff);
        has_adapter_luid_ = luid != 0;
    }
    return true;
}

bool GpuFrameSink::init_device() {
    chast::ComPtr<IDXGIAdapter1> adapter;
    if (has_adapter_luid_) {
        chast::ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                         reinterpret_cast<void**>(factory.put())))) {
            for (UINT index = 0;; ++index) {
                IDXGIAdapter1* candidate = nullptr;
                const HRESULT enumerated = factory->EnumAdapters1(index, &candidate);
                if (enumerated == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (FAILED(enumerated) || candidate == nullptr) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
                    desc.AdapterLuid.LowPart == adapter_luid_.LowPart &&
                    desc.AdapterLuid.HighPart == adapter_luid_.HighPart) {
                    adapter.attach(candidate);
                    break;
                }
                candidate->Release();
            }
        }
    }

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    const UINT flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_DRIVER_TYPE driver =
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(adapter.get(), driver, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   device_.put(), nullptr, context_.put());
    if (hr == E_INVALIDARG) {
        // Runtimes older than Windows 8 reject the whole call when 11_1 is in
        // the list, so retry without it.
        const D3D_FEATURE_LEVEL fallback[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        hr = D3D11CreateDevice(adapter.get(), driver, nullptr, flags, fallback,
                               ARRAYSIZE(fallback), D3D11_SDK_VERSION, device_.put(),
                               nullptr, context_.put());
    }
    if (FAILED(hr) || !device_ || !context_) {
        set_error(adapter ? "D3D11 device creation on Flutter's adapter failed"
                          : "D3D11 device creation failed",
                  device_lost(hr));
        return false;
    }

    video_device_ = chast::com_query<ID3D11VideoDevice>(device_);
    video_context_ = chast::com_query<ID3D11VideoContext>(context_);
    video_context1_ = chast::com_query<ID3D11VideoContext1>(context_);
    if (!video_device_ || !video_context_ || !video_context1_) {
        set_error("the display GPU has no D3D11 video processor interfaces");
        return false;
    }

    // The decoder drives this device from its own worker thread while the sink
    // records and blits on the session thread.
    chast::ComPtr<ID3D10Multithread> multithread =
        chast::com_query<ID3D10Multithread>(context_);
    if (multithread) {
        multithread->SetMultithreadProtected(TRUE);
    }

    D3D11_QUERY_DESC query_desc{};
    query_desc.Query = D3D11_QUERY_EVENT;
    hr = device_->CreateQuery(&query_desc, frame_done_.put());
    if (FAILED(hr)) {
        set_error("D3D11 event query creation failed", device_lost(hr));
        return false;
    }
    for (chast::ComPtr<ID3D11Query>& query : slot_done_) {
        hr = device_->CreateQuery(&query_desc, query.put());
        if (FAILED(hr)) {
            set_error("D3D11 slot event query creation failed", device_lost(hr));
            return false;
        }
    }
    return true;
}

void GpuFrameSink::SetOutput(void* output) {
    std::lock_guard frame_lock(frame_mutex_);
    std::lock_guard lock(mutex_);
    if (output) outputs_[nullptr] = output;
    else outputs_.erase(nullptr);
}

void GpuFrameSink::SetOutputForOwner(const void* owner, void* output) {
    std::lock_guard frame_lock(frame_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!output) {
        outputs_.erase(owner);
        return;
    }
    outputs_[owner] = output;
    if (last_slot_valid_ && slots_[last_slot_].texture && set_texture_) {
        set_texture_(output, slots_[last_slot_].texture.get());
        context_->End(slot_done_[last_slot_].get());
        context_->Flush();
        slot_done_pending_[last_slot_] = true;
    }
}

void GpuFrameSink::ClearOutputs() {
    std::lock_guard frame_lock(frame_mutex_);
    std::lock_guard lock(mutex_);
    outputs_.clear();
}

bool GpuFrameSink::HasOutputs() const {
    std::lock_guard lock(mutex_);
    return !outputs_.empty();
}

ID3D11Device* GpuFrameSink::device() {
    return device_.get();
}

std::string GpuFrameSink::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool GpuFrameSink::fatal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_fatal_;
}

void GpuFrameSink::clear_transient_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error_fatal_) {
        error_.clear();
    }
}

void GpuFrameSink::drop_pipeline() {
    slots_ = {};
    slot_done_pending_ = {};
    next_slot_ = 0;
    last_slot_ = 0;
    last_slot_valid_ = false;
    readback_.reset();
    enumerator_.reset();
    processor_.reset();
    pipeline_input_width_ = 0;
    pipeline_input_height_ = 0;
    pipeline_width_ = 0;
    pipeline_height_ = 0;
    pipeline_ready_ = false;
}

bool GpuFrameSink::Reset() {
    std::lock_guard frame_lock(frame_mutex_);
    drop_pipeline();
    frame_done_.reset();
    video_context1_.reset();
    video_context_.reset();
    video_device_.reset();
    context_.reset();
    device_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        error_fatal_ = false;
    }
    ready_ = init_device();
    if (!ready_) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_.empty()) {
            error_ = "the display GPU could not be re-initialized";
        }
        error_fatal_ = true;
        return false;
    }
    return true;
}

bool GpuFrameSink::ensure_pipeline(const D3D11_TEXTURE2D_DESC& input, uint32_t width,
                                   uint32_t height) {
    if (pipeline_ready_ && input.Format == pipeline_input_format_ && input.Width == pipeline_input_width_ &&
        input.Height == pipeline_input_height_ && width == pipeline_width_ &&
        height == pipeline_height_) {
        return true;
    }

    // The rebuild replaces the textures the slot queries trail. Let those
    // queries finish first so re-Ending them is legal and the new textures
    // cannot be written while the old ones are still in use.
    for (size_t i = 0; i < slot_done_pending_.size(); ++i) {
        if (!slot_done_pending_[i]) {
            continue;
        }
        if (!wait_for_query(slot_done_[i].get())) {
            return false;
        }
        slot_done_pending_[i] = false;
    }

    // Build everything into locals first: a failed rebuild must leave the
    // pipeline that is already on screen untouched.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator = kNominalRate;
    content.InputFrameRate.Denominator = 1;
    content.InputWidth = input.Width;
    content.InputHeight = input.Height;
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    chast::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, enumerator.put());
    if (FAILED(hr) || !enumerator) {
        set_error(fail_hr("CreateVideoProcessorEnumerator", hr), device_lost(hr));
        return false;
    }

    UINT output_flags = 0;
    // Intel's driver reports no capability bits for NV12 input even though
    // input views and blits work, so only the output format is gated here;
    // CreateVideoProcessorInputView reports a genuinely unusable input.
    if (FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     &output_flags)) ||
        (output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        set_error("the display GPU cannot render BGRA8");
        return false;
    }

    chast::ComPtr<ID3D11VideoProcessor> processor;
    hr = video_device_->CreateVideoProcessor(enumerator.get(), 0, processor.put());
    if (FAILED(hr) || !processor) {
        set_error(fail_hr("CreateVideoProcessor", hr), device_lost(hr));
        return false;
    }

    // The sender converts BGRA to NV12 with the BT.601 limited-range matrix
    // (see capture/nv12_pass.cc), so decode into RGB through the matching
    // space. Auto processing would stretch the range and shift the colors.
    video_context1_->VideoProcessorSetStreamColorSpace1(
        processor.get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
    video_context1_->VideoProcessorSetOutputColorSpace1(
        processor.get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    video_context_->VideoProcessorSetStreamFrameFormat(
        processor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context_->VideoProcessorSetStreamAutoProcessingMode(processor.get(), 0, FALSE);
    D3D11_VIDEO_COLOR background{};
    background.RGBA.R = 0.0f;
    background.RGBA.G = 0.0f;
    background.RGBA.B = 0.0f;
    background.RGBA.A = 1.0f;
    video_context_->VideoProcessorSetOutputBackgroundColor(processor.get(), TRUE,
                                                           &background);

    std::array<Slot, kSlotCount> fresh;
    for (size_t i = 0; i < fresh.size(); ++i) {
        Slot& slot = fresh[i];

        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = width;
        texture_desc.Height = height;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        // The video processor writes the texture; the plugin creates its
        // Flutter texture from this description and shares it with Flutter, so
        // the same flags have to be shareable.
        texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = device_->CreateTexture2D(&texture_desc, nullptr, slot.texture.put());
        if (FAILED(hr) || !slot.texture) {
            set_error(fail_hr("CreateTexture2D(display)", hr), device_lost(hr));
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
        hr = video_device_->CreateVideoProcessorOutputView(
            slot.texture.get(), enumerator.get(), &view_desc, slot.output_view.put());
        if (FAILED(hr) || !slot.output_view) {
            set_error(fail_hr("CreateVideoProcessorOutputView", hr), device_lost(hr));
            return false;
        }
    }

    slots_ = std::move(fresh);
    next_slot_ = 0;
    last_slot_valid_ = false;
    readback_.reset();
    enumerator_ = std::move(enumerator);
    processor_ = std::move(processor);
    pipeline_input_width_ = input.Width;
    pipeline_input_format_ = input.Format;
    pipeline_input_height_ = input.Height;
    pipeline_width_ = width;
    pipeline_height_ = height;
    pipeline_ready_ = true;
    return true;
}

bool GpuFrameSink::render(Slot& slot, ID3D11Texture2D* nv12, uint32_t subresource,
                          uint32_t width, uint32_t height, uint32_t crop_x,
                          uint32_t crop_y) {
    D3D11_TEXTURE2D_DESC input{};
    nv12->GetDesc(&input);
    if (input.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        // Software I444 decoding uploads full-resolution BGRA. A GPU copy
        // preserves those chroma samples; converting via NV12 would lose them.
        D3D11_BOX box{crop_x, crop_y, 0, crop_x + width, crop_y + height, 1};
        context_->CopySubresourceRegion(slot.texture.get(), 0, 0, 0, 0, nv12, subresource, &box);
        return true;
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view_desc{};
    view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipSlice = 0;
    // FFmpeg's D3D11VA pool is a texture array; the slice is the picture.
    view_desc.Texture2D.ArraySlice = subresource;
    chast::ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(
        nv12, enumerator_.get(), &view_desc, input_view.put());
    if (FAILED(hr) || !input_view) {
        set_error(fail_hr("CreateVideoProcessorInputView", hr), device_lost(hr));
        return false;
    }

    RECT source{};
    source.left = static_cast<LONG>(crop_x);
    source.top = static_cast<LONG>(crop_y);
    source.right = static_cast<LONG>(crop_x + width);
    source.bottom = static_cast<LONG>(crop_y + height);
    video_context_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &source);
    RECT dest{};
    dest.right = static_cast<LONG>(width);
    dest.bottom = static_cast<LONG>(height);
    video_context_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &dest);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.pInputSurface = input_view.get();
    hr = video_context_->VideoProcessorBlt(processor_.get(), slot.output_view.get(), 0, 1,
                                           &stream);
    if (FAILED(hr)) {
        set_error(fail_hr("VideoProcessorBlt", hr), device_lost(hr));
        return false;
    }
    // The plugin copies this slot on the same immediate context, so its copy
    // is ordered after the blit without an event wait here. OnGpuFrame bounds
    // how far the CPU may run ahead by waiting only when a slot is reused.
    return true;
}

bool GpuFrameSink::wait_for_gpu() {
    // The screenshot readback maps the slot right after this returns, so the
    // copy has to be complete. Frame rendering no longer waits here; the
    // per-slot queries bound the pipeline instead.
    context_->End(frame_done_.get());
    context_->Flush();
    return wait_for_query(frame_done_.get());
}

bool GpuFrameSink::wait_for_query(ID3D11Query* query) {
    const auto start = std::chrono::steady_clock::now();
    const auto spin_until = start + std::chrono::milliseconds(kWaitSpinMs);
    const auto deadline = start + std::chrono::milliseconds(kBlitTimeoutMs);
    for (;;) {
        const HRESULT hr = context_->GetData(query, nullptr, 0, 0);
        if (hr == S_OK) {
            return true;
        }
        if (hr != S_FALSE) {
            set_error(fail_hr("GetData(frame done)", hr), device_lost(hr));
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            set_error("the display GPU did not finish a frame in time");
            return false;
        }
        if (now < spin_until) {
            SwitchToThread();
        } else {
            Sleep(1);
        }
    }
}

bool GpuFrameSink::OnGpuFrame(ID3D11Texture2D* nv12, uint32_t subresource, uint32_t width,
                              uint32_t height, uint32_t crop_x, uint32_t crop_y,
                              uint64_t pts_ms) {
    std::lock_guard frame_lock(frame_mutex_);
    (void)pts_ms;
    if (!ready_ || nv12 == nullptr || width == 0 || height == 0) {
        return false;
    }
    // Each frame starts a new error generation so a transient failure does
    // not stick after the driver recovers. Device-lost errors stay.
    clear_transient_error();
    if (width > kMaxFrameSide || height > kMaxFrameSide ||
        static_cast<uint64_t>(width) * height > kMaxFramePixels) {
        set_error("the remote picture is larger than this viewer accepts");
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    nv12->GetDesc(&desc);
    if (desc.Width > kMaxFrameSide || desc.Height > kMaxFrameSide ||
        static_cast<uint64_t>(desc.Width) * desc.Height > kMaxFramePixels) {
        set_error("the decoder surface is larger than this viewer accepts");
        return false;
    }
    if (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        set_error("the decoder surface is neither NV12 nor BGRA");
        return false;
    }
    if (crop_x > desc.Width || crop_y > desc.Height || width > desc.Width - crop_x ||
        height > desc.Height - crop_y) {
        set_error("the decoded crop exceeds its texture");
        return false;
    }
    if (subresource >= desc.ArraySize) {
        set_error("the decoder surface slice is out of range");
        return false;
    }
    if (!ensure_pipeline(desc, width, height)) {
        return false;
    }

    if (!HasOutputs() || set_texture_ == nullptr) {
        return false;
    }

    const size_t slot_index = next_slot_;
    // The blit and the plugin's copy both run on this immediate context, so
    // reusing a slot can never race the previous frame. Waiting for the
    // slot's older query is backpressure: it caps the pipeline at three
    // frames instead of stalling on every frame as the old per-frame wait
    // did. At the capture rate the query has normally completed already.
    if (slot_done_pending_[slot_index]) {
        if (!wait_for_query(slot_done_[slot_index].get())) {
            return false;
        }
        slot_done_pending_[slot_index] = false;
    }

    Slot& slot = slots_[slot_index];
    if (!render(slot, nv12, subresource, width, height, crop_x, crop_y)) {
        return false;
    }
    // Decode and conversion happen once. Each engine receives a GPU copy;
    // detaching an owner waits until all calls using its output have finished.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outputs_.empty()) {
            return false;
        }
        for (const auto& entry : outputs_) set_texture_(entry.second, slot.texture.get());
    }
    // The plugin flushes after its copy. Flush again so this event query is
    // submitted even on a plugin path that returns before copying.
    context_->End(slot_done_[slot_index].get());
    context_->Flush();
    slot_done_pending_[slot_index] = true;
    last_slot_ = slot_index;
    last_slot_valid_ = true;
    next_slot_ = (slot_index + 1) % kSlotCount;
    return true;
}

bool GpuFrameSink::CaptureBgra(std::vector<uint8_t>& bgra, uint32_t& width,
                               uint32_t& height, std::string* error) {
    std::lock_guard frame_lock(frame_mutex_);
    auto fail = [&](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    if (!ready_ || !device_ || !context_) {
        return fail("the display sink is not ready");
    }
    clear_transient_error();
    if (!last_slot_valid_ || !slots_[last_slot_].texture) {
        return fail("no remote picture has been rendered yet");
    }

    ID3D11Texture2D* texture = slots_[last_slot_].texture.get();
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return fail("the display surface is not BGRA");
    }

    if (readback_) {
        D3D11_TEXTURE2D_DESC existing{};
        readback_->GetDesc(&existing);
        if (existing.Width != desc.Width || existing.Height != desc.Height) {
            readback_.reset();
        }
    }
    if (!readback_) {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        const HRESULT hr = device_->CreateTexture2D(&staging, nullptr, readback_.put());
        if (FAILED(hr) || !readback_) {
            return fail(fail_hr("CreateTexture2D(readback)", hr));
        }
    }

    context_->CopyResource(readback_.get(), texture);
    if (!wait_for_gpu()) {
        return fail(this->error());
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context_->Map(readback_.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || mapped.pData == nullptr) {
        return fail(fail_hr("Map(readback)", hr));
    }
    width = desc.Width;
    height = desc.Height;
    const size_t row = static_cast<size_t>(width) * 4;
    bgra.resize(row * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(bgra.data() + static_cast<size_t>(y) * row,
                    static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch,
                    row);
    }
    context_->Unmap(readback_.get(), 0);
    return true;
}

void GpuFrameSink::set_error(std::string message, bool device_gone) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_gone) {
        // A device-lost reason is the one that matters even if a transient
        // failure was recorded first.
        error_ = std::move(message);
        error_fatal_ = true;
    } else if (error_.empty()) {
        error_ = std::move(message);
    }
}

}  // namespace chast_viewer
