// Capture harness: measures the zero-copy path and, on demand, writes a frame
// and the cursor sprite to disk.
//
// The pixel dumps are deliberately the *only* place in chast that reads desktop
// pixels back: they stage the shared or NV12 texture into a CPU-readable copy
// to prove the pipeline produced the right image. The capture path itself never
// does.
//
//   chast-capture [--display N] [--seconds S] [--dump frame.bmp] [--nv12 out.yuv]
//                 [--colorimetry bt601-limited|bt601-full|bt709-limited|bt709-full]
//                 [--cursor cur.bmp] [--no-hdr] [--no-cursor] [--quiet]
//
// The NV12 dump is a planar YUV4:2:0 file (full Y plane, then interleaved UV),
// which ffmpeg reads as rawvideo yuv420p when told the matrix and range:
//   ffmpeg -f rawvideo -pix_fmt yuv420p -s WxH -i out.yuv -vf scale=out_color_matrix=bt601 \
//          out.png

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "chast/capture.h"
#include "chast/com_ptr.h"
#include "crash_handler.h"

namespace {

struct Options {
    size_t display = 0;
    double seconds = 0.0;  // 0 = capture until interrupted
    std::string frame_path;
    std::string nv12_path;
    std::string cursor_path;
    chast::CaptureConfig config;
    bool quiet = false;
};

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--display" && i + 1 < argc) {
            options.display = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--seconds" && i + 1 < argc) {
            options.seconds = std::stod(argv[++i]);
        } else if (arg == "--dump" && i + 1 < argc) {
            options.frame_path = argv[++i];
        } else if (arg == "--nv12" && i + 1 < argc) {
            options.nv12_path = argv[++i];
            options.config.nv12.enabled = true;
        } else if (arg == "--colorimetry" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "bt601-limited") {
                options.config.nv12.colorimetry = chast::Nv12Colorimetry::Bt601Limited;
            } else if (value == "bt601-full") {
                options.config.nv12.colorimetry = chast::Nv12Colorimetry::Bt601Full;
            } else if (value == "bt709-limited") {
                options.config.nv12.colorimetry = chast::Nv12Colorimetry::Bt709Limited;
            } else if (value == "bt709-full") {
                options.config.nv12.colorimetry = chast::Nv12Colorimetry::Bt709Full;
            } else {
                std::fprintf(stderr, "unknown colorimetry: %s\n", value.c_str());
                return false;
            }
        } else if (arg == "--cursor" && i + 1 < argc) {
            options.cursor_path = argv[++i];
        } else if (arg == "--no-hdr") {
            options.config.want_hdr = false;
        } else if (arg == "--no-cursor") {
            options.config.capture_cursor = false;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

const char* colorimetry_name(chast::Nv12Colorimetry colorimetry) {
    switch (colorimetry) {
        case chast::Nv12Colorimetry::Bt601Full: return "BT.601 full";
        case chast::Nv12Colorimetry::Bt709Limited: return "BT.709 limited";
        case chast::Nv12Colorimetry::Bt709Full: return "BT.709 full";
        case chast::Nv12Colorimetry::Bt601Limited:
        default: return "BT.601 limited";
    }
}

// Copies `texture` into a CPU-readable staging texture and leaves it mapped
// for the caller; `staging` owns the resource until it is unmapped and dropped.
bool stage_readback(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                    chast::ComPtr<ID3D11Texture2D>& staging, D3D11_MAPPED_SUBRESOURCE& mapped,
                    D3D11_TEXTURE2D_DESC& desc, std::string& error) {
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, staging.put()))) {
        error = "staging texture creation failed";
        return false;
    }
    context->CopyResource(staging.get(), texture);
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "staging map failed";
        return false;
    }
    return true;
}

// Stages `texture` through a CPU-readable copy and writes it as BGRA BMP.
// This is verification-only code and is not part of the capture path.
bool save_bmp(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
              const std::string& path, std::string& error) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        error = "only BGRA8 frames can be dumped";
        return false;
    }

    chast::ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!stage_readback(device, context, texture, staging, mapped, desc, error)) {
        return false;
    }

    const UINT row_bytes = desc.Width * 4;
    const UINT image_bytes = row_bytes * desc.Height;
    const UINT header_bytes = 14 + 40;
    std::vector<uint8_t> out(header_bytes + image_bytes);

    // Bottom-up BGRA, as BMP expects.
    uint8_t* pixels = out.data() + header_bytes;
    for (UINT y = 0; y < desc.Height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) +
                             static_cast<size_t>(desc.Height - 1 - y) * mapped.RowPitch;
        std::memcpy(pixels + static_cast<size_t>(y) * row_bytes, src, row_bytes);
    }
    context->Unmap(staging.get(), 0);

    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = header_bytes;
    file_header.bfSize = header_bytes + image_bytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(desc.Width);
    info.biHeight = static_cast<LONG>(desc.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = image_bytes;
    std::memcpy(out.data(), &file_header, sizeof(file_header));
    std::memcpy(out.data() + sizeof(file_header), &info, sizeof(info));

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || !file) {
        error = "could not open " + path;
        return false;
    }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
    return true;
}

