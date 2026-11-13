// GPU color and geometry stages against known textures: FP16 scRGB -> BGRA8
// normalization, video-processor rotation and the BGRA -> NV12 downscale.
// These run on the real adapter because the video processor is exactly the
// part a mocked device would not exercise. Each stage reports a skip when the
// machine cannot provide it (no shader compiler, no rotation caps), so the
// check still runs on a plain desktop.

#include <windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "capture/detail.h"
#include "capture/rotation.h"
#include "capture/shader.h"

using chast::ComPtr;
using chast::Error;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void skip(const char* what) {
    std::printf("skip %s\n", what);
}

void check_near(int actual, int expected, int tolerance, const char* what) {
    const bool ok = std::abs(actual - expected) <= tolerance;
    std::printf("%s %s (got %d, expected %d +/- %d)\n", ok ? "ok  " : "FAIL", what, actual,
                expected, tolerance);
    if (!ok) {
        ++failures;
    }
}

struct Image {
    D3D11_TEXTURE2D_DESC desc{};
    UINT row_pitch = 0;
    std::vector<uint8_t> bytes;  // tightly packed rows
};

bool read_back(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
               Image* out) {
    source->GetDesc(&out->desc);
    D3D11_TEXTURE2D_DESC staging_desc = out->desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, staging.put()))) {
        return false;
    }
    context->CopyResource(staging.get(), source);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const UINT width = out->desc.Width;
    const UINT height = out->desc.Height;
    const bool nv12 = out->desc.Format == DXGI_FORMAT_NV12;
    // BGRA8 is four bytes per pixel; NV12 stores one luma byte per pixel and
    // one interleaved chroma pair per 2x2 block, and D3D11 keeps the same row
    // pitch for both planes.
    const UINT row_bytes = nv12 ? width : width * 4;
    const UINT plane_height = nv12 ? height + height / 2 : height;
    out->row_pitch = mapped.RowPitch;
    out->bytes.resize(static_cast<size_t>(row_bytes) * plane_height);
    const auto* rows = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < plane_height; ++y) {
        std::memcpy(out->bytes.data() + static_cast<size_t>(y) * row_bytes,
                    rows + static_cast<size_t>(y) * mapped.RowPitch, row_bytes);
    }
    context->Unmap(staging.get(), 0);
    return true;
}

const uint8_t* pixel(const Image& image, UINT x, UINT y) {
    return image.bytes.data() + static_cast<size_t>(y) * image.desc.Width * 4 +
           static_cast<size_t>(x) * 4;
}

const uint8_t* luma(const Image& image, UINT x, UINT y) {
    return image.bytes.data() + static_cast<size_t>(y) * image.desc.Width + x;
}

bool create_device(ComPtr<ID3D11Device>* device, ComPtr<ID3D11DeviceContext>* context) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_1;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, device->put(), &got,
                                   context->put());
    if (FAILED(hr)) {
        return false;
    }
    return true;
}

ComPtr<ID3D11Texture2D> create_texture(ID3D11Device* device, DXGI_FORMAT format, UINT width,
                                       UINT height, const void* initial, UINT initial_pitch) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // The video processor accepts these as input views, and the shared flag
    // matches the surfaces the capturer publishes.
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    D3D11_SUBRESOURCE_DATA data{};
    const D3D11_SUBRESOURCE_DATA* data_ptr = nullptr;
    if (initial) {
        data.pSysMem = initial;
        data.SysMemPitch = initial_pitch;
        data_ptr = &data;
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, data_ptr, texture.put()))) {
        return {};
    }
    return texture;
}

