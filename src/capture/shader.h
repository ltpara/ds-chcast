#pragma once

// Shader plumbing shared by the GPU stages. HLSL lives here rather than in
// .hlsl files on purpose: the capture library ships as one static binary and
// compiles its passes at runtime, and keeping source next to the code that
// binds the constant buffers makes the ABI (which is easily broken by a
// mismatched struct) visible in review.
//
// The passes target shader model 5.0, which every D3D11 feature level 11_0
// device supports and which the duplication itself already requires.

#include <windows.h>
#include <d3d11.h>

#include <string>
#include <string_view>

#include "chast/com_ptr.h"
#include "chast/error.h"

namespace chast::detail {

// Fullscreen triangle: one draw of three vertices, positions generated from
// SV_VertexID, no vertex buffer. UVs map (0,0) to the top-left of the source,
// matching a texture sampled with default sampler state.
inline constexpr const char* kFullscreenVertexShader = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    // id 0 -> (0,0) uv, id 1 -> (2,0), id 2 -> (0,2) in clip space; the
    // rasterizer clips the oversized triangle into the viewport.
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

// Converts the scRGB desktop into display-referred sRGB BGRA8.
//
// Windows composites Advanced Color desktops in linear scRGB stored as
// R16G16B16A16_FLOAT. For an HDR desktop the values are scene-referred with
// 1.0 == 80 nits, so SDR content sits at the user's SDR white level
// (DISPLAYCONFIG_SDR_WHITE_LEVEL / 1000 * 80 nits) and dividing by it puts SDR
// white back at 1.0. Above that the image is clipped, which is deliberate:
// this pass exists so SDR content looks identical to an SDR capture, and a
// roll-off would have to pull SDR white below 1.0 to make headroom for HDR
// highlights that an 8-bit BGRA viewer cannot show anyway.
//
// For an advanced-color (WCG) SDR desktop 1.0 already is display reference
// white, so the pass only applies the scRGB -> sRGB transfer.
//
// Load() takes pixel coordinates so the pass never resamples: dirty rects and
// the output texture share the exact same grid.
inline constexpr const char* kHdrToSdrPixelShader = R"(
Texture2D<float4> source : register(t0);
cbuffer Params : register(b0) {
    float inv_sdr_white;  // 1.0 / (sdr_white_level / 1000) for HDR, 1.0 for WCG
    float3 pad;
};

float3 linear_to_srgb(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

float4 main(float4 pos : SV_Position) : SV_Target {
    float3 linear = saturate(source.Load(int3(pos.xy, 0)).rgb * inv_sdr_white);
    return float4(linear_to_srgb(linear), 1.0);
}
)";

// True once, process-wide; false on machines without d3dcompiler_47.dll or
// where the shaders fail to compile (e.g. stripped-down Windows Server).
bool shader_support_available();

Error compile_shader_bytecode(const char* source, const char* entry, const char* target,
                              std::string_view debug_name, ID3DBlob** out);
Error create_vertex_shader(ID3D11Device* device, const char* source, const char* entry,
                           std::string_view debug_name, ID3D11VertexShader** out);
Error create_pixel_shader(ID3D11Device* device, const char* source, const char* entry,
                          std::string_view debug_name, ID3D11PixelShader** out);

}  // namespace chast::detail
