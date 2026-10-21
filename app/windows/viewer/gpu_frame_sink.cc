#include "gpu_frame_sink.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace chast_viewer {

namespace {

constexpr UINT kBlitTimeoutMs = 2000;
constexpr UINT kNominalRate = 60;

constexpr wchar_t kPluginDll[] = L"flutter_gpu_texture_renderer_plugin.dll";

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
                if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
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
                          : "D3D11 device creation failed");
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
        set_error("D3D11 event query creation failed");
        return false;
    }
    return true;
}

void GpuFrameSink::SetOutput(void* output) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ = output;
}

ID3D11Device* GpuFrameSink::device() {
    return device_.get();
}

std::string GpuFrameSink::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool GpuFrameSink::ensure_pipeline(const D3D11_TEXTURE2D_DESC& input, uint32_t width,
                                   uint32_t height) {
    if (pipeline_ready_ && input.Width == pipeline_input_width_ &&
        input.Height == pipeline_input_height_ && width == pipeline_width_ &&
        height == pipeline_height_) {
        return true;
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
        set_error(fail_hr("CreateVideoProcessorEnumerator", hr));
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
        set_error(fail_hr("CreateVideoProcessor", hr));
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
            set_error(fail_hr("CreateTexture2D(display)", hr));
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
        hr = video_device_->CreateVideoProcessorOutputView(
            slot.texture.get(), enumerator.get(), &view_desc, slot.output_view.put());
        if (FAILED(hr) || !slot.output_view) {
            set_error(fail_hr("CreateVideoProcessorOutputView", hr));
            return false;
        }
    }

    slots_ = std::move(fresh);
    next_slot_ = 0;
    enumerator_ = std::move(enumerator);
    processor_ = std::move(processor);
    pipeline_input_width_ = input.Width;
    pipeline_input_height_ = input.Height;
    pipeline_width_ = width;
    pipeline_height_ = height;
    pipeline_ready_ = true;
    return true;
}

bool GpuFrameSink::render(Slot& slot, ID3D11Texture2D* nv12, uint32_t subresource,
                          uint32_t width, uint32_t height, uint32_t crop_x,
                          uint32_t crop_y) {
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view_desc{};
    view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipSlice = 0;
    // FFmpeg's D3D11VA pool is a texture array; the slice is the picture.
    view_desc.Texture2D.ArraySlice = subresource;
    chast::ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = video_device_->CreateVideoProcessorInputView(
        nv12, enumerator_.get(), &view_desc, input_view.put());
    if (FAILED(hr) || !input_view) {
        set_error(fail_hr("CreateVideoProcessorInputView", hr));
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
        set_error(fail_hr("VideoProcessorBlt", hr));
        return false;
    }
    return wait_for_gpu();
}

bool GpuFrameSink::wait_for_gpu() {
    // The plugin copies the slot into the texture Flutter samples right after
    // this returns, on the same immediate context, so the blit has to be
    // complete before it can read the slot.
    context_->End(frame_done_.get());
    context_->Flush();
    const ULONGLONG deadline = GetTickCount64() + kBlitTimeoutMs;
    for (;;) {
        const HRESULT hr = context_->GetData(frame_done_.get(), nullptr, 0, 0);
        if (hr == S_OK) {
            return true;
        }
        if (hr != S_FALSE) {
            set_error(fail_hr("GetData(frame done)", hr));
            return false;
        }
        if (GetTickCount64() >= deadline) {
            set_error("the display GPU did not finish a frame in time");
            return false;
        }
        Sleep(1);
    }
}

bool GpuFrameSink::OnGpuFrame(ID3D11Texture2D* nv12, uint32_t subresource, uint32_t width,
                              uint32_t height, uint32_t crop_x, uint32_t crop_y,
                              uint64_t pts_ms) {
    (void)pts_ms;
    if (!ready_ || nv12 == nullptr || width == 0 || height == 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    nv12->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) {
        set_error("the decoder surface is not NV12");
        return false;
    }
    if (subresource >= desc.ArraySize) {
        set_error("the decoder surface slice is out of range");
        return false;
    }
    if (!ensure_pipeline(desc, width, height)) {
        return false;
    }

    void* output = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output = output_;
    }
    if (output == nullptr || set_texture_ == nullptr) {
        return false;
    }

    Slot& slot = slots_[next_slot_];
    if (!render(slot, nv12, subresource, width, height, crop_x, crop_y)) {
        return false;
    }
    // The plugin copies the slot into the single texture it registered with
    // Flutter and marks that frame available. GPU work on one immediate
    // context is ordered, so the next pass over this slot cannot overwrite it
    // before the copy has read it.
    set_texture_(output, slot.texture.get());
    next_slot_ = (next_slot_ + 1) % kSlotCount;
    return true;
}

void GpuFrameSink::set_error(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) {
        error_ = std::move(message);
    }
}

}  // namespace chast_viewer