void test_hdr(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!chast::detail::HdrPass::available()) {
        ID3DBlob* blob = nullptr;
        const Error err = chast::detail::compile_shader_bytecode(
            chast::detail::kHdrToSdrPixelShader, "main", "ps_5_0", "hdr_to_sdr.ps", &blob);
        std::printf("skip HDR normalization needs D3DCompile: %s\n", err.to_string().c_str());
        if (blob) {
            blob->Release();
        }
        return;
    }
    // One row of scene-referred values: SDR reference white, mid-grey, white
    // and a bright highlight that must saturate. The desktop texture is
    // R16G16B16A16_FLOAT, so pack the reference values as halves rather than
    // handing D3D11 float32 memory it would read as half pairs.
    const float source[4][4] = {
        {0.18f, 0.18f, 0.18f, 1.0f},
        {0.5f, 0.5f, 0.5f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {100.0f, 100.0f, 100.0f, 1.0f},
    };
    uint16_t half_source[4][4];
    for (size_t i = 0; i < 4; ++i) {
        for (size_t c = 0; c < 4; ++c) {
            half_source[i][c] = DirectX::PackedVector::XMConvertFloatToHalf(source[i][c]);
        }
    }
    ComPtr<ID3D11Texture2D> texture = create_texture(
        device, DXGI_FORMAT_R16G16B16A16_FLOAT, 4, 1, half_source, sizeof(half_source[0]));
    if (!texture) {
        skip("HDR source texture creation failed");
        return;
    }

    chast::detail::HdrPass pass(device, context);
    chast::detail::HdrPass::Params params;
    params.hdr = false;
    params.sdr_white_level = 1000;
    params.max_luminance = 0.f;
    ID3D11Texture2D* converted = nullptr;
    Error err = pass.convert(texture.get(), params, &converted);
    check(err.ok() && converted != nullptr, "HDR pass converts an FP16 desktop");
    if (!err.ok() || !converted) {
        std::printf("     %s\n", err.to_string().c_str());
        return;
    }
    check(pass.shared_handle() != nullptr, "HDR target is shared for encoder import");
    Image image;
    check(read_back(device, context, converted, &image), "HDR target can be read back");
    if (image.desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || image.desc.Width != 4) {
        check(false, "HDR target is BGRA8 at the source size");
        return;
    }
    // sRGB transfer: 0.18 -> 0.461 (118), 0.5 -> 0.735 (187-188).
    check_near(pixel(image, 0, 0)[2], 118, 2, "scRGB 0.18 becomes sRGB mid-grey");
    check_near(pixel(image, 1, 0)[2], 187, 2, "scRGB 0.5 becomes sRGB 187");
    check_near(pixel(image, 2, 0)[2], 255, 0, "scRGB 1.0 becomes white");
    check_near(pixel(image, 3, 0)[2], 255, 0, "scRGB highlights saturate");
    check_near(pixel(image, 0, 0)[3], 255, 0, "HDR target is opaque");

    // The user's white level is the scene-referred value of SDR white; at
    // 2000 it is half of scRGB 1.0, so the same source gets one stop darker.
    params.hdr = true;
    params.sdr_white_level = 2000;
    err = pass.convert(texture.get(), params, &converted);
    check(err.ok() && converted != nullptr, "HDR pass applies a new white level");
    if (!err.ok() || !converted || !read_back(device, context, converted, &image)) {
        check(false, "HDR target can be read back after the white-level change");
        return;
    }
    check_near(pixel(image, 0, 0)[2], 85, 2, "0.18 at white level 2000 becomes 85");
    check_near(pixel(image, 1, 0)[2], 137, 2, "0.5 at white level 2000 becomes 137");
    check_near(pixel(image, 2, 0)[2], 187, 2, "1.0 at white level 2000 becomes 187");
}

void test_cpu_rotation() {
    constexpr UINT kWidth = 4;
    constexpr UINT kHeight = 2;
    constexpr UINT kPitch = kWidth * 4 + 8;
    std::vector<uint8_t> source(static_cast<size_t>(kPitch) * kHeight, 0xcc);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            uint8_t* p = source.data() + static_cast<size_t>(y) * kPitch + x * 4;
            p[0] = static_cast<uint8_t>(16 + x * 32 + y);
            p[1] = static_cast<uint8_t>(64 + x * 16 + y * 8);
            p[2] = static_cast<uint8_t>(128 + x * 8 + y * 32);
            p[3] = static_cast<uint8_t>(200 + x + y);
        }
    }

    const struct {
        DXGI_MODE_ROTATION rotation;
        UINT width;
        UINT height;
        const char* dimensions;
    } cases[] = {
        {DXGI_MODE_ROTATION_ROTATE90, kHeight, kWidth,
         "CPU rotation 90 swaps the frame dimensions"},
        {DXGI_MODE_ROTATION_ROTATE180, kWidth, kHeight,
         "CPU rotation 180 keeps the frame dimensions"},
        {DXGI_MODE_ROTATION_ROTATE270, kHeight, kWidth,
         "CPU rotation 270 swaps the frame dimensions"},
    };
    for (const auto& c : cases) {
        std::vector<uint8_t> rotated;
        UINT width = 0;
        UINT height = 0;
        const bool ok = chast::detail::rotate_bgra(source.data(), kPitch, kWidth, kHeight,
                                                   c.rotation, &rotated, &width, &height);
        check(ok && width == c.width && height == c.height, c.dimensions);
        if (!ok || width != c.width || height != c.height) {
            continue;
        }

        bool pixels_match = true;
        for (UINT y = 0; y < kHeight && pixels_match; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                const POINT dst =
                    chast::detail::surface_to_output(POINT{static_cast<LONG>(x),
                                                          static_cast<LONG>(y)},
                                                     kWidth, kHeight, c.rotation);
                const uint8_t* expected =
                    source.data() + static_cast<size_t>(y) * kPitch + x * 4;
                const uint8_t* actual =
                    rotated.data() + (static_cast<size_t>(dst.y) * width + dst.x) * 4;
                if (std::memcmp(expected, actual, 4) != 0) {
                    pixels_match = false;
                    break;
                }
            }
        }
        check(pixels_match, "CPU rotation maps every source pixel to screen space");
    }

    const D3D11_BOX dst = chast::detail::destination_box(5, 7, kWidth, kHeight);
    check(dst.left == 5 && dst.top == 7 && dst.right == 9 && dst.bottom == 9,
          "CPU upload keeps a child's non-zero merged-canvas offset");
}

