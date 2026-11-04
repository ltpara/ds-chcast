// Measures the cost of GpuFrameSink::wait_for_gpu().
//
// The sink's per-frame path is modelled exactly: an NV12 texture is converted
// to BGRA into one of three shared slots with VideoProcessorBlt, then the
// flutter_gpu_texture_renderer plugin's copy is emulated by CopyResource on
// the same D3D11 immediate context (the plugin takes the device from the
// texture it is handed, then copies and flushes on that device).
//
// Modes:
//   current  - VideoProcessorBlt, End(event), Flush, GetData + Sleep(1), then
//              the plugin copy and its Flush, i.e. today's sink.
//   no-wait  - VideoProcessorBlt, plugin copy, Flush. Same-context ordering
//              already serializes the copy after the blit.
//   bounded  - no per-frame wait; a ring of three event queries blocks only
//              when the frame three submissions ago has not completed, which
//              keeps the CPU from running ahead without stalling every frame.

#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d10_1.h>
#include <dxgi1_2.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kSlotCount = 3;
constexpr ULONGLONG kTimeoutMs = 2000;

enum class Mode {
    Current,
    NoWait,
    Bounded,
};

const char* mode_name(Mode mode) {
    switch (mode) {
        case Mode::Current:
            return "current";
        case Mode::NoWait:
            return "no-wait";
        case Mode::Bounded:
            return "bounded(3)";
    }
    return "?";
}

[[noreturn]] void fatal(const std::string& message) {
    std::fprintf(stderr, "gpu-sync-bench: %s\n", message.c_str());
    std::exit(1);
}

std::string hresult_text(const char* what, HRESULT hr) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lx)", what,
                  static_cast<unsigned long>(hr));
    return buffer;
}

uint32_t align16(uint32_t value) {
    return (value + 15u) & ~15u;
}

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double percentile(const std::vector<double>& sorted, double fraction) {
    size_t index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

struct LatencyStats {
    double mean = 0;
    double p50 = 0;
    double p95 = 0;
    double max = 0;
};

LatencyStats latency_stats(std::vector<double> samples) {
    LatencyStats stats;
    if (samples.empty()) {
        return stats;
    }
    double sum = 0;
    for (double sample : samples) {
        sum += sample;
    }
    stats.mean = sum / static_cast<double>(samples.size());
    std::sort(samples.begin(), samples.end());
    stats.p50 = percentile(samples, 0.50);
    stats.p95 = percentile(samples, 0.95);
    stats.max = samples.back();
    return stats;
}

struct Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
};

struct ThroughputStats {
    double loop_ms = 0;
    double stall_ms = 0;
    double total_ms = 0;
    double fps = 0;
};

struct PacedStats {
    double stall_mean_ms = 0;
    double stall_p95_ms = 0;
    double submit_mean_ms = 0;
    LatencyStats latency;
};

class Bench {
public:
    Bench(uint32_t width, uint32_t height)
        : width_(width), height_(height), coded_height_(align16(height)) {}

    bool init(std::string* error) {
        if (!create_device(error)) {
            return false;
        }
        if (!create_input(error)) {
            return false;
        }
        if (!create_processor(error)) {
            return false;
        }
        if (!create_slots(error)) {
            return false;
        }
        if (!create_queries(error)) {
            return false;
        }
        return true;
    }

    const std::string& adapter_name() const { return adapter_name_; }

