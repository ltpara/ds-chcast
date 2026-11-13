// PNG writer: signature, decoded size and pixels, alpha forcing and argument
// validation. Uses WIC for the decode, so it checks what any PNG reader sees
// instead of trusting the encoder's own bookkeeping.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "chast/com_ptr.h"
#include "chast/png.h"
#include "test_workdir.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

bool has_png_signature(const std::filesystem::path& path) {
    static const unsigned char signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::ifstream input(path, std::ios::binary);
    unsigned char header[8]{};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    return input.gcount() == static_cast<std::streamsize>(sizeof(header)) &&
           std::memcmp(header, signature, sizeof(signature)) == 0;
}

bool decode_png(const std::filesystem::path& path, uint32_t& width, uint32_t& height,
                std::vector<uint8_t>& pixels, std::string& error) {
    chast::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.put()));
    chast::ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad,
                                                decoder.put());
    }
    chast::ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, frame.put());
    }
    if (SUCCEEDED(hr)) {
        hr = frame->GetSize(&width, &height);
    }
    chast::ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateFormatConverter(converter.put());
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        error = "WIC could not decode the PNG";
        return false;
    }
    pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()),
                               pixels.data());
    if (FAILED(hr)) {
        error = "WIC could not read the decoded pixels";
        return false;
    }
    return true;
}

void test_round_trip(const std::filesystem::path& directory) {
    constexpr uint32_t kWidth = 5;
    constexpr uint32_t kHeight = 3;
    std::vector<uint8_t> source(static_cast<size_t>(kWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* pixel = source.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            pixel[0] = static_cast<uint8_t>(10 + x * 20);  // B
            pixel[1] = static_cast<uint8_t>(20 + y * 30);  // G
            pixel[2] = static_cast<uint8_t>(200 - x * 5);  // R
            pixel[3] = 0;                                  // A, must come back opaque
        }
    }

    // A nested folder exercises the writer's own directory creation.
    const std::filesystem::path path = directory / "nested" / "shot.png";
    std::string error;
    check(chast::write_png_bgra(path.string(), source.data(), kWidth, kHeight, kWidth * 4,
                                &error),
          "a BGRA image is written");
    check(error.empty(), "a successful write reports no error");
    check(std::filesystem::exists(path) && std::filesystem::file_size(path) > 0,
          "the PNG file exists and is not empty");
    check(has_png_signature(path), "the file starts with the PNG signature");

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> decoded;
    error.clear();
    const bool read = decode_png(path, width, height, decoded, error);
    check(read, "WIC decodes the PNG back");
    check(width == kWidth && height == kHeight, "the decoded size matches the source");
    if (read && width == kWidth && height == kHeight) {
        bool pixels_match = true;
        bool alpha_opaque = true;
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                const uint8_t* expected =
                    source.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
                const uint8_t* actual =
                    decoded.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
                if (actual[0] != expected[0] || actual[1] != expected[1] ||
                    actual[2] != expected[2]) {
                    pixels_match = false;
                }
                if (actual[3] != 0xFF) {
                    alpha_opaque = false;
                }
            }
        }
        check(pixels_match, "the decoded colour channels match the source");
        check(alpha_opaque, "the alpha channel is forced opaque");
    }
}

