#include "chast/png.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include "chast/com_ptr.h"
#include "chast/error.h"

namespace chast {

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        size);
    return out;
}

std::string fail(const char* what, HRESULT hr) {
    // Keep this library independent of com.cc: clipboard-only binaries should
    // not pull in the DXGI helper library just to format an HRESULT.
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(hr));
    return std::string(what) + " failed: " + code;
}

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

// The session thread may be STA (the runner initialises one) or MTA (the web
// authentication path switches it); either way WIC works, so a changed mode is
// not a failure. Only a real initialisation error is.
class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned_ = SUCCEEDED(hr);
        error_ = hr;
    }
    ~ComScope() {
        if (owned_) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool ok() const { return owned_ || error_ == RPC_E_CHANGED_MODE; }
    HRESULT error() const { return error_; }

private:
    bool owned_ = false;
    HRESULT error_ = E_FAIL;
};

bool valid_image(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride) {
    return pixels != nullptr && width != 0 && height != 0 &&
           stride >= static_cast<uint64_t>(width) * 4;
}

bool write_frame(IWICImagingFactory* factory, IWICBitmapEncoder* encoder,
                 const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride,
                 std::string* error) {
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> frame_properties;
    HRESULT hr = encoder->CreateNewFrame(frame.put(), frame_properties.put());
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(frame_properties.get());
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(width, height);
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) {
        hr = frame->SetPixelFormat(&format);
    }
    if (FAILED(hr)) {
        set_error(error, fail("PNG frame setup", hr));
        return false;
    }

    // A desktop picture has no meaningful transparency: a driver can leave the
    // alpha channel at zero, which would encode as a fully transparent PNG.
    std::vector<uint8_t> copy(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = pixels + static_cast<size_t>(y) * stride;
        uint8_t* destination = copy.data() + static_cast<size_t>(y) * width * 4;
        std::memcpy(destination, source, static_cast<size_t>(width) * 4);
        for (uint32_t x = 0; x < width; ++x) {
            destination[x * 4 + 3] = 0xFF;
        }
    }

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(
        width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
        static_cast<UINT>(copy.size()), copy.data(), bitmap.put());
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateFormatConverter(converter.put());
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(bitmap.get(), format, WICBitmapDitherTypeNone, nullptr,
                                   0.0, WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WriteSource(converter.get(), nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (FAILED(hr)) {
        set_error(error, fail("writing the PNG", hr));
        return false;
    }
    return true;
}

bool create_encoder(IWICImagingFactory* factory, IWICStream* stream,
                    ComPtr<IWICBitmapEncoder>& encoder, std::string* error) {
    HRESULT hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (FAILED(hr)) {
        set_error(error, fail("PNG encoder setup", hr));
        return false;
    }
    return true;
}

bool create_directories(const std::string& path, std::string* error) {
    std::error_code ec;
    const std::filesystem::path target(
        std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            set_error(error, "cannot create the screenshot directory: " + ec.message());
            return false;
        }
    }
    return true;
}

}  // namespace

bool write_png_bgra(const std::string& path, const uint8_t* pixels, uint32_t width,
                    uint32_t height, uint32_t stride, std::string* error) {
    if (path.empty() || !valid_image(pixels, width, height, stride)) {
        set_error(error, "invalid BGRA image");
        return false;
    }
    if (!create_directories(path, error)) {
        return false;
    }
    const std::wstring wide_path = widen(path);
    if (wide_path.empty()) {
        set_error(error, "the file name is not valid UTF-8");
        return false;
    }

    ComScope com;
    if (!com.ok()) {
        set_error(error, fail("COM initialisation", com.error()));
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()));
    if (FAILED(hr) || !factory) {
        set_error(error, fail("WIC factory creation", hr));
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.put());
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromFilename(wide_path.c_str(), GENERIC_WRITE);
    }
    if (FAILED(hr)) {
        set_error(error, fail("opening the PNG file", hr));
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (!create_encoder(factory.get(), stream.get(), encoder, error)) {
        return false;
    }
    if (!write_frame(factory.get(), encoder.get(), pixels, width, height, stride, error)) {
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        set_error(error, fail("writing the PNG", hr));
        return false;
    }
    return true;
}