    bool check_picture(std::string* error) {
        ComPtr<ID3D11VideoProcessorInputView> view = make_input_view(error);
        if (!view) {
            return false;
        }
        HRESULT hr = blit(0, view.Get());
        if (FAILED(hr)) {
            *error = hresult_text("VideoProcessorBlt", hr);
            return false;
        }
        context_->CopyResource(plugin_texture_.Get(), slots_[0].texture.Get());
        context_->End(final_query_.Get());
        context_->Flush();
        if (!wait_query(final_query_.Get(), true, error)) {
            return false;
        }

        D3D11_TEXTURE2D_DESC staging_desc{};
        plugin_texture_->GetDesc(&staging_desc);
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        hr = device_->CreateTexture2D(&staging_desc, nullptr, staging.GetAddressOf());
        if (FAILED(hr)) {
            *error = hresult_text("CreateTexture2D(staging)", hr);
            return false;
        }
        context_->CopyResource(staging.Get(), plugin_texture_.Get());
        context_->End(final_query_.Get());
        context_->Flush();
        if (!wait_query(final_query_.Get(), true, error)) {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            *error = hresult_text("Map(staging)", hr);
            return false;
        }
        const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                          static_cast<size_t>(height_ / 2) * mapped.RowPitch;
        const uint8_t blue = row[static_cast<size_t>(width_ / 2) * 4 + 0];
        const uint8_t green = row[static_cast<size_t>(width_ / 2) * 4 + 1];
        const uint8_t red = row[static_cast<size_t>(width_ / 2) * 4 + 2];
        context_->Unmap(staging.Get(), 0);

        std::printf("readback: center pixel B=%u G=%u R=%u\n", blue, green, red);
        if (red < 128 || green < 128 || blue < 128) {
            *error = "the video processor produced a dark picture; the benchmark is not exercising the blit";
            return false;
        }
        return true;
    }

    ThroughputStats run_throughput(Mode mode, int frames, int repetitions) {
        std::vector<double> loop_times;
        std::vector<double> stall_times;
        std::vector<double> total_times;
        for (int repeat = 0; repeat < repetitions; ++repeat) {
            ThroughputStats sample = run_throughput_once(mode, frames);
            loop_times.push_back(sample.loop_ms);
            stall_times.push_back(sample.stall_ms);
            total_times.push_back(sample.total_ms);
        }
        ThroughputStats result;
        result.loop_ms = median(loop_times);
        result.stall_ms = median(stall_times);
        result.total_ms = median(total_times);
        result.fps = static_cast<double>(frames) * 1000.0 / result.total_ms;
        return result;
    }

    LatencyStats run_latency(Mode mode, int samples) {
        run_frames(mode, 40, nullptr);
        drain();

        std::vector<double> times;
        times.reserve(static_cast<size_t>(samples));
        for (int i = 0; i < samples; ++i) {
            // Let the previous sample complete so the measurement is the
            // sink's contribution, not queueing from the prior frame.
            Sleep(5);
            const size_t slot = next_slot_;
            next_slot_ = (next_slot_ + 1) % kSlotCount;

            std::string error;
            ComPtr<ID3D11VideoProcessorInputView> view = make_input_view(&error);
            if (!view) {
                fatal(error);
            }

            const Clock::time_point start = Clock::now();
            HRESULT hr = blit(slot, view.Get());
            if (FAILED(hr)) {
                fatal(hresult_text("VideoProcessorBlt", hr));
            }
            if (mode == Mode::Current) {
                context_->End(current_event_.Get());
                context_->Flush();
                if (!wait_query(current_event_.Get(), true, &error)) {
                    fatal(error);
                }
            }
            context_->CopyResource(plugin_texture_.Get(), slots_[slot].texture.Get());
            context_->End(latency_query_.Get());
            context_->Flush();
            // Busy-wait so the timestamp is not quantized by Sleep(1); this
            // poll policy is identical for every mode.
            if (!wait_query(latency_query_.Get(), false, &error)) {
                fatal(error);
            }
            times.push_back(elapsed_ms(start, Clock::now()));
        }
        return latency_stats(std::move(times));
    }

    PacedStats run_paced(Mode mode, int frames, int latency_samples) {
        run_frames(mode, 40, nullptr);
        drain();

        std::vector<double> stalls;
        std::vector<double> submits;
        std::vector<double> latency;
        stalls.reserve(static_cast<size_t>(frames));
        submits.reserve(static_cast<size_t>(frames));
        latency.reserve(static_cast<size_t>(latency_samples));

        const auto interval = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double, std::milli>(1000.0 / 60.0));
        const int sample_every =
            std::max(1, frames / std::max(1, latency_samples));
        Clock::time_point next = Clock::now();
        for (int i = 0; i < frames; ++i) {
            next += interval;
            std::this_thread::sleep_until(next);
            const Clock::time_point start = Clock::now();
            double stall = 0;
            run_one_frame(mode, static_cast<uint64_t>(i), &stall);
            const double submit = elapsed_ms(start, Clock::now()) - stall;
            stalls.push_back(stall);
            submits.push_back(submit);

            if (i % sample_every == 0) {
                std::string error;
                context_->End(latency_query_.Get());
                context_->Flush();
                if (!wait_query(latency_query_.Get(), false, &error)) {
                    fatal(error);
                }
                latency.push_back(elapsed_ms(start, Clock::now()));
                // A blocking sample can consume the next deadline; restart
                // the schedule rather than submitting a burst of frames.
                next = Clock::now();
            }
        }
        drain();

