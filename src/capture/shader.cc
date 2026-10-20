// Shader compilation for the GPU stages.
//
// d3dcompiler_47.dll is delay-loaded (see CMakeLists) so the capture library
// loads on systems where it is absent; availability() is the runtime gate the
// capturer checks before asking DXGI for FP16 frames.

#include "capture/shader.h"

#include <d3dcompiler.h>

#include <atomic>
#include <cstring>

namespace chast::detail {

namespace {

std::atomic<bool> g_checked{false};
std::atomic<bool> g_available{false};

struct CompiledBlob {
    ComPtr<ID3DBlob> code;
    std::string diagnostics;
};

Error compile(const char* source, const char* entry, const char* target,
              std::string_view debug_name, CompiledBlob& out) {
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif

    HRESULT hr = D3DCompile(source, std::strlen(source), debug_name.data(), nullptr, nullptr,
                            entry, target, flags, 0, out.code.put(), errors.put());
    if (errors) {
        out.diagnostics.assign(static_cast<const char*>(errors->GetBufferPointer()),
                               errors->GetBufferSize());
    }
    if (FAILED(hr)) {
        return Error(Status::Unsupported,
                     "D3DCompile " + std::string(debug_name) + " failed: " +
                         hresult_message(hr) + (out.diagnostics.empty() ? ""
                                                                       : ", " + out.diagnostics));
    }
    return Error::success();
}

}  // namespace

bool shader_support_available() {
    if (!g_checked.load(std::memory_order_acquire)) {
        CompiledBlob blob;
        bool ok = compile(kFullscreenVertexShader, "main", "vs_5_0", "fullscreen.vs", blob).ok();
        if (ok) {
            ok = compile(kHdrToSdrPixelShader, "main", "ps_5_0", "hdr_to_sdr.ps", blob).ok();
        }
        g_available.store(ok, std::memory_order_release);
        g_checked.store(true, std::memory_order_release);
    }
    return g_available.load(std::memory_order_acquire);
}

Error compile_shader_bytecode(const char* source, const char* entry, const char* target,
                              std::string_view debug_name, ID3DBlob** out) {
    CompiledBlob blob;
    Error err = compile(source, entry, target, debug_name, blob);
    if (!err.ok()) {
        return err;
    }
    *out = blob.code.detach();
    return Error::success();
}

Error create_vertex_shader(ID3D11Device* device, const char* source, const char* entry,
                           std::string_view debug_name, ID3D11VertexShader** out) {
    ComPtr<ID3DBlob> blob;
    Error err = compile_shader_bytecode(source, entry, "vs_5_0", debug_name, blob.put());
    if (!err.ok()) {
        return err;
    }
    return check(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                            nullptr, out),
                 "CreateVertexShader");
}

Error create_pixel_shader(ID3D11Device* device, const char* source, const char* entry,
                          std::string_view debug_name, ID3D11PixelShader** out) {
    ComPtr<ID3DBlob> blob;
    Error err = compile_shader_bytecode(source, entry, "ps_5_0", debug_name, blob.put());
    if (!err.ok()) {
        return err;
    }
    return check(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, out),
                 "CreatePixelShader");
}

}  // namespace chast::detail