void test_rotation(ID3D11Device* device, ID3D11DeviceContext* context) {
    // Four quadrants so a rotation is distinguishable from a flip or a copy:
    // top-left red, top-right green, bottom-left blue, bottom-right white.
    constexpr UINT kWidth = 64;
    constexpr UINT kHeight = 32;
    std::vector<uint8_t> source(static_cast<size_t>(kWidth) * kHeight * 4);
    struct Color {
        uint8_t b, g, r;
    };
    const Color red{0, 0, 255};
    const Color green{0, 255, 0};
    const Color blue{255, 0, 0};
    const Color white{255, 255, 255};
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            const Color& color = y < kHeight / 2 ? (x < kWidth / 2 ? red : green)
                                                 : (x < kWidth / 2 ? blue : white);
            uint8_t* p = source.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            p[0] = color.b;
            p[1] = color.g;
            p[2] = color.r;
            p[3] = 255;
        }
    }
    ComPtr<ID3D11Texture2D> texture = create_texture(
        device, DXGI_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, source.data(), kWidth * 4);
    if (!texture) {
        skip("rotation source texture creation failed");
        return;
    }

    chast::detail::RotationPass pass;
    Error err = pass.initialize(device, context, DXGI_MODE_ROTATION_ROTATE90, kWidth, kHeight,
                                kHeight, kWidth);
    if (!err.ok() || !pass.ready()) {
        skip("video processor has no rotation capability");
        return;
    }
    ID3D11Texture2D* rotated = nullptr;
    err = pass.rotate(texture.get(), &rotated);
    check(err.ok() && rotated != nullptr, "rotation pass runs on the video processor");
    if (!err.ok() || !rotated) {
        std::printf("     %s\n", err.to_string().c_str());
        return;
    }
    check(pass.shared_handle() != nullptr, "rotated target is shared for encoder import");
    Image image;
    check(read_back(device, context, rotated, &image), "rotated target can be read back");
    check(image.desc.Width == kHeight && image.desc.Height == kWidth,
          "90-degree rotation swaps the frame dimensions");
    if (image.desc.Width != kHeight || image.desc.Height != kWidth) {
        return;
    }
    // D3D11_VIDEO_PROCESSOR_ROTATION_90 is clockwise, so the input's
    // bottom-left quadrant lands top-left, and so on.
    const struct {
        UINT x, y;
        Color color;
        const char* what;
    } quadrants[] = {
        {image.desc.Width / 4, image.desc.Height / 4, blue,
         "rotation 90 maps the bottom-left quadrant to top-left"},
        {image.desc.Width * 3 / 4, image.desc.Height / 4, red,
         "rotation 90 maps the top-left quadrant to top-right"},
        {image.desc.Width / 4, image.desc.Height * 3 / 4, white,
         "rotation 90 maps the bottom-right quadrant to bottom-left"},
        {image.desc.Width * 3 / 4, image.desc.Height * 3 / 4, green,
         "rotation 90 maps the top-right quadrant to bottom-right"},
    };
    for (const auto& q : quadrants) {
        const uint8_t* p = pixel(image, q.x, q.y);
        check_near(p[0], q.color.b, 2, q.what);
        check_near(p[1], q.color.g, 2, q.what);
        check_near(p[2], q.color.r, 2, q.what);
    }
}