void test_memory_encode(const std::filesystem::path& directory) {
    constexpr uint32_t kWidth = 4;
    constexpr uint32_t kHeight = 2;
    std::vector<uint8_t> source(static_cast<size_t>(kWidth) * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* pixel = source.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
            pixel[0] = static_cast<uint8_t>(30 + x * 10);
            pixel[1] = static_cast<uint8_t>(60 + y * 20);
            pixel[2] = static_cast<uint8_t>(90 + x * 5);
            pixel[3] = 0;
        }
    }

    std::vector<uint8_t> png;
    std::string error;
    check(chast::encode_png_bgra(source.data(), kWidth, kHeight, kWidth * 4, png, &error) &&
              error.empty() && png.size() > 8 &&
              std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0,
          "a BGRA image is encoded to PNG bytes in memory");

    const std::filesystem::path path = directory / "memory" / "shot.png";
    check(chast::write_png_file(path.string(), png.data(), png.size(), &error) &&
              error.empty() && std::filesystem::file_size(path) == png.size(),
          "the encoded bytes are written to a file");

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> decoded;
    check(decode_png(path, width, height, decoded, error) && width == kWidth &&
              height == kHeight && decoded.size() == source.size() &&
              decoded[0] == source[0] && decoded[1] == source[1] &&
              decoded[2] == source[2] && decoded[3] == 0xFF,
          "the in-memory PNG decodes to the encoded pixels");

    uint32_t memory_width = 0;
    uint32_t memory_height = 0;
    std::vector<uint8_t> memory_pixels;
    error.clear();
    check(chast::decode_png_bgra(png.data(), png.size(), memory_width, memory_height,
                                 memory_pixels, &error) &&
              memory_width == kWidth && memory_height == kHeight &&
              memory_pixels.size() == source.size() &&
              memory_pixels[0] == source[0] && memory_pixels[1] == source[1] &&
              memory_pixels[2] == source[2] && memory_pixels[3] == 0xFF,
          "PNG bytes decode directly from memory");
    const std::vector<uint8_t> expected_png_pixels = memory_pixels;
    check(!chast::decode_png_bgra(nullptr, 0, memory_width, memory_height,
                                  memory_pixels, &error) &&
              memory_width == 0 && memory_height == 0 && memory_pixels.empty(),
          "empty PNG memory is refused");
    std::vector<uint8_t> bad_png = png;
    bad_png[1] ^= 0xFF;
    check(!chast::decode_png_bgra(bad_png.data(), bad_png.size(), memory_width,
                                  memory_height, memory_pixels, &error),
          "PNG memory with a bad signature is refused");

    // CF_DIB has no BITMAPFILEHEADER. The conversion helper builds that header
    // in memory and WIC decodes the resulting BMP without touching the disk.
    std::vector<uint8_t> dib(40 + static_cast<size_t>(kWidth) * kHeight * 4, 0);
    auto put32 = [&dib](size_t offset, uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            dib[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    put32(0, 40);
    put32(4, kWidth);
    // A negative DIB height marks top-down rows, matching the BGRA fixture.
    put32(8, static_cast<uint32_t>(-static_cast<int32_t>(kHeight)));
    dib[12] = 1;
    dib[14] = 32;
    put32(20, static_cast<uint32_t>(kWidth) * kHeight * 4);
    std::memcpy(dib.data() + 40, source.data(), source.size());
    std::vector<uint8_t> dib_png;
    check(chast::encode_dib_png(dib, dib_png, &error) && !dib_png.empty(),
          "a CF_DIB image converts to PNG in memory");
    std::vector<uint8_t> dib_pixels;
    check(chast::decode_png_bgra(dib_png.data(), dib_png.size(), memory_width,
                                 memory_height, dib_pixels, &error) &&
              memory_width == kWidth && memory_height == kHeight &&
              dib_pixels == expected_png_pixels,
          "the converted DIB PNG has the same pixels");

    // Clipboard screenshots commonly exceed the 32 MiB network DIB limit before
    // being represented as PNG. Exercise the same 4096x2048 top-down BI_RGB
    // shape as the native clipboard probe so this path is covered without a
    // window station.
    constexpr uint32_t kLargeWidth = 4096;
    constexpr uint32_t kLargeHeight = 2048;
    const size_t large_pixels = static_cast<size_t>(kLargeWidth) * kLargeHeight * 4;
    std::vector<uint8_t> large_dib(40 + large_pixels, 0);
    std::fill(large_dib.begin() + 40, large_dib.end(), static_cast<uint8_t>(0x80));
    auto put_large32 = [&large_dib](size_t offset, uint32_t value) {
        for (int i = 0; i != 4; ++i) {
            large_dib[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    put_large32(0, 40);
    put_large32(4, kLargeWidth);
    put_large32(8, static_cast<uint32_t>(-static_cast<int32_t>(kLargeHeight)));
    large_dib[12] = 1;
    large_dib[14] = 32;
    put_large32(20, static_cast<uint32_t>(large_pixels));
    std::vector<uint8_t> large_png;
    error.clear();
    const bool large_converted = chast::encode_dib_png(large_dib, large_png, &error);
    check(large_converted, "a 4096x2048 top-down DIB converts to PNG");
    if (!large_converted) {
        std::printf("     conversion error: %s\n", error.c_str());
    }
    std::vector<uint8_t> large_decoded;
    check(chast::decode_png_bgra(large_png.data(), large_png.size(), memory_width,
                                 memory_height, large_decoded, &error) &&
              memory_width == kLargeWidth && memory_height == kLargeHeight &&
              large_decoded.size() == large_pixels,
          "the large DIB PNG decodes to the expected dimensions");

    constexpr uint32_t kBottomWidth = 3;
    constexpr uint32_t kBottomHeight = 2;
    constexpr size_t kBottomStride = 12;
    std::vector<uint8_t> bottom_up_dib(40 + kBottomStride * kBottomHeight, 0);
    auto put_bottom32 = [&bottom_up_dib](size_t offset, uint32_t value) {
        for (int i = 0; i != 4; ++i) {
            bottom_up_dib[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    };
    put_bottom32(0, 40);
    put_bottom32(4, kBottomWidth);
    put_bottom32(8, kBottomHeight);
    bottom_up_dib[12] = 1;
    bottom_up_dib[14] = 24;
    put_bottom32(20, static_cast<uint32_t>(kBottomStride * kBottomHeight));
    std::vector<uint8_t> bottom_pixels(kBottomStride * kBottomHeight);
    for (uint32_t y = 0; y < kBottomHeight; ++y) {
        for (uint32_t x = 0; x < kBottomWidth; ++x) {
            uint8_t* pixel = bottom_pixels.data() + y * kBottomStride + x * 3;
            pixel[0] = static_cast<uint8_t>(10 + y * 20 + x);
            pixel[1] = static_cast<uint8_t>(40 + y * 20 + x);
            pixel[2] = static_cast<uint8_t>(70 + y * 20 + x);
        }
    }
    std::memcpy(bottom_up_dib.data() + 40, bottom_pixels.data(), bottom_pixels.size());
    std::vector<uint8_t> bottom_png;
    check(chast::encode_dib_png(bottom_up_dib, bottom_png, &error),
          "a bottom-up 24-bit DIB converts to PNG");
    std::vector<uint8_t> bottom_decoded;
    bool bottom_pixels_match = false;
    if (chast::decode_png_bgra(bottom_png.data(), bottom_png.size(), memory_width,
                               memory_height, bottom_decoded, &error) &&
        memory_width == kBottomWidth && memory_height == kBottomHeight &&
        bottom_decoded.size() == kBottomWidth * kBottomHeight * 4) {
        bottom_pixels_match = true;
        for (uint32_t x = 0; x < kBottomWidth; ++x) {
            const uint8_t* expected = bottom_pixels.data() + kBottomStride + x * 3;
            const uint8_t* actual = bottom_decoded.data() + x * 4;
            bottom_pixels_match = bottom_pixels_match && actual[0] == expected[0] &&
                                  actual[1] == expected[1] && actual[2] == expected[2] &&
                                  actual[3] == 0xFF;
        }
    }
    const bool bottom_ok = bottom_pixels_match;
    check(bottom_ok, "bottom-up rows are flipped and 24-bit colours are preserved");
    if (!bottom_ok) {
        std::printf("     decoded: ");
        for (size_t i = 0; i < bottom_decoded.size(); ++i) std::printf("%02X", bottom_decoded[i]);
        std::printf("\n     expected BGR: ");
        for (size_t i = 0; i < 9; ++i) std::printf("%02X", bottom_pixels[kBottomStride + i]);
        std::printf("\n");
    }

    std::vector<uint8_t> empty_dib;
    check(!chast::encode_dib_png(empty_dib, dib_png, &error) && dib_png.empty(),
          "an empty DIB is refused");

    std::vector<uint8_t> empty;
    check(!chast::encode_png_bgra(nullptr, 1, 1, 4, empty, &error) && empty.empty() &&
              !error.empty(),
          "null pixels are not encoded");
    check(!chast::encode_png_bgra(source.data(), 0, 1, 4, empty, &error) && !error.empty(),
          "a zero width image is not encoded");
    check(!chast::encode_png_bgra(source.data(), kWidth, kHeight, kWidth * 4 - 1, empty,
                                  &error) && !error.empty(),
          "a short stride is not encoded");
    check(!chast::write_png_file("", png.data(), png.size(), &error) && !error.empty(),
          "an empty PNG file name is refused");
    check(!chast::write_png_file(path.string(), nullptr, 4, &error) && !error.empty(),
          "null PNG bytes are refused");
    check(!chast::write_png_file(path.string(), png.data(), 0, &error) && !error.empty(),
          "empty PNG bytes are refused");
}

void test_unicode_directory(const std::filesystem::path& directory) {
    const uint8_t source[4] = {0x7F, 0x40, 0x20, 0};
    const std::filesystem::path path = directory / L"截图" / L"shot.png";
    const std::u8string text = path.u8string();
    const std::string utf8_path(reinterpret_cast<const char*>(text.data()), text.size());
    std::string error;
    check(chast::write_png_bgra(utf8_path, source, 1, 1, 4, &error) && error.empty() &&
              std::filesystem::exists(path),
          "a UTF-8 path with a non-ASCII folder is written");

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> decoded;
    error.clear();
    check(decode_png(path, width, height, decoded, error) && width == 1 && height == 1,
          "the non-ASCII path decodes back");
}

void test_validation() {
    uint8_t pixel[4] = {1, 2, 3, 4};
    std::string error;
    check(!chast::write_png_bgra("", pixel, 1, 1, 4, &error) && !error.empty(),
          "an empty file name is refused");
    check(!chast::write_png_bgra("image.png", nullptr, 1, 1, 4, &error) && !error.empty(),
          "null pixels are refused");
    check(!chast::write_png_bgra("image.png", pixel, 0, 1, 4, &error) && !error.empty(),
          "a zero width is refused");
    check(!chast::write_png_bgra("image.png", pixel, 1, 0, 4, &error) && !error.empty(),
          "a zero height is refused");
    check(!chast::write_png_bgra("image.png", pixel, 2, 1, 4, &error) && !error.empty(),
          "a row shorter than the width is refused");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // The writer initialises COM itself; the decode helper below needs it on
    // this thread as well.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::filesystem::path directory = chast_test::scratch_directory("chast-png-test");
    std::error_code ignored;

    test_validation();
    if (!directory.empty()) {
        test_round_trip(directory);
        test_memory_encode(directory);
        test_unicode_directory(directory);
    } else {
        std::printf("FAIL could not create a temp directory\n");
        ++failures;
    }
    std::filesystem::remove_all(directory, ignored);
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }

    if (failures != 0) {
        std::printf("png writer checks FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("png writer checks passed\n");
    return 0;
}