bool encode_png_bgra(const uint8_t* pixels, uint32_t width, uint32_t height,
                     uint32_t stride, std::vector<uint8_t>& png, std::string* error) {
    png.clear();
    if (!valid_image(pixels, width, height, stride)) {
        set_error(error, "invalid BGRA image");
        return false;
    }

    ComScope com;
    if (!com.ok()) {
        set_error(error, fail("COM initialisation", com.error()));
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()));
    if (FAILED(hr) || !factory) {
        set_error(error, fail("WIC factory creation", hr));
        return false;
    }

    // WIC writes through this stream; the HGLOBAL is freed with it.
    ComPtr<IStream> memory;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, memory.put());
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateStream(stream.put());
    }
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromIStream(memory.get());
    }
    if (FAILED(hr)) {
        set_error(error, fail("opening the PNG memory stream", hr));
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    if (!create_encoder(factory.get(), stream.get(), encoder, error)) {
        return false;
    }
    if (!write_frame(factory.get(), encoder.get(), pixels, width, height, stride, error)) {
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        set_error(error, fail("writing the PNG", hr));
        return false;
    }

    LARGE_INTEGER begin{};
    hr = memory->Seek(begin, STREAM_SEEK_SET, nullptr);
    STATSTG stat{};
    if (SUCCEEDED(hr)) {
        hr = memory->Stat(&stat, STATFLAG_NONAME);
    }
    if (FAILED(hr) || stat.cbSize.QuadPart == 0) {
        set_error(error, fail("reading the PNG size", hr));
        return false;
    }
    png.resize(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    hr = memory->Read(png.data(), static_cast<ULONG>(png.size()), &read);
    if (FAILED(hr) || read != png.size()) {
        png.clear();
        set_error(error, fail("reading the PNG bytes", hr));
        return false;
    }
    return true;
}

bool encode_dib_png(const std::vector<uint8_t>& dib, std::vector<uint8_t>& png,
                    std::string* error) {
    png.clear();
    constexpr size_t kBitmapFileHeaderSize = 14;
    if (dib.size() < 12 || dib.size() > 160u * 1024u * 1024u ||
        dib.size() > UINT_MAX - kBitmapFileHeaderSize) {
        set_error(error, "invalid DIB memory buffer");
        return false;
    }
    const auto read_u32 = [&dib](size_t offset) {
        return static_cast<uint32_t>(dib[offset]) |
               (static_cast<uint32_t>(dib[offset + 1]) << 8) |
               (static_cast<uint32_t>(dib[offset + 2]) << 16) |
               (static_cast<uint32_t>(dib[offset + 3]) << 24);
    };
    const uint32_t header_size = read_u32(0);
    uint32_t width = 0;
    uint32_t height = 0;
    bool top_down = false;
    uint32_t bit_count = 0;
    uint32_t compression = 0;
    uint32_t colors_used = 0;
    uint32_t palette_entry_bytes = 4;
    if (header_size == 12) {
        width = static_cast<uint32_t>(dib[4]) |
                (static_cast<uint32_t>(dib[5]) << 8);
        height = static_cast<uint32_t>(dib[6]) |
                 (static_cast<uint32_t>(dib[7]) << 8);
        bit_count = static_cast<uint32_t>(dib[10]) |
                    (static_cast<uint32_t>(dib[11]) << 8);
        palette_entry_bytes = 3;
    } else if (header_size == 40 || header_size == 52 || header_size == 56 ||
               header_size == 64 || header_size == 108 || header_size == 124) {
        if (dib.size() < 40) {
            set_error(error, "DIB memory buffer is shorter than its header");
            return false;
        }
        width = read_u32(4);
        const int32_t signed_height = static_cast<int32_t>(read_u32(8));
        if (read_u32(8) == 0x80000000u) {
            set_error(error, "DIB memory buffer has an invalid height");
            return false;
        }
        top_down = signed_height < 0;
        height = static_cast<uint32_t>(top_down ? -signed_height : signed_height);
        bit_count = static_cast<uint32_t>(dib[14]) |
                    (static_cast<uint32_t>(dib[15]) << 8);
        compression = read_u32(16);
        if (header_size >= 40) {
            colors_used = read_u32(32);
        }
    } else {
        set_error(error, "DIB memory buffer has an unknown header");
        return false;
    }
    if (dib.size() < header_size) {
        set_error(error, "DIB memory buffer is shorter than its header");
        return false;
    }
    constexpr uint64_t kMaxDibDimension = 8192;
    constexpr uint64_t kMaxDecodedPixels = 64ull * 1024ull * 1024ull;
    const uint64_t pixel_count = static_cast<uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > kMaxDibDimension ||
        height > kMaxDibDimension || pixel_count == 0 ||
        pixel_count > kMaxDecodedPixels) {
        set_error(error, "DIB memory buffer has an unusable size");
        return false;
    }
    uint64_t mask_bytes = 0;
    if (compression == 3 && (header_size == 40 || header_size == 64)) {
        mask_bytes = 12;
    }
    uint64_t palette_bytes = 0;
    if (compression <= 3) {
        uint64_t entries = colors_used;
        if (entries == 0 && bit_count > 0 && bit_count <= 8) {
            entries = uint64_t{1} << bit_count;
        }
        if (entries > 256) {
            set_error(error, "DIB memory buffer has an invalid color table");
            return false;
        }
        palette_bytes = entries * palette_entry_bytes;
    }
    const uint64_t dib_pixel_offset = header_size + mask_bytes + palette_bytes;
    const uint64_t pixel_offset = kBitmapFileHeaderSize + dib_pixel_offset;
    if (dib_pixel_offset > dib.size()) {
        set_error(error, "DIB memory buffer is shorter than its color table");
        return false;
    }

    std::vector<uint8_t> pixels;
    const bool direct_bgr = bit_count == 24 || bit_count == 32;
    if (compression == 0 && direct_bgr) {
        const uint64_t stride = ((static_cast<uint64_t>(width) * bit_count + 31) / 32) * 4;
        if (stride == 0 || stride > dib.size() ||
            height > (dib.size() - dib_pixel_offset) / stride) {
            set_error(error, "DIB memory buffer is shorter than its pixel data");
            return false;
        }
        pixels.resize(static_cast<size_t>(pixel_count) * 4);
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t source_y = top_down ? y : height - 1 - y;
            const uint8_t* source = dib.data() + dib_pixel_offset + stride * source_y;
            uint8_t* destination = pixels.data() + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t* output = destination + static_cast<size_t>(x) * 4;
                const size_t bytes = bit_count / 8;
                std::memcpy(output, source + static_cast<size_t>(x) * bytes, bytes);
                output[3] = 0xFF;
            }
        }
    } else {
        // WIC is the general BMP decoder, but its current Windows build rejects
        // large top-down DIBs with WINCODEC_ERR_BADIMAGE. Common clipboard DIBs
        // are uncompressed BGRA/BGR, so decode those directly above and retain
        // WIC for bitfields, RLE and embedded JPEG/PNG payloads.
        std::vector<uint8_t> bmp(kBitmapFileHeaderSize + dib.size(), 0);
        bmp[0] = 'B';
        bmp[1] = 'M';
        const uint32_t file_size = static_cast<uint32_t>(bmp.size());
        const uint32_t offset = static_cast<uint32_t>(pixel_offset);
        for (int i = 0; i < 4; ++i) {
            bmp[2 + i] = static_cast<uint8_t>(file_size >> (8 * i));
            bmp[10 + i] = static_cast<uint8_t>(offset >> (8 * i));
        }
        std::memcpy(bmp.data() + kBitmapFileHeaderSize, dib.data(), dib.size());

        if (!decode_png_bgra(bmp.data(), bmp.size(), width, height, pixels, error)) {
            // decode_png_bgra() names the exact WIC stage and HRESULT. Preserve it
            // so clipboard diagnostics can distinguish stream, header and pixel
            // failures instead of collapsing them into a generic message.
            if (error != nullptr && error->empty()) {
                *error = "the DIB could not be decoded";
            }
            return false;
        }
    }
    if (!encode_png_bgra(pixels.data(), width, height, width * 4, png, error)) {
        png.clear();
        return false;
    }
    return true;
}

