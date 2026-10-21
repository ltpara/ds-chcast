// Minimal duplicate/acquire/release loop, no D3D work in between.
// Used to tell a driver or platform fault apart from a bug in the capturer.
//
//   chast-dup-loop [frames] [--desc] [--cursor] [--raw-shape FILE]
//                  [--staging] [--hold]
//
// Each flag adds one step the capturer performs per frame, so a crash can be
// attributed to a single one. --hold releases a frame at the start of the next
// iteration instead of right after acquiring it, which is the model the
// capturer uses for a pass-through frame.

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "crash_handler.h"

namespace {

template <typename T>
void release(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

struct Options {
    int frames = 20;
    bool desc = false;
    bool cursor = false;
    bool staging = false;
    bool hold = false;
    std::string raw_shape_path;
};

}  // namespace

int main(int argc, char** argv) {
    chast_tools::install_crash_handler();
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--desc") {
            options.desc = true;
        } else if (arg == "--cursor") {
            options.cursor = true;
        } else if (arg == "--staging") {
            options.staging = true;
        } else if (arg == "--hold") {
            options.hold = true;
        } else if (arg == "--raw-shape" && i + 1 < argc) {
            options.raw_shape_path = argv[++i];
        } else {
            options.frames = std::atoi(arg.c_str());
        }
    }

    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* staging = nullptr;
    auto shutdown = [&]() {
        release(staging);
        release(duplication);
        release(output1);
        release(context);
        release(device);
        release(output);
        release(adapter);
        release(factory);
    };

    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        std::fprintf(stderr, "CreateDXGIFactory1 hr=%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }
    hr = factory->EnumAdapters1(0, &adapter);
    if (FAILED(hr) || !adapter) {
        std::fprintf(stderr, "EnumAdapters1 hr=%08lX\n", static_cast<unsigned long>(hr));
        shutdown();
        return 1;
    }
    hr = adapter->EnumOutputs(0, &output);
    if (FAILED(hr) || !output) {
        std::fprintf(stderr, "EnumOutputs hr=%08lX\n", static_cast<unsigned long>(hr));
        shutdown();
        return 1;
    }

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                           &device, &level, &context);
    std::printf("device hr=%08lX level=%d\n", static_cast<unsigned long>(hr), level);
    if (FAILED(hr) || !device || !context) {
        shutdown();
        return 1;
    }

    hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    if (FAILED(hr) || !output1) {
        std::fprintf(stderr, "IDXGIOutput1 hr=%08lX\n", static_cast<unsigned long>(hr));
        shutdown();
        return 1;
    }
    hr = output1->DuplicateOutput(device, &duplication);
    std::printf("duplicate hr=%08lX dup=%p\n", static_cast<unsigned long>(hr),
                static_cast<void*>(duplication));
    if (FAILED(hr) || !duplication) {
        shutdown();
        return 1;
    }

    std::vector<uint8_t> cursor_buffer;

    IDXGIResource* held = nullptr;
    ID3D11Texture2D* held_texture = nullptr;
    for (int i = 0; i < options.frames; ++i) {
        if (options.hold && held) {
            context->Flush();
            HRESULT rhr = duplication->ReleaseFrame();
            std::printf("[%d] released-held hr=%08lX\n", i, static_cast<unsigned long>(rhr));
            release(held_texture);
            release(held);
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        hr = duplication->AcquireNextFrame(100, &info, &resource);
        std::printf("[%d] acquire hr=%08lX accum=%u res=%p\n", i, static_cast<unsigned long>(hr),
                    info.AccumulatedFrames, static_cast<void*>(resource));
        if (FAILED(hr)) {
            continue;
        }

        ID3D11Texture2D* texture = nullptr;
        if (options.desc || options.staging) {
            const HRESULT qhr =
                resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
            if (FAILED(qhr) || !texture) {
                std::fprintf(stderr, "[%d] ID3D11Texture2D hr=%08lX\n", i,
                             static_cast<unsigned long>(qhr));
            } else {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                std::printf("[%d] tex %ux%u fmt=%u bind=%08X misc=%08X usage=%u\n", i, desc.Width,
                            desc.Height, desc.Format, desc.BindFlags, desc.MiscFlags, desc.Usage);
            }
        }

        if (options.cursor) {
            cursor_buffer.resize(64 * 1024);
            DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
            UINT used = 0;
            HRESULT chr = duplication->GetFramePointerShape(
                static_cast<UINT>(cursor_buffer.size()), cursor_buffer.data(), &used, &shape);
            std::printf("[%d] shape hr=%08lX type=%u %ux%u pitch=%u used=%u\n", i,
                        static_cast<unsigned long>(chr), shape.Type, shape.Width, shape.Height,
                        shape.Pitch, used);
            if (!options.raw_shape_path.empty() && used > 0) {
                FILE* file = nullptr;
                if (fopen_s(&file, options.raw_shape_path.c_str(), "wb") == 0 && file) {
                    fwrite(cursor_buffer.data(), 1, used, file);
                    fclose(file);
                    std::printf("[%d] wrote raw shape %u bytes to %s\n", i, used,
                                options.raw_shape_path.c_str());
                    options.raw_shape_path.clear();
                }
            }
        }

        if (options.staging && texture) {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (!staging) {
                D3D11_TEXTURE2D_DESC staging_desc = desc;
                staging_desc.Usage = D3D11_USAGE_STAGING;
                staging_desc.BindFlags = 0;
                staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                staging_desc.MiscFlags = 0;
                HRESULT shr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
                std::printf("[%d] staging hr=%08lX\n", i, static_cast<unsigned long>(shr));
            }
            context->CopyResource(staging, texture);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT mhr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
            std::printf("[%d] map hr=%08lX pitch=%u\n", i, static_cast<unsigned long>(mhr),
                        mapped.RowPitch);
            if (SUCCEEDED(mhr)) {
                context->Unmap(staging, 0);
            }
        }

        if (options.hold) {
            held = resource;
            held_texture = texture;
        } else {
            context->Flush();
            HRESULT rhr = duplication->ReleaseFrame();
            std::printf("[%d] released hr=%08lX\n", i, static_cast<unsigned long>(rhr));
            release(texture);
            release(resource);
        }
    }

    release(held_texture);
    release(held);
    shutdown();
    std::printf("done\n");
    return 0;
}
