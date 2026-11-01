#include "chast/png.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
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
    return std::string(what) + " failed: " + hresult_message(hr);
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

}  // namespace

bool write_png_bgra(const std::string& path, const uint8_t* pixels, uint32_t width,
                    uint32_t height, uint32_t stride, std::string* error) {
    if (path.empty() || pixels == nullptr || width == 0 || height == 0 ||
        stride < width * 4) {
        set_error(error, "invalid BGRA image");
        return false;
    }

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
    if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    }
    if (FAILED(hr)) {
        set_error(error, fail("PNG encoder setup", hr));
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> frame_properties;
    hr = encoder->CreateNewFrame(frame.put(), frame_properties.put());
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
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    if (FAILED(hr)) {
        set_error(error, fail("writing the PNG", hr));
        return false;
    }
    return true;
}

}  // namespace chast