        PacedStats stats;
        double stall_sum = 0;
        double submit_sum = 0;
        for (double value : stalls) {
            stall_sum += value;
        }
        for (double value : submits) {
            submit_sum += value;
        }
        stats.stall_mean_ms = stall_sum / static_cast<double>(stalls.size());
        std::vector<double> sorted_stalls = stalls;
        std::sort(sorted_stalls.begin(), sorted_stalls.end());
        stats.stall_p95_ms = percentile(sorted_stalls, 0.95);
        stats.submit_mean_ms = submit_sum / static_cast<double>(submits.size());
        stats.latency = latency_stats(std::move(latency));
        return stats;
    }

private:
    bool create_device(std::string* error) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
        };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       device_.GetAddressOf(), nullptr,
                                       context_.GetAddressOf());
        if (hr == E_INVALIDARG) {
            const D3D_FEATURE_LEVEL fallback[] = {
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0,
            };
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   fallback, ARRAYSIZE(fallback), D3D11_SDK_VERSION,
                                   device_.GetAddressOf(), nullptr,
                                   context_.GetAddressOf());
        }
        if (FAILED(hr) || !device_ || !context_) {
            *error = hresult_text("D3D11CreateDevice", hr);
            return false;
        }

        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(device_.As(&dxgi_device))) {
            dxgi_device->GetAdapter(&adapter);
        }
        if (adapter) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                char name[160] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
                                    static_cast<int>(sizeof(name)), nullptr, nullptr);
                adapter_name_ = name;
            }
        }

        device_.As(&video_device_);
        context_.As(&video_context_);
        context_.As(&video_context1_);
        if (!video_device_ || !video_context_ || !video_context1_) {
            *error = "the GPU has no D3D11 video processor interfaces";
            return false;
        }

        ComPtr<ID3D10Multithread> multithread;
        if (SUCCEEDED(context_.As(&multithread)) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
        }
        return true;
    }

    bool create_input(std::string* error) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width_;
        desc.Height = coded_height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, input_.GetAddressOf());
        if (FAILED(hr)) {
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr = device_->CreateTexture2D(&desc, nullptr, input_.GetAddressOf());
        }
        if (FAILED(hr)) {
            *error = hresult_text("CreateTexture2D(NV12)", hr);
            return false;
        }

        std::vector<uint8_t> image(static_cast<size_t>(width_) * coded_height_ * 3 / 2,
                                   uint8_t{128});
        std::fill(image.begin(),
                  image.begin() + static_cast<size_t>(width_) * coded_height_,
                  uint8_t{235});
        context_->UpdateSubresource(input_.Get(), 0, nullptr, image.data(), width_, 0);
        return true;
    }

    bool create_processor(std::string* error) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate.Numerator = 60;
        content.InputFrameRate.Denominator = 1;
        content.InputWidth = width_;
        content.InputHeight = coded_height_;
        content.OutputFrameRate = content.InputFrameRate;
        content.OutputWidth = width_;
        content.OutputHeight = height_;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = video_device_->CreateVideoProcessorEnumerator(
            &content, enumerator_.GetAddressOf());
        if (FAILED(hr) || !enumerator_) {
            *error = hresult_text("CreateVideoProcessorEnumerator", hr);
            return false;
        }

        UINT output_flags = 0;
        hr = enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &output_flags);
        if (FAILED(hr) ||
            (output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
            *error = "the GPU cannot render BGRA8 through the video processor";
            return false;
        }

        hr = video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                                 processor_.GetAddressOf());
        if (FAILED(hr) || !processor_) {
            *error = hresult_text("CreateVideoProcessor", hr);
            return false;
        }

        video_context1_->VideoProcessorSetStreamColorSpace1(
            processor_.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
        video_context1_->VideoProcessorSetOutputColorSpace1(
            processor_.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        video_context_->VideoProcessorSetStreamFrameFormat(
            processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
        D3D11_VIDEO_COLOR background{};
        background.RGBA.R = 0.0f;
        background.RGBA.G = 0.0f;
        background.RGBA.B = 0.0f;
        background.RGBA.A = 1.0f;
        video_context_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), TRUE,
                                                               &background);
        return true;
    }

    bool create_slots(std::string* error) {
        for (size_t i = 0; i < kSlotCount; ++i) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width_;
            desc.Height = height_;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            HRESULT hr = device_->CreateTexture2D(&desc, nullptr,
                                                  slots_[i].texture.GetAddressOf());
            if (FAILED(hr) || !slots_[i].texture) {
                *error = hresult_text("CreateTexture2D(slot)", hr);
                return false;
            }

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
            view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            view_desc.Texture2D.MipSlice = 0;
            hr = video_device_->CreateVideoProcessorOutputView(
                slots_[i].texture.Get(), enumerator_.Get(), &view_desc,
                slots_[i].output_view.GetAddressOf());
            if (FAILED(hr) || !slots_[i].output_view) {
                *error = hresult_text("CreateVideoProcessorOutputView", hr);
                return false;
            }
        }

        // flutter_gpu_texture_renderer owns a texture per registered Flutter
        // texture and shares it through a handle; create the same shape here.
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width_;
        desc.Height = height_;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr,
                                              plugin_texture_.GetAddressOf());
        if (FAILED(hr) || !plugin_texture_) {
            *error = hresult_text("CreateTexture2D(plugin)", hr);
            return false;
        }
        ComPtr<IDXGIResource> resource;
        HANDLE shared_handle = nullptr;
        if (SUCCEEDED(plugin_texture_.As(&resource))) {
            resource->GetSharedHandle(&shared_handle);
        }
        if (shared_handle == nullptr) {
            *error = "the plugin texture could not be shared";
            return false;
        }
        return true;
    }

    bool create_queries(std::string* error) {
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_EVENT;
        HRESULT hr = device_->CreateQuery(&desc, current_event_.GetAddressOf());
        if (FAILED(hr)) {
            *error = hresult_text("CreateQuery(current)", hr);
            return false;
        }
        for (size_t i = 0; i < kSlotCount; ++i) {
            hr = device_->CreateQuery(&desc, bounded_[i].GetAddressOf());
            if (FAILED(hr)) {
                *error = hresult_text("CreateQuery(bounded)", hr);
                return false;
            }
        }
        hr = device_->CreateQuery(&desc, latency_query_.GetAddressOf());
        if (FAILED(hr)) {
            *error = hresult_text("CreateQuery(latency)", hr);
            return false;
        }
        hr = device_->CreateQuery(&desc, final_query_.GetAddressOf());
        if (FAILED(hr)) {
            *error = hresult_text("CreateQuery(final)", hr);
            return false;
        }
        return true;
    }

    ComPtr<ID3D11VideoProcessorInputView> make_input_view(std::string* error) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.ArraySlice = 0;
        ComPtr<ID3D11VideoProcessorInputView> view;
        HRESULT hr = video_device_->CreateVideoProcessorInputView(
            input_.Get(), enumerator_.Get(), &desc, view.GetAddressOf());
        if (FAILED(hr) || !view) {
            *error = hresult_text("CreateVideoProcessorInputView", hr);
            return nullptr;
        }
        return view;
    }

    HRESULT blit(size_t slot, ID3D11VideoProcessorInputView* view) {
        RECT source{};
        source.right = static_cast<LONG>(width_);
        source.bottom = static_cast<LONG>(height_);
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE,
                                                          &source);
        RECT dest{};
        dest.right = static_cast<LONG>(width_);
        dest.bottom = static_cast<LONG>(height_);
        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dest);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.pInputSurface = view;
        return video_context_->VideoProcessorBlt(processor_.Get(),
                                                 slots_[slot].output_view.Get(), 0, 1,
                                                 &stream);
    }

    bool wait_query(ID3D11Query* query, bool sleep, std::string* error) {
        const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
        for (;;) {
            const HRESULT hr = context_->GetData(query, nullptr, 0, 0);
            if (hr == S_OK) {
                return true;
            }
            if (hr != S_FALSE) {
                *error = hresult_text("GetData(query)", hr);
                return false;
            }
            if (GetTickCount64() >= deadline) {
                *error = "a GPU event did not complete within 2 s";
                return false;
            }
            if (sleep) {
                Sleep(1);
            } else {
                YieldProcessor();
            }
        }
    }

    void drain() {
        std::string error;
        context_->End(final_query_.Get());
        context_->Flush();
        if (!wait_query(final_query_.Get(), true, &error)) {
            fatal(error);
        }
    }

    void run_frames(Mode mode, int frames, double* stall_ms) {
        for (int i = 0; i < frames; ++i) {
            run_one_frame(mode, static_cast<uint64_t>(i), stall_ms);
        }
    }

    void run_one_frame(Mode mode, uint64_t frame_index, double* stall_ms) {
        const size_t slot = next_slot_;
        next_slot_ = (next_slot_ + 1) % kSlotCount;

        std::string error;
        ComPtr<ID3D11VideoProcessorInputView> view = make_input_view(&error);
        if (!view) {
            fatal(error);
        }

        if (mode == Mode::Bounded && frame_index >= kSlotCount) {
            const Clock::time_point start = Clock::now();
            if (!wait_query(bounded_[frame_index % kSlotCount].Get(), true, &error)) {
                fatal(error);
            }
            if (stall_ms != nullptr) {
                *stall_ms += elapsed_ms(start, Clock::now());
            }
        }

        HRESULT hr = blit(slot, view.Get());
        if (FAILED(hr)) {
            fatal(hresult_text("VideoProcessorBlt", hr));
        }

        if (mode == Mode::Current) {
            context_->End(current_event_.Get());
            context_->Flush();
            const Clock::time_point start = Clock::now();
            if (!wait_query(current_event_.Get(), true, &error)) {
                fatal(error);
            }
            if (stall_ms != nullptr) {
                *stall_ms += elapsed_ms(start, Clock::now());
            }
        }

        context_->CopyResource(plugin_texture_.Get(), slots_[slot].texture.Get());
        if (mode == Mode::Bounded) {
            context_->End(bounded_[frame_index % kSlotCount].Get());
        }
        context_->Flush();
    }

    ThroughputStats run_throughput_once(Mode mode, int frames) {
        run_frames(mode, 40, nullptr);
        drain();

        ThroughputStats stats;
        const Clock::time_point loop_start = Clock::now();
        run_frames(mode, frames, &stats.stall_ms);
        const Clock::time_point loop_end = Clock::now();
        stats.loop_ms = elapsed_ms(loop_start, loop_end);

        std::string error;
        context_->End(final_query_.Get());
        context_->Flush();
        if (!wait_query(final_query_.Get(), true, &error)) {
            fatal(error);
        }
        stats.total_ms = elapsed_ms(loop_start, Clock::now());
        return stats;
    }

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t coded_height_ = 0;
    std::string adapter_name_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoContext1> video_context1_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11Texture2D> input_;
    Slot slots_[kSlotCount];
    ComPtr<ID3D11Texture2D> plugin_texture_;

    ComPtr<ID3D11Query> current_event_;
    ComPtr<ID3D11Query> bounded_[kSlotCount];
    ComPtr<ID3D11Query> latency_query_;
    ComPtr<ID3D11Query> final_query_;
    size_t next_slot_ = 0;
};