void test_nv12_scale(ID3D11Device* device, ID3D11DeviceContext* context) {
    // Left half red, right half blue, both aligned to the 4:2:0 chroma grid.
    constexpr UINT kWidth = 64;
    constexpr UINT kHeight = 64;
    std::vector<uint8_t> source(static_cast<size_t>(kWidth) * kHeight * 4);
    for (UINT y = 0; y < kHeight; ++y) {
        for (UINT x = 0; x < kWidth; ++x) {
            uint8_t* p = source.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            p[0] = x < kWidth / 2 ? 0 : 255;      // B
            p[1] = 0;                              // G
            p[2] = x < kWidth / 2 ? 255 : 0;       // R
            p[3] = 255;
        }
    }
    ComPtr<ID3D11Texture2D> texture = create_texture(
        device, DXGI_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, source.data(), kWidth * 4);
    if (!texture) {
        skip("NV12 source texture creation failed");
        return;
    }

    chast::detail::Nv12Pass pass;
    Error err = pass.initialize(device, context, kWidth, kHeight, 32, 32,
                                chast::Nv12Colorimetry::Bt601Limited);
    if (!err.ok() || !pass.ready()) {
        skip("video processor cannot convert this frame to NV12");
        return;
    }
    check(pass.width() == 32 && pass.height() == 32, "NV12 pass reports the scaled frame");
    check(pass.surface_width() == 32 && pass.surface_height() == 32,
          "NV12 surface is 16-pixel aligned");
    ID3D11Texture2D* converted = nullptr;
    err = pass.convert(texture.get(), &converted);
    check(err.ok() && converted != nullptr, "NV12 pass downscales the BGRA frame on the GPU");
    if (!err.ok() || !converted) {
        std::printf("     %s\n", err.to_string().c_str());
        return;
    }
    check(pass.shared_handle() != nullptr, "NV12 target is shared for encoder import");
    Image image;
    check(read_back(device, context, converted, &image), "NV12 target can be read back");
    if (image.desc.Format != DXGI_FORMAT_NV12 || image.desc.Width != 32 ||
        image.desc.Height != 32) {
        check(false, "NV12 target is 32x32");
        return;
    }
    // The luma plane covers the full-ish [16,235] range: BT.601 red is about
    // Y=81, blue about Y=41. The chroma plane follows the luma plane.
    const uint8_t* uv = image.bytes.data() + static_cast<size_t>(image.desc.Width) *
                                               image.desc.Height;
    check_near(luma(image, 8, 8)[0], 81, 12, "scaled red keeps its BT.601 luma");
    check_near(luma(image, 24, 8)[0], 41, 12, "scaled blue keeps its BT.601 luma");
    const size_t uv_left = static_cast<size_t>(4) * image.desc.Width + 4 * 2;
    const size_t uv_right = static_cast<size_t>(4) * image.desc.Width + 12 * 2;
    check_near(uv[uv_left + 1], 240, 16, "red survives downscale as high V");
    check_near(uv[uv_left], 90, 16, "red survives downscale as low U");
    check_near(uv[uv_right], 240, 16, "blue survives downscale as high U");
    check_near(uv[uv_right + 1], 110, 16, "blue survives downscale as low V");

    // A non-multiple-of-16 output keeps the real size while the surface is
    // padded for oneVPL import.
    chast::detail::Nv12Pass padded;
    err = padded.initialize(device, context, kWidth, kHeight, 24, 24,
                            chast::Nv12Colorimetry::Bt601Limited);
    if (!err.ok() || !padded.ready()) {
        skip("video processor cannot build a 24x24 NV12 target");
        return;
    }
    check(padded.width() == 24 && padded.height() == 24 &&
              padded.surface_width() == 32 && padded.surface_height() == 32,
          "NV12 pass separates the image size from the aligned surface");
}

void test_cpu_frame_pool() {
    chast::detail::CpuFramePool pool(2);
    const uint8_t first_pixels[] = {1, 2, 3, 4};
    const uint8_t second_pixels[] = {5, 6, 7, 8};
    const uint8_t third_pixels[] = {9, 10, 11, 12};
    const uint8_t fourth_pixels[] = {13, 14, 15, 16};

    auto first = pool.copy(first_pixels, sizeof(first_pixels));
    const uint8_t* first_storage = first->data();
    auto second = pool.copy(second_pixels, sizeof(second_pixels));
    check(second->data() != first_storage &&
              std::memcmp(first->data(), first_pixels, sizeof(first_pixels)) == 0,
          "a retained CPU frame is never overwritten by the next capture");
    check(pool.cached_buffers() == 2 && pool.reusable_buffers() == 0,
          "the CPU frame pool does not recycle retained surfaces");

    first.reset();
    check(pool.reusable_buffers() == 1,
          "a released CPU frame becomes reusable");
    auto third = pool.copy(third_pixels, sizeof(third_pixels));
    check(third->data() == first_storage &&
              std::memcmp(third->data(), third_pixels, sizeof(third_pixels)) == 0,
          "a released CPU frame reuses its allocation");

    auto fourth = pool.copy(fourth_pixels, sizeof(fourth_pixels));
    check(fourth->data() != second->data() && fourth->data() != third->data() &&
              pool.cached_buffers() == 2,
          "a bounded pool falls back to a transient frame when all slots are retained");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_cpu_rotation();
    test_cpu_frame_pool();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!create_device(&device, &context)) {
        skip("no D3D11 hardware device");
        return 0;
    }
    test_hdr(device.get(), context.get());
    test_rotation(device.get(), context.get());
    test_nv12_scale(device.get(), context.get());
    std::printf("%s\n", failures ? "capture transform checks failed" : "capture transform checks passed");
    return failures ? 1 : 0;
}
