#include "capture/detail.h"
#include "capture/shader.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace chast::detail {

namespace {

// Process-wide latch for "this machine cannot normalize FP16 desktops". It is
// per process rather than per capturer because the shader and compiler are
// process-wide: a second capturer on the same machine would fail the same way.
std::atomic<bool> g_unavailable{false};

// Must match cbuffer Params in kHdrToSdrPixelShader.
struct HdrConstants {
    float inv_sdr_white;
    float pad[3];
};
static_assert(sizeof(HdrConstants) == 16, "constant buffer must be 16-byte aligned");

float inverse_white(const HdrPass::Params& params) {
    if (!params.hdr) {
        // Advanced-color SDR: linear scRGB with display reference white at 1.0.
        return 1.0f;
    }
    // Scene-referred scRGB, 1.0 == 80 nits; SDR white sits at the user's
    // configured level in the same units. An unknown level means 80 nits.
    const UINT level = params.sdr_white_level != 0 ? params.sdr_white_level : 1000;
    return 1000.0f / static_cast<float>(level);
}

}  // namespace

bool HdrPass::available() { return !unavailable() && shader_support_available(); }

bool HdrPass::unavailable() {
    return g_unavailable.load(std::memory_order_acquire);
}

void HdrPass::mark_unavailable() {
    g_unavailable.store(true, std::memory_order_release);
}

HdrPass::HdrPass(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device), context_(context) {}

Error HdrPass::convert(ID3D11Texture2D* source, const Params& params, ID3D11Texture2D** out) {
    *out = nullptr;
    if (!source) {
        return Error(Status::InvalidCall, "HDR pass got a null source texture");
    }

    // The desktop texture can change size only across an output mode change,
    // which also recreates the duplication, but the size check keeps a caller
    // that forces a reinit honest.
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);

    Error err = ensure_target(source_desc.Width, source_desc.Height);
    if (!err.ok()) {
        return err;
    }
    err = ensure_source_view(source);
    if (!err.ok()) {
        return err;
    }
    update_params(params);

    ID3D11RenderTargetView* rtv = rtv_.get();
    ID3D11ShaderResourceView* srv = srv_.get();
    ID3D11Buffer* constants = constants_.get();
    const D3D11_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_.get(), nullptr, 0);
    context_->PSSetShader(ps_.get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &constants);
    context_->PSSetShaderResources(0, 1, &srv);
    context_->Draw(3, 0);

    // Unbind so the encoder never samples the desktop image as a bound shader
    // input and the next frame's copy does not fight a live render target.
    ID3D11ShaderResourceView* null_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    *out = target_.get();
    return Error::success();
}

Error HdrPass::ensure_target(UINT width, UINT height) {
    if (target_ && width_ == width && height_ == height) {
        return Error::success();
    }
    if (width == 0 || height == 0) {
        return Error(Status::InvalidCall, "HDR pass target would be empty");
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    // Shared so a hardware encoder on another device can open it by handle
    // instead of copying the frame through the CPU.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    ComPtr<ID3D11Texture2D> target;
    Error err = check(device_->CreateTexture2D(&desc, nullptr, target.put()), "CreateTexture2D(hdr)");
    if (!err.ok()) {
        return err;
    }

    ComPtr<ID3D11RenderTargetView> rtv;
    err = check(device_->CreateRenderTargetView(target.get(), nullptr, rtv.put()),
                "CreateRenderTargetView");
    if (!err.ok()) {
        return err;
    }

    if (!vs_) {
        err = create_vertex_shader(device_.get(), kFullscreenVertexShader, "main", "fullscreen.vs",
                                   vs_.put());
        if (!err.ok()) {
            return err;
        }
        err = create_pixel_shader(device_.get(), kHdrToSdrPixelShader, "main", "hdr_to_sdr.ps",
                                  ps_.put());
        if (!err.ok()) {
            return err;
        }
    }

    if (!constants_) {
        D3D11_BUFFER_DESC buffer_desc{};
        buffer_desc.ByteWidth = sizeof(HdrConstants);
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const HdrConstants init{1.0f, {0.0f, 0.0f, 0.0f}};
        const D3D11_SUBRESOURCE_DATA init_data{&init, 0, 0};
        err = check(device_->CreateBuffer(&buffer_desc, &init_data, constants_.put()),
                    "CreateBuffer(constants)");
        if (!err.ok()) {
            return err;
        }
        params_dirty_ = true;
    }

    target_ = std::move(target);
    rtv_ = std::move(rtv);
    srv_.reset();
    srv_source_ = nullptr;
    width_ = width;
    height_ = height;
    return Error::success();
}

Error HdrPass::ensure_source_view(ID3D11Texture2D* source) {
    if (srv_ && srv_source_ == source) {
        return Error::success();
    }
    // One view per desktop texture address, not per texture: the duplication
    // hands back the same texture until the duplication is recreated, so the
    // address is stable within a frame and the view is built once.
    ComPtr<ID3D11ShaderResourceView> srv;
    Error err = check(device_->CreateShaderResourceView(source, nullptr, srv.put()),
                      "CreateShaderResourceView(hdr source)");
    if (!err.ok()) {
        return err;
    }
    srv_ = std::move(srv);
    srv_source_ = source;
    return Error::success();
}

void HdrPass::update_params(const Params& params) {
    const float inv_white = inverse_white(params);
    if (!params_dirty_ && inv_white == inv_white_) {
        return;
    }
    HdrConstants data{inv_white, {0.0f, 0.0f, 0.0f}};
    context_->UpdateSubresource(constants_.get(), 0, nullptr, &data, 0, 0);
    inv_white_ = inv_white;
    params_dirty_ = false;
}

HANDLE HdrPass::shared_handle() const {
    if (!target_) {
        return nullptr;
    }
    ComPtr<IDXGIResource> resource = com_query<IDXGIResource>(target_);
    HANDLE handle = nullptr;
    if (resource && FAILED(resource->GetSharedHandle(&handle))) {
        return nullptr;
    }
    // The handle is owned by the texture and stays valid for the capturer's
    // lifetime; it must not be closed by the caller.
    return handle;
}

}  // namespace chast::detail