// Writes the NV12 texture as a planar yuv420p file: the whole Y plane with its
// source row pitch dropped, then the interleaved UV rows the same way. NV12
// stores chroma as one plane of interleaved U/V pairs at half resolution, which
// is exactly yuv420p's UV ordering for each pair of rows.
//
// The texture is 16-aligned (see the NV12 docs in capture.h), so only the
// top-left width x height pixels are written: the padding is not image.
bool save_nv12(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
               UINT width, UINT height, UINT surface_width, UINT surface_height,
               const std::string& path, std::string& error) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) {
        error = "only NV12 textures can be dumped as NV12";
        return false;
    }
    if (width == 0 || height == 0 || width > surface_width || height > surface_height) {
        error = "NV12 dump size does not fit the surface";
        return false;
    }

    chast::ComPtr<ID3D11Texture2D> staging;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!stage_readback(device, context, texture, staging, mapped, desc, error)) {
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || !file) {
        context->Unmap(staging.get(), 0);
        error = "could not open " + path;
        return false;
    }
    const uint8_t* base = static_cast<const uint8_t*>(mapped.pData);
    // Y plane: one byte per pixel, `RowPitch` bytes per row; only the image
    // columns are kept.
    for (UINT y = 0; y < height; ++y) {
        fwrite(base + static_cast<size_t>(y) * mapped.RowPitch, 1, width, file);
    }
    // Chroma plane starts after the full-size (aligned) Y plane in the mapped
    // resource, a half-resolution row per two image rows.
    const uint8_t* uv = base + static_cast<size_t>(surface_height) * mapped.RowPitch;
    for (UINT y = 0; y < height / 2; ++y) {
        fwrite(uv + static_cast<size_t>(y) * mapped.RowPitch, 1, width, file);
    }
    fclose(file);
    context->Unmap(staging.get(), 0);
    return true;
}

bool save_cursor_bmp(const chast::CursorInfo& cursor, const std::string& path, std::string& error) {
    if (!cursor.shape_valid || cursor.bitmap.empty() || cursor.width == 0 || cursor.height == 0) {
        error = "no cursor shape captured";
        return false;
    }
    // The sprite is premultiplied BGRA; BMP wants straight alpha, so undo the
    // premultiply for the dump (the API itself stays premultiplied).
    const UINT row_bytes = cursor.width * 4;
    const UINT header_bytes = 14 + 40;
    std::vector<uint8_t> out(header_bytes + row_bytes * cursor.height);
    uint8_t* pixels = out.data() + header_bytes;
    for (UINT y = 0; y < cursor.height; ++y) {
        const uint8_t* src = cursor.bitmap.data() + static_cast<size_t>(y) * row_bytes;
        uint8_t* dst = pixels + static_cast<size_t>(cursor.height - 1 - y) * row_bytes;
        for (UINT x = 0; x < cursor.width; ++x) {
            const uint8_t alpha = src[x * 4u + 3u];
            for (UINT c = 0; c < 3; ++c) {
                dst[x * 4u + c] = alpha == 0
                                      ? 0
                                      : static_cast<uint8_t>(
                                            std::min<UINT>(255u, src[x * 4u + c] * 255u / alpha));
            }
            dst[x * 4u + 3u] = alpha;
        }
    }
    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = header_bytes;
    file_header.bfSize = header_bytes + row_bytes * cursor.height;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(cursor.width);
    info.biHeight = static_cast<LONG>(cursor.height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::memcpy(out.data(), &file_header, sizeof(file_header));
    std::memcpy(out.data() + sizeof(file_header), &info, sizeof(info));

    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || !file) {
        error = "could not open " + path;
        return false;
    }
    fwrite(out.data(), 1, out.size(), file);
    fclose(file);
    return true;
}

const char* type_name(DXGI_OUTDUPL_POINTER_SHAPE_TYPE type) {
    switch (type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: return "monochrome";
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: return "color";
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: return "masked-color";
        default: return "invalid";
    }
}

}  // namespace