bool decode_png_bgra(const uint8_t* data, size_t size, uint32_t& width,
                     uint32_t& height, std::vector<uint8_t>& pixels,
                     std::string* error) {
    width = 0;
    height = 0;
    pixels.clear();
    if (data == nullptr || size == 0 || size > static_cast<size_t>(ULONG_MAX)) {
        set_error(error, "invalid PNG memory buffer");
        return false;
    }

    ComScope com;
    if (!com.ok()) {
        set_error(error, fail("COM initialisation", com.error()));
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()));
    if (FAILED(hr) || !factory) {
        set_error(error, fail("WIC factory creation", hr));
        return false;
    }

    ComPtr<IStream> memory;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, memory.put());
    ULONG written = 0;
    if (SUCCEEDED(hr)) {
        hr = memory->Write(data, static_cast<ULONG>(size), &written);
    }
    if (SUCCEEDED(hr) && static_cast<size_t>(written) != size) {
        hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    LARGE_INTEGER begin{};
    if (SUCCEEDED(hr)) {
        hr = memory->Seek(begin, STREAM_SEEK_SET, nullptr);
    }
    ComPtr<IWICStream> stream;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateStream(stream.put());
    }
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromIStream(memory.get());
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(stream.get(), nullptr,
                                              WICDecodeMetadataCacheOnLoad,
                                              decoder.put());
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, frame.put());
    }
    if (SUCCEEDED(hr)) {
        hr = frame->GetSize(&width, &height);
    }
    constexpr uint64_t kMaxDecodedPixels = 64ull * 1024ull * 1024ull;
    constexpr uint64_t kMaxDecodedBytes = static_cast<uint64_t>(UINT_MAX);
    const uint64_t count = static_cast<uint64_t>(width) * height;
    if (SUCCEEDED(hr) && (count == 0 || count > kMaxDecodedPixels ||
                          count * 4 > kMaxDecodedBytes ||
                          static_cast<uint64_t>(width) * 4 > UINT_MAX)) {
        hr = WINCODEC_ERR_BADIMAGE;
    }
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateFormatConverter(converter.put());
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        width = 0;
        height = 0;
        set_error(error, fail("reading the PNG", hr));
        return false;
    }
    pixels.resize(static_cast<size_t>(count) * 4);
    hr = converter->CopyPixels(nullptr, width * 4,
                               static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) {
        width = 0;
        height = 0;
        pixels.clear();
        set_error(error, fail("reading the PNG pixels", hr));
        return false;
    }
    return true;
}

bool write_png_file(const std::string& path, const uint8_t* data, size_t size,
                    std::string* error) {
    if (path.empty() || data == nullptr || size == 0) {
        set_error(error, "invalid PNG file");
        return false;
    }
    if (!create_directories(path, error)) {
        return false;
    }
    std::ofstream file(
        std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()),
                                            path.size())),
        std::ios::binary | std::ios::trunc);
    if (!file) {
        set_error(error, "cannot create the screenshot file");
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!file) {
        set_error(error, "cannot write the screenshot file");
        return false;
    }
    return true;
}

}  // namespace chast