double measure_sleep_ms(int calls) {
    const Clock::time_point start = Clock::now();
    for (int i = 0; i < calls; ++i) {
        Sleep(1);
    }
    return elapsed_ms(start, Clock::now()) / static_cast<double>(calls);
}

struct Resolution {
    const char* name;
    uint32_t width;
    uint32_t height;
};

}  // namespace

int main(int argc, char** argv) {
    int frames = 600;
    int samples = 200;
    int repetitions = 3;
    int paced_frames = 240;
    bool request_high_resolution_timer = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            samples = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repetitions = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--paced-frames") == 0 && i + 1 < argc) {
            paced_frames = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-timer") == 0) {
            request_high_resolution_timer = false;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--frames N] [--samples N] [--repeat N] "
                         "[--paced-frames N] [--no-timer]\n",
                         argv[0]);
            return 2;
        }
    }
    if (frames < 10 || samples < 10 || repetitions < 1 || paced_frames < 60) {
        std::fprintf(stderr, "frames, samples, repeat and paced frames must be positive\n");
        return 2;
    }

    const double sleep_before = measure_sleep_ms(30);
    if (request_high_resolution_timer) {
        timeBeginPeriod(1);
    }
    const double sleep_after = measure_sleep_ms(30);
    std::printf("timer resolution: %s (Sleep(1): %.2f ms before, %.2f ms after)\n",
                request_high_resolution_timer ? "timeBeginPeriod(1)" : "unchanged",
                sleep_before, sleep_after);

    const Resolution resolutions[] = {
        {"1080p", 1920, 1080},
        {"4K", 3840, 2160},
    };

    for (const Resolution& resolution : resolutions) {
        Bench bench(resolution.width, resolution.height);
        std::string error;
        if (!bench.init(&error)) {
            fatal(error);
        }
        std::printf("\nGPU: %s\n", bench.adapter_name().c_str());
        std::printf("resolution: %s, NV12 %ux%u -> BGRA %ux%u, %zu shared slots\n",
                    resolution.name, resolution.width, align16(resolution.height),
                    resolution.width, resolution.height, kSlotCount);
        if (!bench.check_picture(&error)) {
            fatal(error);
        }

        std::printf("\nthroughput (median of %d runs, %d frames each):\n", repetitions,
                    frames);
        std::printf("%-11s %10s %14s %14s %10s %9s %12s\n", "mode", "loop ms",
                    "stall/frame", "submit/frame", "total ms", "fps", "gpu ms/frame");
        for (Mode mode : {Mode::Current, Mode::NoWait, Mode::Bounded}) {
            ThroughputStats stats = bench.run_throughput(mode, frames, repetitions);
            const double stall_us = stats.stall_ms * 1000.0 / frames;
            const double submit_us =
                (stats.loop_ms - stats.stall_ms) * 1000.0 / frames;
            std::printf("%-11s %10.2f %11.1f us %11.1f us %10.2f %9.1f %12.3f\n",
                        mode_name(mode), stats.loop_ms, stall_us, submit_us,
                        stats.total_ms, stats.fps, stats.total_ms / frames);
        }

        std::printf("\nlatency, submit -> plugin copy complete (%d samples):\n", samples);
        std::printf("%-11s %10s %10s %10s %10s\n", "mode", "mean ms", "p50 ms",
                    "p95 ms", "max ms");
        for (Mode mode : {Mode::Current, Mode::NoWait, Mode::Bounded}) {
            LatencyStats stats = bench.run_latency(mode, samples);
            std::printf("%-11s %10.2f %10.2f %10.2f %10.2f\n", mode_name(mode),
                        stats.mean, stats.p50, stats.p95, stats.max);
        }

        std::printf("\npaced at 60 fps, %d frames (20 latency samples):\n", paced_frames);
        std::printf("%-11s %13s %13s %13s %10s %10s\n", "mode", "stall mean",
                    "stall p95", "submit mean", "p50 lat", "p95 lat");
        for (Mode mode : {Mode::Current, Mode::NoWait, Mode::Bounded}) {
            PacedStats stats = bench.run_paced(mode, paced_frames, 20);
            std::printf("%-11s %10.2f ms %10.2f ms %10.2f ms %7.2f ms %7.2f ms\n",
                        mode_name(mode), stats.stall_mean_ms, stats.stall_p95_ms,
                        stats.submit_mean_ms, stats.latency.p50, stats.latency.p95);
        }
    }

    if (request_high_resolution_timer) {
        timeEndPeriod(1);
    }
    return 0;
}