int main(int argc, char** argv) {
    chast_tools::install_crash_handler();
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }

    std::string error;
    std::unique_ptr<chast::IDxgiCapturer> capturer =
        chast::create_capturer(options.display, options.config, &error);
    if (!capturer) {
        std::fprintf(stderr, "capturer creation failed: %s\n", error.c_str());
        return 1;
    }

    const chast::DisplayInfo& display = capturer->display();
    if (!options.quiet) {
        std::printf("capturing %ls: %ux%u, hdr=%d, advanced-sdr=%d, sdr-white=%u, rotation=%u\n",
                    display.device_name.c_str(), display.width, display.height, display.hdr,
                    display.advanced_color_sdr, display.sdr_white_level,
                    display.rotation_degrees);
        const chast::AdapterDevice adapter = capturer->adapter_device();
        std::printf("device: %p, vendor %04X, LUID %08lX:%08lX\n", static_cast<void*>(adapter.device),
                    adapter.vendor_id, static_cast<unsigned long>(adapter.luid.HighPart),
                    static_cast<unsigned long>(adapter.luid.LowPart));
        if (options.config.nv12.enabled) {
            std::printf("nv12: requested, colorimetry %s\n",
                        colorimetry_name(options.config.nv12.colorimetry));
        }
        if (!capturer->last_error().ok()) {
            std::printf("note: %s\n", capturer->last_error().to_string().c_str());
        }
    }

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto start = std::chrono::steady_clock::now();
    uint64_t frames = 0;
    uint64_t pointer_only = 0;
    uint64_t timeouts = 0;
    uint64_t reinit_needed = 0;
    bool dumped = false;
    bool dumped_nv12 = false;
    bool dumped_cursor = false;

    while (true) {
        if (options.seconds > 0.0) {
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (elapsed >= options.seconds) {
                break;
            }
        }

        const chast::CaptureResult result = capturer->capture(std::chrono::milliseconds(100));
        switch (result) {
            case chast::CaptureResult::NoNewFrame:
                ++timeouts;
                continue;
            case chast::CaptureResult::ReinitNeeded: {
                ++reinit_needed;
                if (!options.quiet) {
                    std::printf("reinitializing: %s\n", capturer->last_error().to_string().c_str());
                }
                chast::Error err = capturer->reinitialize();
                if (!err.ok()) {
                    std::fprintf(stderr, "reinit failed: %s\n", err.to_string().c_str());
                    return 1;
                }
                continue;
            }
            case chast::CaptureResult::FatalError:
                std::fprintf(stderr, "capture failed: %s\n",
                             capturer->last_error().to_string().c_str());
                return 1;
            case chast::CaptureResult::Frame:
                break;
        }

        ++frames;
        const chast::FrameInfo& frame = capturer->frame();
        if (!frame.desktop_updated) {
            ++pointer_only;
        }

        if (!options.frame_path.empty() && !dumped && frame.desktop_updated) {
            std::string dump_error;
            if (save_bmp(frame.device, frame.context, frame.texture, options.frame_path,
                         dump_error)) {
                dumped = true;
                if (!options.quiet) {
                    std::printf("wrote %s (%ux%u, %s)\n", options.frame_path.c_str(), frame.width,
                                frame.height, frame.format == DXGI_FORMAT_B8G8R8A8_UNORM
                                                   ? "BGRA8"
                                                   : "other");
                }
            } else {
                std::fprintf(stderr, "frame dump failed: %s\n", dump_error.c_str());
                options.frame_path.clear();
            }
        }

        if (!options.nv12_path.empty() && !dumped_nv12 && frame.nv12_updated &&
            frame.nv12_texture != nullptr) {
            std::string dump_error;
            if (save_nv12(frame.device, frame.context, frame.nv12_texture, frame.width,
                          frame.height, frame.nv12_surface_width, frame.nv12_surface_height,
                          options.nv12_path, dump_error)) {
                dumped_nv12 = true;
                if (!options.quiet) {
                    std::printf("wrote %s (%ux%u, NV12, %s)\n", options.nv12_path.c_str(),
                                frame.width, frame.height,
                                colorimetry_name(options.config.nv12.colorimetry));
                }
            } else {
                std::fprintf(stderr, "nv12 dump failed: %s\n", dump_error.c_str());
                options.nv12_path.clear();
            }
        }

        if (!options.cursor_path.empty() && !dumped_cursor && capturer->cursor().shape_valid) {
            const chast::CursorInfo& cursor = capturer->cursor();
            std::string dump_error;
            if (save_cursor_bmp(cursor, options.cursor_path, dump_error)) {
                dumped_cursor = true;
                if (!options.quiet) {
                    std::printf("wrote %s (%ux%u, %s, content id %llu, hotspot %ld,%ld, origin %ld,%ld%s)\n",
                                options.cursor_path.c_str(), cursor.width, cursor.height,
                                type_name(cursor.shape_type),
                                static_cast<unsigned long long>(cursor.content_id),
                                cursor.hotspot.x, cursor.hotspot.y, cursor.origin_offset.x,
                                cursor.origin_offset.y, cursor.outlined ? ", outlined" : "");
                }
            }
        }

        const bool wanted_dumps = !options.frame_path.empty() || !options.nv12_path.empty() ||
                                  !options.cursor_path.empty();
        const bool all_dumped = (options.frame_path.empty() || dumped) &&
                                (options.nv12_path.empty() || dumped_nv12) &&
                                (options.cursor_path.empty() || dumped_cursor);
        if (options.seconds <= 0.0 && wanted_dumps && all_dumped) {
            break;  // one-shot dumps are a verification mode, not a stream
        }
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint64_t events = frames + timeouts;
    std::printf("frames=%llu pointer-only=%llu timeouts=%llu reinits=%llu fps=%.1f\n",
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(pointer_only),
                static_cast<unsigned long long>(timeouts),
                static_cast<unsigned long long>(reinit_needed),
                elapsed > 0.0 ? static_cast<double>(events) / elapsed : 0.0);
    return 0;
}
