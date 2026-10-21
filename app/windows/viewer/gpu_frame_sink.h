#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <dxgi1_2.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

#include "chast/com_ptr.h"
#include "viewer_engine.h"

namespace chast_viewer {

// GPU bridge from the decoder's NV12 texture to Flutter, built on
// flutter_gpu_texture_renderer (the plugin RustDesk uses).
//
// The D3D11 video processor converts NV12 -> BGRA into one of three shared
// textures, then the plugin copies that texture into the single shared texture
// it registered with Flutter. Both steps run on the GPU, so the picture never
// enters system memory. The plugin owns the texture Flutter samples; this class
// only borrows its opaque output pointer for the duration of a session.
class GpuFrameSink final : public FrameSink {
public:
    GpuFrameSink();
    ~GpuFrameSink() override;

    GpuFrameSink(const GpuFrameSink&) = delete;
    GpuFrameSink& operator=(const GpuFrameSink&) = delete;

    // `output` is the pointer Dart obtained from the plugin's output(textureId)
    // call. The bridge clears it after the session thread has stopped and
    // before Dart unregisters the texture.
    void SetOutput(void* output);

    bool ready() const noexcept { return ready_; }
    ID3D11Device* device() override;
    bool OnGpuFrame(ID3D11Texture2D* nv12, uint32_t subresource, uint32_t width,
                    uint32_t height, uint32_t crop_x, uint32_t crop_y,
                    uint64_t pts_ms) override;
    std::string error() const override;

private:
    using SetTextureFn = void (*)(void*, void*);
    using GetAdapterLuidFn = int64_t (*)();

    static constexpr size_t kSlotCount = 3;

    struct Slot {
        chast::ComPtr<ID3D11Texture2D> texture;
        chast::ComPtr<ID3D11VideoProcessorOutputView> output_view;
    };

    bool init_device();
    bool resolve_plugin();
    bool ensure_pipeline(const D3D11_TEXTURE2D_DESC& input, uint32_t width,
                         uint32_t height);
    bool render(Slot& slot, ID3D11Texture2D* nv12, uint32_t subresource, uint32_t width,
                uint32_t height, uint32_t crop_x, uint32_t crop_y);
    bool wait_for_gpu();
    void set_error(std::string message);

    chast::ComPtr<ID3D11Device> device_;
    chast::ComPtr<ID3D11DeviceContext> context_;
    chast::ComPtr<ID3D11VideoDevice> video_device_;
    chast::ComPtr<ID3D11VideoContext> video_context_;
    chast::ComPtr<ID3D11VideoContext1> video_context1_;
    chast::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    chast::ComPtr<ID3D11VideoProcessor> processor_;
    chast::ComPtr<ID3D11Query> frame_done_;

    HMODULE plugin_ = nullptr;
    SetTextureFn set_texture_ = nullptr;
    LUID adapter_luid_{};
    bool has_adapter_luid_ = false;

    mutable std::mutex mutex_;
    void* output_ = nullptr;

    // Slots are only touched by the session thread.
    std::array<Slot, kSlotCount> slots_;
    size_t next_slot_ = 0;

    UINT pipeline_input_width_ = 0;
    UINT pipeline_input_height_ = 0;
    UINT pipeline_width_ = 0;
    UINT pipeline_height_ = 0;
    bool pipeline_ready_ = false;

    bool ready_ = false;
    std::string error_;
};

}  // namespace chast_viewer
